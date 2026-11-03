#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "backend.h"
#include "player.h"
#include "log.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define LOOKAHEAD_US 500   /* events due within this window are sent together */
#define WAKE_EARLY_US 250  /* wake this long before the next deadline */

/* ---- songs ---------------------------------------------------------------- */

struct song_ref *song_ref_new(struct smf_song *song)
{
    struct song_ref *s = malloc(sizeof *s);
    if (s) {
        s->refs = 1;
        s->song = song;
    }
    return s;
}

void song_ref_get(struct song_ref *s) { InterlockedIncrement(&s->refs); }

void song_ref_put(struct song_ref *s)
{
    if (s && InterlockedDecrement(&s->refs) == 0) {
        smf_free(s->song);
        free(s);
    }
}

/* ---- commands --------------------------------------------------------------- */

enum { C_NEW, C_PAUSED, C_SEEK, C_GAIN, C_RATIO, C_LOOP, C_FREE, C_MARK, C_QUIT };

struct cmd {
    int op, id, i;
    double d, d2;
    int64_t t;
    struct song_ref *song;
    char text[64];
};

static SRWLOCK g_qlock = SRWLOCK_INIT;
static struct cmd *g_q;
static unsigned g_nq, g_capq;
static HANDLE g_wake, g_thread;
/* ENGINE_*; written under g_qlock so push() and the failure drain agree. */
static volatile LONG g_status = ENGINE_IDLE;

static void push(struct cmd c)
{
    AcquireSRWLockExclusive(&g_qlock);
    if (g_status != ENGINE_STARTING && g_status != ENGINE_UP) {
        ReleaseSRWLockExclusive(&g_qlock);
        if (c.op == C_NEW)
            song_ref_put(c.song);
        return;
    }
    if (g_nq == g_capq) {
        unsigned ncap = g_capq ? g_capq * 2 : 64;
        struct cmd *nq = realloc(g_q, ncap * sizeof *nq);
        if (!nq) {
            ReleaseSRWLockExclusive(&g_qlock);
            if (c.op == C_NEW)
                song_ref_put(c.song);
            return;
        }
        g_q = nq;
        g_capq = ncap;
    }
    g_q[g_nq++] = c;
    ReleaseSRWLockExclusive(&g_qlock);
    SetEvent(g_wake);
}

/* ---- voices (sequencer thread only, except the done flags) ----------------- */

struct voice {
    int id;
    int cut;              /* lost the device to a newer voice; stays silent */
    volatile LONG done;
    struct song_ref *song;
    struct player p;
};

static SRWLOCK g_vlock = SRWLOCK_INIT;   /* guards the array shape for engine_voice_done */
static struct voice **g_v;
static unsigned g_nv, g_capv;
static int g_owner = -1;                 /* id of the voice that owns the device */
static struct backend g_be;
static struct config g_cfgcopy;

static int64_t now_us(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER n;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&n);
    return n.QuadPart / freq.QuadPart * 1000000 + n.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart;
}

static struct voice *find_voice(int id)
{
    for (unsigned i = 0; i < g_nv; i++)
        if (g_v[i]->id == id)
            return g_v[i];
    return NULL;
}

static void remove_voice(int id)
{
    AcquireSRWLockExclusive(&g_vlock);
    for (unsigned i = 0; i < g_nv; i++) {
        if (g_v[i]->id == id) {
            struct voice *v = g_v[i];
            g_v[i] = g_v[--g_nv];
            ReleaseSRWLockExclusive(&g_vlock);
            song_ref_put(v->song);
            free(v);
            return;
        }
    }
    ReleaseSRWLockExclusive(&g_vlock);
}

static void cut_owner(int new_owner)
{
    if (g_owner >= 0 && g_owner != new_owner) {
        struct voice *o = find_voice(g_owner);
        if (o && (o->p.state == PL_PLAYING || o->p.state == PL_PAUSED)) {
            player_stop(&o->p);
            LOGI("engine: voice %d cut by voice %d", o->id, new_owner);
        }
        if (o) {
            o->cut = 1;
            InterlockedExchange(&o->done, 1);
        }
    }
    g_owner = new_owner;
}

