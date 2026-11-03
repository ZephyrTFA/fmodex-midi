/* One voice playing one song onto a MIDI output. Portable and clock-agnostic:
 * the caller passes the current wall time (us) into every call and sleeps
 * until the deadline player_service() returns. Not thread-safe; the
 * sequencer thread owns all players. */
#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include "smf.h"

#define PLAYER_NEVER INT64_MAX

struct midi_out {
    void *ctx;
    void (*short_msg)(void *ctx, uint32_t msg);
    void (*long_msg)(void *ctx, const uint8_t *data, uint32_t len);
};

enum player_state { PL_IDLE, PL_PLAYING, PL_PAUSED, PL_DONE };

struct chan_state {
    uint8_t cc[128];
    uint8_t cc_set[128];       /* 1 if the song has set cc[n] */
    uint8_t program, program_set;
    uint16_t bend;             /* 14-bit, 8192 = centre */
    uint8_t rpn_msb, rpn_lsb;  /* currently selected RPN (127/127 = null) */
    uint8_t rpn_val[3][2];     /* data entry for RPN 0 (bend range), 1, 2 */
    uint8_t rpn_set[3];
    uint32_t notes[4];         /* sounding notes, bit per key */
};

struct player {
    const struct smf_song *song;
    struct midi_out out;
    enum player_state state;
    uint32_t idx;              /* next event */
    int loop;
    double ratio;              /* tempo ratio, 1.0 = as written */
    int64_t anchor_wall;       /* wall time at which song time == anchor_pos */
    int64_t anchor_pos;
    double gain;               /* linear amplitude, 0..1+ */
    int gm_curve;              /* 1: cc * sqrt(gain), 0: cc * gain */
    int gain_dirty;
    int64_t gain_sent_wall;
    struct chan_state ch[16];
};

void player_init(struct player *p, const struct smf_song *song, struct midi_out out, int gm_curve);
/* Start at song position pos_us (us at ratio 1.0). */
void player_start(struct player *p, int64_t pos_us, int64_t now);
/* Send everything due by now (+ lookahead); returns the next deadline. */
int64_t player_service(struct player *p, int64_t now, int64_t lookahead_us);
void player_pause(struct player *p, int64_t now);
void player_resume(struct player *p, int64_t now);
void player_seek(struct player *p, int64_t pos_us, int64_t now);
void player_set_ratio(struct player *p, double ratio, int64_t now);
void player_set_gain(struct player *p, double gain);   /* applied by service, rate-limited */
void player_set_loop(struct player *p, int loop);
/* Notes off + sustain off + reset controllers on all channels. */
void player_stop(struct player *p);
/* Current song position (us at ratio 1.0). */
int64_t player_position(const struct player *p, int64_t now);

#endif
