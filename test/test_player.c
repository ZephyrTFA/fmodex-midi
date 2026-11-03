/* Native unit tests for the player against a virtual clock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "player.h"

#define MAXOUT 100000
static struct { int64_t t; uint32_t msg; int sysex; } out[MAXOUT];
static int nout;
static int64_t g_now;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void cap_short(void *ctx, uint32_t m) { (void)ctx; if (nout < MAXOUT) out[nout++] = (typeof(out[0])){ g_now, m, 0 }; }
static void cap_long(void *ctx, const uint8_t *d, uint32_t n) { (void)ctx; (void)n; if (nout < MAXOUT) out[nout++] = (typeof(out[0])){ g_now, d[0], 1 }; }

static struct smf_song *load(const char *path)
{
    static unsigned char buf[1 << 20];
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    struct smf_song *s;
    char err[128];
    if (smf_parse(buf, n, &s, err, sizeof err)) { printf("parse %s: %s\n", path, err); exit(2); }
    return s;
}

/* Run the player on the virtual clock until `until`, jumping to each deadline. */
static void run(struct player *p, int64_t until)
{
    for (;;) {
        int64_t d = player_service(p, g_now, 0);
        if (d == PLAYER_NEVER || d > until) { g_now = until; return; }
        g_now = d > g_now ? d : g_now;
    }
}

static int find(uint32_t msg, int from)
{
    for (int i = from; i < nout; i++)
        if (!out[i].sysex && out[i].msg == msg)
            return i;
    return -1;
}

static struct midi_out cap = { NULL, cap_short, cap_long };
#define NOTE_ON  0x643090u   /* 90 30 64 */
#define NOTE_OFF 0x403080u   /* 80 30 40 */

static void test_basic(struct smf_song *s)
{
    struct player p;
    nout = 0; g_now = 1000;
    player_init(&p, s, cap, 1);
    player_start(&p, 0, g_now);
    int pre = nout;
    run(&p, 10000000);
    int on = find(NOTE_ON, pre), off = find(NOTE_OFF, pre);
    CHECK(on >= 0 && out[on].t == 1000, "note on at %lld", on >= 0 ? (long long)out[on].t : -1LL);
    CHECK(off >= 0 && out[off].t == 1000 + 461538, "note off at %lld", off >= 0 ? (long long)out[off].t : -1LL);
    CHECK(p.state == PL_DONE, "state %d", p.state);
    /* every song event is emitted exactly once, in order, at its time */
    int k = pre;
    for (uint32_t i = 0; i < s->nev; i++, k++) {
        CHECK(k < nout && out[k].msg == s->ev[i].msg && out[k].t == 1000 + s->ev[i].t_us,
              "event %u mismatch", i);
    }
}

static void test_ratio(struct smf_song *s)
{
    struct player p;
    nout = 0; g_now = 0;
    player_init(&p, s, cap, 1);
    player_set_ratio(&p, 2.0, 0);
    player_start(&p, 0, 0);
    run(&p, 10000000);
    int off = find(NOTE_OFF, 0);
    CHECK(off >= 0 && out[off].t == 230769, "ratio 2 note off at %lld", off >= 0 ? (long long)out[off].t : -1LL);
}

static void test_pause(struct smf_song *s)
{
    struct player p;
    nout = 0; g_now = 0;
    player_init(&p, s, cap, 1);
    player_start(&p, 0, 0);
    run(&p, 200000);
    int mark = nout;
    player_pause(&p, g_now);
    CHECK(find(0x003080u, mark) >= 0, "pause must send explicit note off for the sounding note");
    CHECK(find(0x0040b0u, mark) >= 0, "pause must send sustain off");
    CHECK(player_position(&p, 900000) == 200000, "paused position %lld", (long long)player_position(&p, 900000));
    g_now = 1200000;
    player_resume(&p, g_now);
    run(&p, 10000000);
    int off = find(NOTE_OFF, mark);
    CHECK(off >= 0 && out[off].t == 1461538, "resumed note off at %lld", off >= 0 ? (long long)out[off].t : -1LL);
}

static void test_seek(struct smf_song *s)
{
    struct player p;
    nout = 0; g_now = 0;
    player_init(&p, s, cap, 1);
    player_start(&p, 1000000, 0);
    /* program 19 and CC7=90 (scaled by gain 1 -> 90) chased before playing */
    int prog = find(0x0013c0u, 0), vol = find(0x5a07b0u, 0);
    CHECK(prog >= 0 && vol >= 0, "chased program %d / volume %d", prog, vol);
    /* after the chase, output continues exactly with the first event at or
     * after 1s, at wall time (t - 1s) */
    int pre = nout;
    uint32_t first = smf_find(s, 1000000);
    CHECK(first > 0 && first < s->nev, "seek target in range");
    run(&p, 100000000);
    int k = pre;
    for (uint32_t i = first; i < s->nev; i++, k++)
        CHECK(k < nout && out[k].msg == s->ev[i].msg && out[k].t == s->ev[i].t_us - 1000000,
              "post-seek event %u mismatch", i);
    CHECK(p.state == PL_DONE, "seek playback finished");
}

