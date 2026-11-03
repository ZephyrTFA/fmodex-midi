/* Standard MIDI File parser. Portable C (no Windows headers) so it can be
 * tested and fuzzed natively. Input is untrusted (server-supplied): any
 * malformed file yields an error, never a crash. */
#ifndef SMF_H
#define SMF_H

#include <stddef.h>
#include <stdint.h>

#define SMF_MAX_INPUT   (8u << 20)   /* bytes */
#define SMF_MAX_EVENTS  (1u << 20)
#define SMF_MAX_SYSEX   (1u << 20)   /* total sysex bytes */

enum { SMF_EV_SHORT = 0, SMF_EV_SYSEX = 1 };

struct smf_event {
    int64_t  t_us;     /* absolute time at tempo ratio 1.0 */
    uint32_t tick;
    uint32_t msg;      /* SHORT: status | d1 << 8 | d2 << 16; SYSEX: offset into song->sysex */
    uint32_t len;      /* SHORT: message length (1..3); SYSEX: byte count */
    uint8_t  kind;
};

struct smf_song {
    struct smf_event *ev;
    uint32_t nev;
    uint8_t *sysex;
    uint32_t nsysex;
    int64_t length_us;   /* end of the longest track (End-of-Track time) */
    uint16_t format, ntracks, division;
};

/* Parse an SMF (or RIFF RMID wrapper). On success returns 0 and *out owns a
 * new song; on failure returns -1 and writes a reason into err. */
int smf_parse(const uint8_t *data, size_t len, struct smf_song **out, char *err, size_t errcap);
void smf_free(struct smf_song *song);

/* Index of the first event with t_us >= t (nev if none). */
uint32_t smf_find(const struct smf_song *song, int64_t t);

#endif