static void run_cmd(struct cmd *c, int64_t now)
{
    struct voice *v = c->op == C_NEW || c->op == C_MARK || c->op == C_QUIT ? NULL : find_voice(c->id);
    if (!v && c->op != C_NEW && c->op != C_MARK && c->op != C_QUIT)
        return;

    switch (c->op) {
    case C_NEW: {
        if (find_voice(c->id))
            remove_voice(c->id);
        struct voice *nv = calloc(1, sizeof *nv);
        if (!nv) {
            song_ref_put(c->song);
            return;
        }
        nv->id = c->id;
        nv->song = c->song;   /* the command's reference moves to the voice */
        player_init(&nv->p, c->song->song,
                    (struct midi_out){ g_be.ctx, g_be.short_msg, g_be.long_msg },
                    g_cfgcopy.curve == CURVE_GM);
        player_set_loop(&nv->p, c->i);
        nv->p.gain = c->d;
        player_set_ratio(&nv->p, c->d2, now);
        AcquireSRWLockExclusive(&g_vlock);
        if (g_nv == g_capv) {
            unsigned ncap = g_capv ? g_capv * 2 : 16;
            struct voice **na = realloc(g_v, ncap * sizeof *na);
            if (!na) {
                ReleaseSRWLockExclusive(&g_vlock);
                song_ref_put(nv->song);
                free(nv);
                return;
            }
            g_v = na;
            g_capv = ncap;
        }
        g_v[g_nv++] = nv;
        ReleaseSRWLockExclusive(&g_vlock);
        break;
    }
    case C_PAUSED:
        if (v->cut)
            break;
        if (c->i) {
            player_pause(&v->p, now);
        } else if (v->p.state == PL_IDLE) {
            cut_owner(v->id);
            if (g_be.mark) {
                char m[64];
                _snprintf(m, sizeof m, "start voice %d", v->id);
                g_be.mark(g_be.ctx, m);
            }
            player_start(&v->p, v->p.anchor_pos, now);
            LOGI("engine: voice %d started at %ld ms", v->id, (long)(v->p.anchor_pos / 1000));
        } else if (v->p.state == PL_PAUSED) {
            player_resume(&v->p, now);
        }
        break;
    case C_SEEK:
        if (!v->cut)
            player_seek(&v->p, c->t, now);
        break;
    case C_GAIN:
        player_set_gain(&v->p, c->d);
        break;
    case C_RATIO:
        player_set_ratio(&v->p, c->d, now);
        break;
    case C_LOOP:
        player_set_loop(&v->p, c->i);
        break;
    case C_FREE:
        if (!v->cut)
            player_stop(&v->p);
        if (g_owner == v->id)
            g_owner = -1;
        remove_voice(v->id);
        break;
    case C_MARK:
        if (g_be.mark)
            g_be.mark(g_be.ctx, c->text);
        break;
    }
}

/* Drop every queued command (and the song references they hold). */
static void drain_queue(void)
{
    for (unsigned i = 0; i < g_nq; i++)
        if (g_q[i].op == C_NEW)
            song_ref_put(g_q[i].song);
    g_nq = 0;
}

