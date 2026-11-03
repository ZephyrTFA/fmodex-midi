#include <math.h>
#include <string.h>
#include "player.h"

#define GAIN_INTERVAL_US 20000   /* re-send CC7 at most ~50 Hz while fading */
#define CC7_DEFAULT 100
/* A looping song shorter than this loops with a gap instead: a zero-length
 * (or few-tick) song would otherwise wrap continuously and flood the synth
 * with resets. Untrusted input, found by fuzzing. */
#define MIN_LOOP_US 100000
#define MAX_RATIO 16.0

static void send(struct player *p, uint8_t st, uint8_t d1, uint8_t d2)
{
    p->out.short_msg(p->out.ctx, st | (uint32_t)d1 << 8 | (uint32_t)d2 << 16);
}

static uint8_t scale_cc7(const struct player *p, uint8_t v)
{
    double g = p->gain < 0 ? 0 : p->gain > 1 ? 1 : p->gain;
    double s = v * (p->gm_curve ? sqrt(g) : g);
    long r = lround(s);
    return (uint8_t)(r < 0 ? 0 : r > 127 ? 127 : r);
}

static uint8_t song_cc7(const struct chan_state *c)
{
    return c->cc_set[7] ? c->cc[7] : CC7_DEFAULT;
}

static void reset_shadow(struct chan_state *c)
{
    memset(c, 0, sizeof *c);
    c->bend = 8192;
    c->rpn_msb = c->rpn_lsb = 127;
}

/* Update the shadow state for a channel message (sent or chased). */
static void track(struct player *p, uint32_t msg)
{
    uint8_t st = msg & 0xff, d1 = (msg >> 8) & 0x7f, d2 = (msg >> 16) & 0x7f;
    struct chan_state *c = &p->ch[st & 0x0f];

    switch (st & 0xf0) {
    case 0x90:   /* velocity 0 is note off */
    case 0x80:
        if ((st & 0xf0) == 0x90 && d2)
            c->notes[d1 >> 5] |= 1u << (d1 & 31);
        else
            c->notes[d1 >> 5] &= ~(1u << (d1 & 31));
        break;
    case 0xc0:
        c->program = d1;
        c->program_set = 1;
        break;
    case 0xe0:
        c->bend = (uint16_t)(d1 | d2 << 7);
        break;
    case 0xb0:
        c->cc[d1] = d2;
        c->cc_set[d1] = 1;
        if (d1 == 101) c->rpn_msb = d2;
        else if (d1 == 100) c->rpn_lsb = d2;
        else if (d1 == 99 || d1 == 98) c->rpn_msb = c->rpn_lsb = 127;   /* NRPN: not chased */
        else if ((d1 == 6 || d1 == 38) && c->rpn_msb == 0 && c->rpn_lsb < 3) {
            c->rpn_val[c->rpn_lsb][d1 == 38] = d2;
            c->rpn_set[c->rpn_lsb] = 1;
        } else if (d1 == 120 || d1 == 123) {
            memset(c->notes, 0, sizeof c->notes);
        } else if (d1 == 121) {
            /* RP-015 reset: everything but bank, volume, pan, effects depth */
            for (int n = 1; n < 120; n++)
                if (n != 7 && n != 10 && n != 32 && n != 91 && n != 93)
                    c->cc_set[n] = 0;
            c->bend = 8192;
            c->rpn_msb = c->rpn_lsb = 127;
        }
        break;
    }
}

static void emit(struct player *p, const struct smf_event *e)
{
    if (e->kind == SMF_EV_SYSEX) {
        p->out.long_msg(p->out.ctx, p->song->sysex + e->msg, e->len);
        return;
    }
    uint32_t msg = e->msg;
    track(p, msg);
    if ((msg & 0xf0) == 0xb0 && ((msg >> 8) & 0xff) == 7)
        msg = (msg & 0xffff) | (uint32_t)scale_cc7(p, (msg >> 16) & 0x7f) << 16;
    p->out.short_msg(p->out.ctx, msg);
}

static void notes_off(struct player *p)
{
    for (int ch = 0; ch < 16; ch++) {
        struct chan_state *c = &p->ch[ch];
        for (int k = 0; k < 128; k++)
            if (c->notes[k >> 5] & (1u << (k & 31)))
                send(p, (uint8_t)(0x80 | ch), (uint8_t)k, 0);
        memset(c->notes, 0, sizeof c->notes);
        send(p, (uint8_t)(0xb0 | ch), 64, 0);    /* sustain off */
        send(p, (uint8_t)(0xb0 | ch), 123, 0);   /* all notes off */
    }
}

