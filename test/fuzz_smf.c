/* libFuzzer target: parse arbitrary bytes as a MIDI file; if it parses, play
 * the whole song on a virtual clock, with a seek, pause/resume, ratio
 * change and loop wrap driven by the input's last bytes.
 * Build: see test/fuzz.sh. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "player.h"
#include "smf.h"

static unsigned long sink;
static void out_short(void *ctx, uint32_t m) { (void)ctx; sink += m; }
static void out_long(void *ctx, const uint8_t *d, uint32_t n) { (void)ctx; for (uint32_t i = 0; i < n; i++) sink += d[i]; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct smf_song *s;
    char err[128];
    if (smf_parse(data, size, &s, err, sizeof err))
        return 0;

    /* Event times must be non-decreasing and inside the song. */
    for (uint32_t i = 0; i < s->nev; i++) {
        if ((i && s->ev[i].t_us < s->ev[i - 1].t_us) || s->ev[i].t_us > s->length_us)
            __builtin_trap();
        if (s->ev[i].kind == SMF_EV_SYSEX && (uint64_t)s->ev[i].msg + s->ev[i].len > s->nsysex)
            __builtin_trap();
    }

    uint8_t k[4] = { 0 };
    memcpy(k, data + (size >= 4 ? size - 4 : 0), size >= 4 ? 4 : size);
    struct player p;
    player_init(&p, s, (struct midi_out){ NULL, out_short, out_long }, k[0] & 1);
    player_set_loop(&p, k[0] & 2);
    player_set_gain(&p, k[1] / 255.0);
    int64_t len = s->length_us > 0 ? s->length_us : 1;
    int64_t now = 0;
    player_start(&p, (int64_t)(k[2] % 8) * len / 8, now);

    /* Run to the end (bounded: loops forever otherwise), with a few
     * mid-song control changes. */
    int64_t stop_at = len * 3 + 1000000;
    if (stop_at > 4000000000LL)
        stop_at = 4000000000LL;
    for (int steps = 0; steps < 200000; steps++) {
        int64_t d = player_service(&p, now, 500);
        if (d == PLAYER_NEVER || d > stop_at)
            break;
        now = d > now ? d : now + 1;
        if (steps == 3) {
            player_pause(&p, now);
            player_resume(&p, now + 1000);
            now += 1000;
        } else if (steps == 7) {
            player_set_ratio(&p, 0.25 + (k[3] % 16) / 4.0, now);
        } else if (steps == 11) {
            player_seek(&p, (int64_t)(k[3] % 5) * len / 4, now);
        }
    }
    player_stop(&p);
    smf_free(s);
    return 0;
}