static void test_loop(struct smf_song *s)
{
    struct player p;
    nout = 0; g_now = 0;
    player_init(&p, s, cap, 1);
    player_set_loop(&p, 1);
    player_start(&p, 0, 0);
    run(&p, 2 * s->length_us + 100);
    int on1 = find(NOTE_ON, 0), on2 = on1 >= 0 ? find(NOTE_ON, on1 + 1) : -1;
    int on3 = on2 >= 0 ? find(NOTE_ON, on2 + 1) : -1;
    CHECK(on2 >= 0 && out[on2].t == s->length_us, "loop 2nd note at %lld", on2 >= 0 ? (long long)out[on2].t : -1LL);
    CHECK(on3 >= 0 && out[on3].t == 2 * s->length_us, "loop 3rd note at %lld", on3 >= 0 ? (long long)out[on3].t : -1LL);
    CHECK(p.state == PL_PLAYING, "looping voice still playing");
}

static void test_gain(struct smf_song *s)
{
    struct player p;
    nout = 0; g_now = 0;
    player_init(&p, s, cap, 1);
    player_start(&p, 0, 0);
    player_set_gain(&p, 0.25);   /* gm curve: 100 * sqrt(.25) = 50 */
    g_now = 30000;
    player_service(&p, g_now, 0);
    CHECK(find(0x3207b0u, 0) >= 0, "CC7 100 -> 50 at gain 0.25");
    int mark = nout;
    player_set_gain(&p, 0.0);
    g_now = 35000;
    int64_t d = player_service(&p, g_now, 0);
    CHECK(find(0x0007b0u, mark) < 0, "gain change within 20ms must be deferred");
    CHECK(d == 50000, "deferred gain deadline %lld", (long long)d);
    g_now = 50000;
    player_service(&p, g_now, 0);
    CHECK(find(0x0007b0u, mark) >= 0, "deferred gain sent");
}

static void test_stop_no_stuck_notes(struct smf_song *s)
{
    struct player p;
    nout = 0; g_now = 0;
    player_init(&p, s, cap, 1);
    player_start(&p, 0, 0);
    run(&p, 100000);
    int mark = nout;
    player_stop(&p);
    CHECK(find(0x003080u, mark) >= 0, "stop sends note off");
    CHECK(find(0x007bb0u, mark) >= 0, "stop sends all notes off");
    CHECK(player_service(&p, 200000, 0) == PLAYER_NEVER, "stopped player is silent");
}

/* Fuzzer find: a looping ~15 us song (1 tick at 32767 ppq) must not spin. */
static void test_tiny_loop(void)
{
    static const unsigned char eot_only[] = "MThd\0\0\0\6\0\0\0\1\x7f\xff" "MTrk\0\0\0\4\1\xff\x2f\0";
    struct smf_song *s;
    char err[64];
    CHECK(smf_parse(eot_only, sizeof eot_only - 1, &s, err, sizeof err) == 0, "parse eot-only: %s", err);
    struct player p;
    nout = 0; g_now = 0;
    player_init(&p, s, cap, 1);
    player_set_loop(&p, 1);
    player_start(&p, 0, 0);
    int calls = 0;
    for (; calls < 100000 && g_now < 1000000; calls++) {
        int64_t d = player_service(&p, g_now, 500);
        if (d == PLAYER_NEVER) break;
        g_now = d > g_now ? d : g_now + 1;
    }
    int resets = 0;
    for (int i = 0; i < nout; i++)
        resets += out[i].msg == 0x0079b0u;   /* B0 79 00 (CC121 ch0) */
    CHECK(resets <= 12, "tiny loop wrapped %d times in 1 s", resets);
    CHECK(calls < 100, "tiny loop took %d service calls for 1 s", calls);
    smf_free(s);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: test_player piano.mid tempo_changes.mid\n"); return 2; }
    struct smf_song *piano = load(argv[1]), *tempo = load(argv[2]);
    test_basic(piano);
    test_ratio(piano);
    test_pause(piano);
    test_seek(tempo);
    test_loop(piano);
    test_gain(piano);
    test_stop_no_stuck_notes(piano);
    test_tiny_loop();
    smf_free(piano);
    smf_free(tempo);
    printf(failures ? "test_player: %d failures\n" : "test_player: ok\n", failures);
    return failures != 0;
}