static void send_gain(struct player *p, int64_t now)
{
    for (int ch = 0; ch < 16; ch++)
        send(p, (uint8_t)(0xb0 | ch), 7, scale_cc7(p, song_cc7(&p->ch[ch])));
    p->gain_dirty = 0;
    p->gain_sent_wall = now;
}

/* Bring a synth that may hold anything into a GM power-on-ish state. */
static void reset_device(struct player *p)
{
    for (int ch = 0; ch < 16; ch++) {
        uint8_t b = (uint8_t)(0xb0 | ch);
        send(p, b, 120, 0);
        send(p, b, 121, 0);
        send(p, b, 0, 0);
        send(p, b, 32, 0);
        send(p, (uint8_t)(0xc0 | ch), 0, 0);
        send(p, b, 10, 64);
        send(p, (uint8_t)(0xe0 | ch), 0, 64);
    }
}

/* Re-send the shadow state (after pause or seek). */
static void resend_state(struct player *p, int64_t now)
{
    for (int ch = 0; ch < 16; ch++) {
        struct chan_state *c = &p->ch[ch];
        uint8_t b = (uint8_t)(0xb0 | ch);
        if (c->cc_set[0]) send(p, b, 0, c->cc[0]);
        if (c->cc_set[32]) send(p, b, 32, c->cc[32]);
        if (c->program_set) send(p, (uint8_t)(0xc0 | ch), c->program, 0);
        for (int n = 1; n < 120; n++) {
            if (!c->cc_set[n] || n == 7 || n == 32 || n == 6 || n == 38 || (n >= 96 && n <= 101))
                continue;
            send(p, b, (uint8_t)n, c->cc[n]);
        }
        for (int r = 0; r < 3; r++) {
            if (!c->rpn_set[r])
                continue;
            send(p, b, 101, 0);
            send(p, b, 100, (uint8_t)r);
            send(p, b, 6, c->rpn_val[r][0]);
            send(p, b, 38, c->rpn_val[r][1]);
        }
        send(p, b, 101, c->rpn_msb);
        send(p, b, 100, c->rpn_lsb);
        send(p, (uint8_t)(0xe0 | ch), c->bend & 0x7f, c->bend >> 7);
    }
    send_gain(p, now);
}

/* Silently replay state up to event index `upto`; SysEx is sent (it is
 * usually a reset/setup message that the rest of the song depends on). */
static void chase(struct player *p, uint32_t upto)
{
    for (int ch = 0; ch < 16; ch++)
        reset_shadow(&p->ch[ch]);
    for (uint32_t i = 0; i < upto; i++) {
        const struct smf_event *e = &p->song->ev[i];
        if (e->kind == SMF_EV_SYSEX)
            p->out.long_msg(p->out.ctx, p->song->sysex + e->msg, e->len);
        else
            track(p, e->msg);
    }
    for (int ch = 0; ch < 16; ch++)
        memset(p->ch[ch].notes, 0, sizeof p->ch[ch].notes);   /* chased notes never sound */
}

static int64_t wall_of(const struct player *p, int64_t t)
{
    if (p->ratio <= 1e-6)
        return PLAYER_NEVER;
    double w = (double)p->anchor_wall + (double)(t - p->anchor_pos) / p->ratio;
    return w >= 9.2e18 ? PLAYER_NEVER : (int64_t)llround(w);
}

void player_init(struct player *p, const struct smf_song *song, struct midi_out out, int gm_curve)
{
    memset(p, 0, sizeof *p);
    p->song = song;
    p->out = out;
    p->ratio = 1.0;
    p->gain = 1.0;
    p->gm_curve = gm_curve;
    for (int ch = 0; ch < 16; ch++)
        reset_shadow(&p->ch[ch]);
}

int64_t player_position(const struct player *p, int64_t now)
{
    if (p->state != PL_PLAYING)
        return p->anchor_pos;
    int64_t pos = p->anchor_pos + (int64_t)llround((double)(now - p->anchor_wall) * p->ratio);
    return pos < p->anchor_pos ? p->anchor_pos : pos;
}

