#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smf.h"

/* Working event while parsing: sorted by (tick, track, seq), which is the
 * same merge order as mido's merge_tracks (stable sort by time, tracks in
 * file order). Tempo and end-of-track markers live here only to build the
 * tempo map; they are not emitted. */
enum { W_SHORT = SMF_EV_SHORT, W_SYSEX = SMF_EV_SYSEX, W_TEMPO, W_END };

struct wev {
    uint32_t tick;
    uint32_t seq;
    uint16_t track;
    uint8_t  kind;
    uint32_t msg;   /* short msg / sysex offset / tempo (us per quarter) */
    uint32_t len;
};

struct parser {
    struct wev *w;
    uint32_t nw, capw;
    uint8_t *sysex;
    uint32_t nsysex, capsysex;
    char *err;
    size_t errcap;
};

static int fail(struct parser *p, const char *msg)
{
    if (p->err && p->errcap)
        snprintf(p->err, p->errcap, "%s", msg);
    return -1;
}

static uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
static uint16_t be16(const uint8_t *b) { return (uint16_t)(b[0] << 8 | b[1]); }

static int push(struct parser *p, struct wev e)
{
    if (p->nw == p->capw) {
        if (p->capw >= SMF_MAX_EVENTS + 1024)
            return fail(p, "too many events");
        uint32_t ncap = p->capw ? p->capw * 2 : 1024;
        struct wev *nw = realloc(p->w, (size_t)ncap * sizeof *nw);
        if (!nw)
            return fail(p, "out of memory");
        p->w = nw;
        p->capw = ncap;
    }
    p->w[p->nw++] = e;
    return 0;
}

static int push_sysex(struct parser *p, const uint8_t *prefix, uint32_t nprefix,
                      const uint8_t *data, uint32_t n, uint32_t *off)
{
    uint32_t need = nprefix + n;
    if (need > SMF_MAX_SYSEX || p->nsysex > SMF_MAX_SYSEX - need)
        return fail(p, "too much sysex data");
    if (p->nsysex + need > p->capsysex) {
        uint32_t ncap = p->capsysex ? p->capsysex : 256;
        while (ncap < p->nsysex + need)
            ncap *= 2;
        uint8_t *ns = realloc(p->sysex, ncap);
        if (!ns)
            return fail(p, "out of memory");
        p->sysex = ns;
        p->capsysex = ncap;
    }
    *off = p->nsysex;
    memcpy(p->sysex + p->nsysex, prefix, nprefix);
    memcpy(p->sysex + p->nsysex + nprefix, data, n);
    p->nsysex += need;
    return 0;
}

/* Variable-length quantity, max 4 bytes (28 bits). */
static int vlq(const uint8_t **pp, const uint8_t *end, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*pp >= end)
            return -1;
        uint8_t b = *(*pp)++;
        v = v << 7 | (b & 0x7f);
        if (!(b & 0x80)) {
            *out = v;
            return 0;
        }
    }
    return -1;
}

static int parse_track(struct parser *p, const uint8_t *q, const uint8_t *end, uint16_t track)
{
    uint64_t tick = 0;
    uint32_t seq = 0;
    uint8_t running = 0;

    while (q < end) {
        uint32_t delta;
        if (vlq(&q, end, &delta))
            break;                      /* truncated: treat as end of track */
        tick += delta;
        if (tick > 0xffffffffu)
            return fail(p, "song too long (tick overflow)");
        if (q >= end)
            break;

        uint8_t st = *q;
        if (st < 0x80) {
            if (!running)
                return fail(p, "data byte without running status");
            st = running;
        } else {
            q++;
        }

        if (st < 0xf0) {
            int n = (st & 0xe0) == 0xc0 ? 1 : 2;   /* Cx / Dx have one data byte */
            if (end - q < n)
                break;
            uint8_t d1 = q[0], d2 = n == 2 ? q[1] : 0;
            if ((d1 | d2) & 0x80)
                return fail(p, "bad data byte in channel message");
            q += n;
            running = st;
            if (push(p, (struct wev){ (uint32_t)tick, seq++, track, W_SHORT,
                                      st | (uint32_t)d1 << 8 | (uint32_t)d2 << 16, (uint32_t)(n + 1) }))
                return -1;
        } else if (st == 0xff) {
            if (q >= end)
                break;
            uint8_t type = *q++;
            uint32_t n;
            if (vlq(&q, end, &n) || (uint32_t)(end - q) < n)
                break;
            if (type == 0x2f) {
                if (push(p, (struct wev){ (uint32_t)tick, seq++, track, W_END, 0, 0 }))
                    return -1;
                return 0;
            }
            if (type == 0x51 && n == 3) {
                uint32_t tempo = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
                if (tempo == 0)
                    tempo = 1;
                if (push(p, (struct wev){ (uint32_t)tick, seq++, track, W_TEMPO, tempo, 0 }))
                    return -1;
            }
            q += n;
        } else if (st == 0xf0 || st == 0xf7) {
            uint32_t n, off;
            if (vlq(&q, end, &n) || (uint32_t)(end - q) < n)
                break;
            /* F0 <len> <data...F7>: sent as F0 + data. F7 <len> <bytes>: an
             * escape, sent raw. Empty escapes are dropped. */
            if (st == 0xf0 || n) {
                uint8_t f0 = 0xf0;
                if (push_sysex(p, &f0, st == 0xf0, q, n, &off))
                    return -1;
                if (push(p, (struct wev){ (uint32_t)tick, seq++, track, W_SYSEX, off, n + (st == 0xf0) }))
                    return -1;
            }
            q += n;
        } else {
            return fail(p, "system common/realtime status in track data");
        }
    }
    /* No End-of-Track (or truncated): the track ends at its last event. */
    return push(p, (struct wev){ (uint32_t)tick, seq++, track, W_END, 0, 0 });
}