static DWORD WINAPI seq_thread(LPVOID arg)
{
    (void)arg;
    /* Opening the device can take 100+ ms (OmniMIDI probe, winmm open), so
     * it happens here rather than on the host's thread. */
    int ok = backend_open(g_cfgcopy.backend, g_cfgcopy.winmm_device, g_cfgcopy.event_log, &g_be) == 0;
    AcquireSRWLockExclusive(&g_qlock);
    g_status = ok ? ENGINE_UP : ENGINE_FAILED;
    if (!ok)
        drain_queue();
    ReleaseSRWLockExclusive(&g_qlock);
    if (!ok) {
        LOGI("engine: no MIDI backend could be opened");
        return 1;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int hires = timer != NULL;
    if (!timer) {
        timeBeginPeriod(1);
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    LOGI("engine: sequencer thread up (high-resolution timer: %d)", hires);

    struct cmd *batch = NULL;
    unsigned capb = 0;
    int quit = 0;
    while (!quit) {
        unsigned nb;
        AcquireSRWLockExclusive(&g_qlock);
        nb = g_nq;
        if (nb > capb) {
            struct cmd *nbuf = realloc(batch, nb * sizeof *nbuf);
            if (nbuf) {
                batch = nbuf;
                capb = nb;
            } else {
                nb = capb;
            }
        }
        memcpy(batch, g_q, nb * sizeof *batch);
        memmove(g_q, g_q + nb, (g_nq - nb) * sizeof *g_q);
        g_nq -= nb;
        ReleaseSRWLockExclusive(&g_qlock);

        int64_t now = now_us();
        for (unsigned i = 0; i < nb; i++) {
            if (batch[i].op == C_QUIT)
                quit = 1;
            else
                run_cmd(&batch[i], now);
        }
        if (quit)
            break;

        now = now_us();
        int64_t next = PLAYER_NEVER;
        for (unsigned i = 0; i < g_nv; i++) {
            struct voice *v = g_v[i];
            if (v->p.state != PL_PLAYING)
                continue;
            int64_t d = player_service(&v->p, now, LOOKAHEAD_US);
            if (v->p.state == PL_DONE)
                InterlockedExchange(&v->done, 1);
            if (d < next)
                next = d;
        }

        if (next == PLAYER_NEVER) {
            WaitForSingleObject(g_wake, INFINITE);
            continue;
        }
        int64_t wait = next - WAKE_EARLY_US - now_us();
        if (wait <= 0)
            continue;
        LARGE_INTEGER due = { .QuadPart = -wait * 10 };   /* relative, 100 ns units */
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            HANDLE hs[2] = { g_wake, timer };
            WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        } else {
            WaitForSingleObject(g_wake, (DWORD)(wait / 1000));
        }
    }

    /* Shutdown: silence everything, drop all voices, close the device. */
    for (unsigned i = 0; i < g_nv; i++)
        if (!g_v[i]->cut)
            player_stop(&g_v[i]->p);
    while (g_nv)
        remove_voice(g_v[0]->id);
    /* Commands queued after QUIT may still hold song references. */
    AcquireSRWLockExclusive(&g_qlock);
    drain_queue();
    ReleaseSRWLockExclusive(&g_qlock);
    g_be.close(g_be.ctx);
    if (timer)
        CloseHandle(timer);
    if (!hires)
        timeEndPeriod(1);
    free(batch);
    LOGI("engine: sequencer thread down");
    return 0;
}

int engine_start(const struct config *cfg)
{
    if (g_status == ENGINE_STARTING || g_status == ENGINE_UP)
        return 0;
    if (!cfg->enabled || cfg->backend == BACKEND_OFF)
        return -1;
    g_cfgcopy = *cfg;
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_wake)
        return -1;
    g_status = ENGINE_STARTING;
    g_thread = CreateThread(NULL, 0, seq_thread, NULL, 0, NULL);
    if (!g_thread) {
        AcquireSRWLockExclusive(&g_qlock);
        g_status = ENGINE_FAILED;
        drain_queue();
        ReleaseSRWLockExclusive(&g_qlock);
        CloseHandle(g_wake);
        g_wake = NULL;
        return -1;
    }
    return 0;
}

void engine_shutdown(void)
{
    if (!g_thread)
        return;
    push((struct cmd){ .op = C_QUIT });   /* dropped if the thread already failed */
    WaitForSingleObject(g_thread, 5000);
    AcquireSRWLockExclusive(&g_qlock);
    g_status = ENGINE_IDLE;
    drain_queue();
    ReleaseSRWLockExclusive(&g_qlock);
    CloseHandle(g_thread);
    CloseHandle(g_wake);
    g_thread = g_wake = NULL;
    g_owner = -1;
}

int engine_status(void) { return (int)g_status; }

void engine_voice_new(int id, struct song_ref *song, int loop, double gain, double ratio)
{
    song_ref_get(song);
    push((struct cmd){ .op = C_NEW, .id = id, .i = loop, .d = gain, .d2 = ratio, .song = song });
}

void engine_voice_paused(int id, int paused) { push((struct cmd){ .op = C_PAUSED, .id = id, .i = paused }); }
void engine_voice_seek(int id, int64_t pos) { push((struct cmd){ .op = C_SEEK, .id = id, .t = pos }); }
void engine_voice_gain(int id, double g)     { push((struct cmd){ .op = C_GAIN, .id = id, .d = g }); }
void engine_voice_ratio(int id, double r)    { push((struct cmd){ .op = C_RATIO, .id = id, .d = r }); }
void engine_voice_loop(int id, int loop)     { push((struct cmd){ .op = C_LOOP, .id = id, .i = loop }); }
void engine_voice_free(int id)               { push((struct cmd){ .op = C_FREE, .id = id }); }

void engine_mark(const char *text)
{
    struct cmd c = { .op = C_MARK };
    lstrcpynA(c.text, text, sizeof c.text);
    push(c);
}

int engine_voice_done(int id)
{
    int r = 0;
    AcquireSRWLockShared(&g_vlock);
    for (unsigned i = 0; i < g_nv; i++)
        if (g_v[i]->id == id)
            r = g_v[i]->done != 0;
    ReleaseSRWLockShared(&g_vlock);
    return r;
}