void player_start(struct player *p, int64_t pos_us, int64_t now)
{
    reset_device(p);
    if (pos_us < 0)
        pos_us = 0;
    if (p->song->length_us > 0 && pos_us >= p->song->length_us)
        pos_us = p->loop ? pos_us % p->song->length_us : p->song->length_us;
    p->idx = smf_find(p->song, pos_us);
    if (p->idx > 0) {
        chase(p, p->idx);
        resend_state(p, now);
    } else {
        send_gain(p, now);
    }
    p->anchor_pos = pos_us;
    p->anchor_wall = now;
    p->state = PL_PLAYING;
}

int64_t player_service(struct player *p, int64_t now, int64_t lookahead_us)
{
    if (p->state != PL_PLAYING)
        return PLAYER_NEVER;

    int64_t horizon = now + lookahead_us;
    int64_t gain_deadline = PLAYER_NEVER;
    if (p->gain_dirty) {
        if (now - p->gain_sent_wall >= GAIN_INTERVAL_US)
            send_gain(p, now);
        else
            gain_deadline = p->gain_sent_wall + GAIN_INTERVAL_US;
    }

    const struct smf_song *s = p->song;
    int wrapped = 0;
    for (;;) {
        while (p->idx < s->nev) {
            int64_t w = wall_of(p, s->ev[p->idx].t_us);
            if (w > horizon)
                return w < gain_deadline ? w : gain_deadline;
            emit(p, &s->ev[p->idx++]);
        }
        int64_t len = p->loop && s->length_us < MIN_LOOP_US ? MIN_LOOP_US : s->length_us;
        int64_t end = wall_of(p, len);
        if (end > horizon || (wrapped && p->loop))
            return end < gain_deadline ? end : gain_deadline;
        if (p->loop) {
            wrapped = 1;
            /* Wrap with no gap: silence what's sounding, reset controllers;
             * the song's tick-0 events re-establish its start state. */
            notes_off(p);
            for (int ch = 0; ch < 16; ch++) {
                send(p, (uint8_t)(0xb0 | ch), 121, 0);
                reset_shadow(&p->ch[ch]);
            }
            send_gain(p, now);
            p->anchor_wall = end;
            p->anchor_pos = 0;
            p->idx = 0;
            continue;
        }
        notes_off(p);
        p->anchor_pos = s->length_us;
        p->state = PL_DONE;
        return PLAYER_NEVER;
    }
}

void player_pause(struct player *p, int64_t now)
{
    if (p->state != PL_PLAYING)
        return;
    p->anchor_pos = player_position(p, now);
    p->state = PL_PAUSED;
    notes_off(p);
}

void player_resume(struct player *p, int64_t now)
{
    if (p->state != PL_PAUSED)
        return;
    p->anchor_wall = now;
    p->state = PL_PLAYING;
    resend_state(p, now);
}

void player_seek(struct player *p, int64_t pos_us, int64_t now)
{
    if (p->state == PL_IDLE || p->state == PL_DONE) {
        p->anchor_pos = pos_us < 0 ? 0 : pos_us;
        return;
    }
    if (pos_us < 0)
        pos_us = 0;
    if (p->song->length_us > 0 && pos_us > p->song->length_us)
        pos_us = p->loop ? pos_us % p->song->length_us : p->song->length_us;
    if (p->state == PL_PLAYING)
        notes_off(p);
    p->idx = smf_find(p->song, pos_us);
    chase(p, p->idx);
    p->anchor_pos = pos_us;
    p->anchor_wall = now;
    if (p->state == PL_PLAYING)
        resend_state(p, now);
}

void player_set_ratio(struct player *p, double ratio, int64_t now)
{
    p->anchor_pos = player_position(p, now);
    p->anchor_wall = now;
    p->ratio = ratio < 0 ? 0 : ratio > MAX_RATIO ? MAX_RATIO : ratio;
}

void player_set_gain(struct player *p, double gain)
{
    if (gain != p->gain) {
        p->gain = gain;
        p->gain_dirty = 1;
    }
}

void player_set_loop(struct player *p, int loop)
{
    p->loop = loop != 0;
}

void player_stop(struct player *p)
{
    if (p->state == PL_IDLE)
        return;
    notes_off(p);
    for (int ch = 0; ch < 16; ch++) {
        send(p, (uint8_t)(0xb0 | ch), 120, 0);
        send(p, (uint8_t)(0xb0 | ch), 121, 0);
    }
    p->state = PL_DONE;
}