static int wev_cmp(const void *a, const void *b)
{
    const struct wev *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    if (x->track != y->track) return x->track < y->track ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* Strip a RIFF RMID wrapper down to its "data" chunk. */
static int unwrap_rmid(const uint8_t **data, size_t *len)
{
    const uint8_t *d = *data;
    size_t n = *len;
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "RMID", 4))
        return 0;
    size_t off = 12;
    while (off + 8 <= n) {
        uint32_t clen = (uint32_t)d[off + 4] | (uint32_t)d[off + 5] << 8 |
                        (uint32_t)d[off + 6] << 16 | (uint32_t)d[off + 7] << 24;
        size_t avail = n - off - 8;
        if (!memcmp(d + off, "data", 4)) {
            *data = d + off + 8;
            *len = clen < avail ? clen : avail;
            return 0;
        }
        if (clen > avail)
            break;
        off += 8 + (size_t)clen + (clen & 1);
    }
    return -1;
}

int smf_parse(const uint8_t *data, size_t len, struct smf_song **out, char *err, size_t errcap)
{
    struct parser p = { .err = err, .errcap = errcap };
    struct smf_song *song = NULL;
    *out = NULL;

    if (len > SMF_MAX_INPUT)
        return fail(&p, "file too large");
    if (unwrap_rmid(&data, &len))
        return fail(&p, "RMID without data chunk");
    if (len < 14 || memcmp(data, "MThd", 4))
        return fail(&p, "not a MIDI file");
    uint32_t hlen = be32(data + 4);
    if (hlen < 6 || hlen > len - 8)
        return fail(&p, "bad header length");
    uint16_t format = be16(data + 8), ntracks = be16(data + 10), division = be16(data + 12);
    if (format > 1)
        return fail(&p, "unsupported SMF format (only 0 and 1)");
    if (division == 0)
        return fail(&p, "zero division");

    /* SMPTE division: fixed microseconds per tick, tempo events ignored. */
    int smpte = (division & 0x8000) != 0;
    uint64_t smpte_num = 0, smpte_den = 1;
    if (smpte) {
        int fps = -(int8_t)(division >> 8);
        int tpf = division & 0xff;
        if (fps <= 0 || tpf == 0)
            return fail(&p, "bad SMPTE division");
        /* us per tick = 1e6 / (fps * tpf); 29 means 29.97 drop-frame */
        smpte_num = fps == 29 ? 100000000ull : 1000000ull;
        smpte_den = (uint64_t)tpf * (fps == 29 ? 2997u : (unsigned)fps);
    }

    size_t off = 8 + hlen;
    uint16_t track = 0;
    while (off + 8 <= len && track < ntracks) {
        uint32_t clen = be32(data + off + 4);
        size_t avail = len - off - 8;
        if (clen > avail)
            clen = (uint32_t)avail;       /* truncated file: parse what's there */
        if (!memcmp(data + off, "MTrk", 4)) {
            if (parse_track(&p, data + off + 8, data + off + 8 + clen, track))
                goto bad;
            track++;
        }
        off += 8 + (size_t)clen;
    }
    if (track == 0) {
        fail(&p, "no tracks");
        goto bad;
    }

    qsort(p.w, p.nw, sizeof *p.w, wev_cmp);

    song = calloc(1, sizeof *song);
    if (!song) {
        fail(&p, "out of memory");
        goto bad;
    }
    uint32_t nshort = 0;
    for (uint32_t i = 0; i < p.nw; i++)
        nshort += p.w[i].kind <= W_SYSEX;
    if (nshort > SMF_MAX_EVENTS) {
        fail(&p, "too many events");
        goto bad;
    }
    song->ev = malloc((size_t)(nshort ? nshort : 1) * sizeof *song->ev);
    if (!song->ev) {
        fail(&p, "out of memory");
        goto bad;
    }

    /* Tempo map: t = (base_num + (tick - base_tick) * tempo) / division,
     * all in integers so no rounding accumulates across tempo changes. */
    uint64_t base_num = 0, base_tick = 0, tempo = 500000;
    int64_t length = 0;
    for (uint32_t i = 0; i < p.nw; i++) {
        const struct wev *w = &p.w[i];
        int64_t t;
        if (smpte)
            t = (int64_t)((uint64_t)w->tick * smpte_num / smpte_den);
        else
            t = (int64_t)((base_num + (w->tick - base_tick) * tempo) / division);

        if (w->kind == W_TEMPO) {
            if (!smpte) {
                base_num += (w->tick - base_tick) * tempo;
                base_tick = w->tick;
                tempo = w->msg;
            }
        } else if (w->kind == W_END) {
            if (t > length)
                length = t;
        } else {
            song->ev[song->nev++] = (struct smf_event){ t, w->tick, w->msg, w->len, w->kind };
            if (t > length)
                length = t;
        }
    }

    song->length_us = length;
    song->format = format;
    song->ntracks = track;
    song->division = division;
    song->sysex = p.sysex;
    song->nsysex = p.nsysex;
    free(p.w);
    *out = song;
    return 0;

bad:
    if (song) {
        free(song->ev);
        free(song);
    }
    free(p.w);
    free(p.sysex);
    return -1;
}

void smf_free(struct smf_song *song)
{
    if (!song)
        return;
    free(song->ev);
    free(song->sysex);
    free(song);
}

uint32_t smf_find(const struct smf_song *song, int64_t t)
{
    uint32_t lo = 0, hi = song->nev;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (song->ev[mid].t_us < t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
