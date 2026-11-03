/* Hooked FMOD Ex exports. Every function here is __stdcall with `this` as the
 * first argument, matching how MSVC-built hosts call the FMOD Ex C++ API
 * (docs/design.md). Each one calls the real function in
 * fmodex_real.dll; the forwarded exports never reach this file.
 *
 * Strategy (design §3.2): FMOD keeps playing every MIDI channel, muted, so
 * the host's END callbacks, isPlaying, channel reuse etc. stay stock. We mirror
 * the channel's state onto a voice in our sequencer.
 *
 * Locking: g_lock guards the sound/channel tables. Never call a real FMOD
 * function while holding it: channel callbacks run inside System::update and
 * the host may re-enter these hooks from them. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "fmod_min.h"
#include "hooks_gen.h"
#include "config.h"
#include "engine.h"
#include "log.h"
#include "smf.h"

/* ---- real function table --------------------------------------------- */

#define X(sym, name) static void *real_##sym;
FMOD_HOOKS(X)
#undef X

/* Not hooked (hosts rarely call them); we call them ourselves. */
static FMOD_RESULT (__stdcall *real_Sound_getFormat)(void *, int *, int *, int *, int *);
static FMOD_RESULT (__stdcall *real_Channel_getFrequency)(void *, float *);

static HMODULE g_self;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static int g_ready;

static BOOL CALLBACK init_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    config_load(g_self);
    log_open(g_cfg.log_file, g_cfg.log_level);

    /* Hosts may delay-load fmodex.dll, and fmodex_real.dll is only mapped
     * once a forwarded export gets resolved. Don't rely on that order: load
     * it from our own directory (we're never under the loader lock here). */
    HMODULE real = GetModuleHandleW(L"fmodex_real.dll");
    if (!real) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
        wchar_t *slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : NULL;
        if (slash && (size_t)(slash - path) + 17 < MAX_PATH) {
            lstrcpyW(slash + 1, L"fmodex_real.dll");
            real = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
    }
    if (!real) {
        LOGI("fmodex_real.dll could not be loaded; every hook will fail");
        return TRUE;
    }
    int missing = 0;
#define X(sym, name) \
    if (!(real_##sym = (void *)GetProcAddress(real, name))) { LOGI("missing real export %s", name); missing++; }
    FMOD_HOOKS(X)
#undef X
    real_Sound_getFormat = (void *)GetProcAddress(real,
        "?getFormat@Sound@FMOD@@QAG?AW4FMOD_RESULT@@PAW4FMOD_SOUND_TYPE@@PAW4FMOD_SOUND_FORMAT@@PAH2@Z");
    real_Channel_getFrequency = (void *)GetProcAddress(real,
        "?getFrequency@Channel@FMOD@@QAG?AW4FMOD_RESULT@@PAM@Z");

    LOGI("fmodex-midi proxy ready (real=%p, missing=%d, enabled=%d, backend=%d)",
         (void *)real, missing, g_cfg.enabled, (int)g_cfg.backend);
    g_ready = 1;
    return TRUE;
}

static inline void ensure_init(void)
{
    if (!g_ready)
        InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
}

/* Call the real function `sym` with prototype `proto`; fail cleanly if it
 * could not be resolved rather than jumping through NULL. */
#define REAL(sym, proto) ((FMOD_RESULT (__stdcall *) proto)real_##sym)
#define CALL_REAL(sym, proto, ...) \
    (real_##sym ? REAL(sym, proto)(__VA_ARGS__) : FMOD_ERR_INTERNAL)

#define real_setMute(ch, m)      CALL_REAL(Channel_setMute, (void *, int), ch, m)
#define real_isPlaying(ch, out)  CALL_REAL(Channel_isPlaying, (void *, unsigned char *), ch, out)

/* ---- state -------------------------------------------------------------- */

struct midi_sound {
    void *sound;
    struct song_ref *song;
    int loop;
};

struct midi_channel {
    void *channel;
    void *sound;
    int id;              /* engine voice id */
    int paused;          /* the FMOD channel's own paused flag */
    int desired_mute;    /* what the host asked for; FMOD always gets mute=true */
    float volume;
    float f0, freq;      /* frequency at playSound, current frequency */
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static struct midi_sound *g_sounds;
static unsigned g_nsounds, g_capsounds;
static struct midi_channel *g_chans;
static unsigned g_nchans, g_capchans;
static int g_next_id = 1;

static void *g_master;
static float g_master_volume = 1.0f, g_master_pitch = 1.0f;
static int g_master_mute, g_master_paused;

/* 0 = not tried, 1 = running, -1 = failed or disabled (stock everywhere) */
static int g_engine_state;

static int grow(void **arr, unsigned *cap, size_t elem)
{
    unsigned ncap = *cap ? *cap * 2 : 16;
    void *p = realloc(*arr, ncap * elem);
    if (!p)
        return 0;
    *arr = p;
    *cap = ncap;
    return 1;
}

static int sound_index(void *sound)
{
    for (unsigned i = 0; i < g_nsounds; i++)
        if (g_sounds[i].sound == sound)
            return (int)i;
    return -1;
}

static int chan_index(void *ch)
{
    for (unsigned i = 0; i < g_nchans; i++)
        if (g_chans[i].channel == ch)
            return (int)i;
    return -1;
}

/* Effective values pushed to the engine (caller holds g_lock). */
static double eff_gain(const struct midi_channel *c)
{
    if (c->desired_mute || g_master_mute)
        return 0.0;
    return (double)c->volume * (double)g_master_volume;
}

static double eff_ratio(const struct midi_channel *c)
{
    double r = c->f0 > 0 ? (double)c->freq / (double)c->f0 : 1.0;
    return r * (double)g_master_pitch;
}

static int eff_paused(const struct midi_channel *c)
{
    return c->paused || g_master_paused;
}

/* Drop channel entry i and its voice (caller holds g_lock). */
static void chan_drop(unsigned i)
{
    engine_voice_free(g_chans[i].id);
    g_chans[i] = g_chans[--g_nchans];
}

/* Look up a MIDI channel; on success returns with g_lock held exclusively. */
static struct midi_channel *chan_lock(void *ch)
{
    AcquireSRWLockExclusive(&g_lock);
    int i = chan_index(ch);
    if (i >= 0)
        return &g_chans[i];
    ReleaseSRWLockExclusive(&g_lock);
    return NULL;
}

static void unlock(void) { ReleaseSRWLockExclusive(&g_lock); }

/* Is this buffer a Standard MIDI File (raw or RIFF RMID wrapped)? */
static int looks_like_midi(const unsigned char *p, unsigned len)
{
    if (len >= 14 && !memcmp(p, "MThd", 4))
        return 1;
    if (len >= 20 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "RMID", 4))
        return 1;
    return 0;
}

/* The backend failed to open (asynchronously): give every mirrored channel
 * back to FMOD, unmuted as the host wanted, and stop treating sounds as MIDI. */
static void fall_back_to_stock(void)
{
    g_engine_state = -1;
    LOGI("MIDI output unavailable (enabled=%d backend=%d): stock FMOD playback",
         g_cfg.enabled, (int)g_cfg.backend);
    AcquireSRWLockExclusive(&g_lock);
    unsigned n = g_nchans;
    struct midi_channel *chs = n ? malloc(n * sizeof *chs) : NULL;
    if (chs)
        memcpy(chs, g_chans, n * sizeof *chs);
    else
        n = 0;
    g_nchans = 0;
    for (unsigned i = 0; i < g_nsounds; i++)
        song_ref_put(g_sounds[i].song);
    g_nsounds = 0;
    ReleaseSRWLockExclusive(&g_lock);
    for (unsigned i = 0; i < n; i++)
        real_setMute(chs[i].channel, chs[i].desired_mute);
    free(chs);
}

/* Start the engine on first use; 1 while it's starting or up. */
static int engine_up(void)
{
    if (g_engine_state == 0) {
        g_engine_state = engine_start(&g_cfg) == 0 ? 1 : -1;
        if (g_engine_state < 0)
            LOGI("MIDI output disabled (enabled=%d backend=%d): stock FMOD playback",
                 g_cfg.enabled, (int)g_cfg.backend);
    }
    if (g_engine_state > 0 && engine_status() == ENGINE_FAILED)
        fall_back_to_stock();
    return g_engine_state > 0;
}

/* Escape hatch: give a channel back to stock FMOD playback. */
static void escape(void *ch, const char *why)
{
    int mute = -1;
    struct midi_channel *c = chan_lock(ch);
    if (c) {
        mute = c->desired_mute;
        chan_drop((unsigned)(c - g_chans));
        unlock();
    }
    if (mute >= 0) {
        real_setMute(ch, mute);
        LOGI("channel %p: back to stock FMOD (%s)", ch, why);
    }
}

#define LOGCH(midi, ...) do { \
    if (g_log_level >= LOG_TRACE || (g_log_level >= LOG_INFO && (midi))) \
        log_write(__VA_ARGS__); \
} while (0)

/* ---- System ------------------------------------------------------------ */

FMOD_RESULT __stdcall hk_System_init(void *sys, int maxchannels, unsigned flags, void *extra)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(System_init, (void *, int, unsigned, void *), sys, maxchannels, flags, extra);
    LOGI("System::init(%p, maxch=%d, flags=0x%x, extra=%p) = %d", sys, maxchannels, flags, extra, r);
    return r;
}

/* Our tick on the host's thread: drop voices whose FMOD channel has ended. */
static void reap_channels(void)
{
    void *chs[64];
    unsigned n = 0, start = 0;
    for (;;) {
        AcquireSRWLockShared(&g_lock);
        unsigned total = g_nchans;
        n = 0;
        for (unsigned i = start; i < total && n < 64; i++)
            chs[n++] = g_chans[i].channel;
        ReleaseSRWLockShared(&g_lock);
        if (!n)
            return;
        for (unsigned i = 0; i < n; i++) {
            unsigned char playing = 0;
            FMOD_RESULT r = real_isPlaying(chs[i], &playing);
            if (r == FMOD_OK && playing)
                continue;
            struct midi_channel *c = chan_lock(chs[i]);
            if (c) {
                LOGI("channel %p: FMOD channel ended (r=%d), voice %d freed", chs[i], r, c->id);
                chan_drop((unsigned)(c - g_chans));
                unlock();
            }
        }
        if (n < 64)
            return;
        start += 64;
    }
}

FMOD_RESULT __stdcall hk_System_update(void *sys)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(System_update, (void *), sys);
    LOGT("System::update(%p) = %d", sys, r);
    if (g_engine_state > 0 && engine_status() == ENGINE_FAILED)
        fall_back_to_stock();
    reap_channels();
    log_flush();
    return r;
}

FMOD_RESULT __stdcall hk_System_release(void *sys)
{
    ensure_init();
    AcquireSRWLockExclusive(&g_lock);
    while (g_nchans)
        chan_drop(0);
    for (unsigned i = 0; i < g_nsounds; i++)
        song_ref_put(g_sounds[i].song);
    g_nsounds = 0;
    g_master = NULL;
    ReleaseSRWLockExclusive(&g_lock);
    engine_shutdown();
    g_engine_state = 0;
    FMOD_RESULT r = CALL_REAL(System_release, (void *), sys);
    LOGI("System::release(%p) = %d", sys, r);
    log_flush();
    return r;
}

FMOD_RESULT __stdcall hk_System_createSound(void *sys, const char *name_or_data, unsigned mode,
                                            void *exinfo, void **sound)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(System_createSound, (void *, const char *, unsigned, void *, void **),
                              sys, name_or_data, mode, exinfo, sound);
    void *snd = (r == FMOD_OK && sound) ? *sound : NULL;

    if (!(mode & (FMOD_OPENMEMORY | FMOD_OPENMEMORY_POINT))) {
        LOGI("System::createSound(file \"%.200s\", mode=0x%08x) = %d sound=%p",
             name_or_data ? name_or_data : "(null)", mode, r, snd);
        return r;
    }

    unsigned len = exinfo ? EXINFO_LENGTH(exinfo) : 0;
    int cbsize = exinfo ? EXINFO_CBSIZE(exinfo) : 0;
    const unsigned char *p = (const unsigned char *)name_or_data;
    int midi = snd && p && looks_like_midi(p, len);
    LOGI("System::createSound(mem %p len=%u cbsize=%d mode=0x%08x magic=%.4s) = %d sound=%p%s",
         (const void *)p, len, cbsize, mode, (p && len >= 4) ? (const char *)p : "????",
         r, snd, midi ? " [MIDI]" : "");
    if (!midi)
        return r;

    if (real_Sound_getFormat) {
        int type = -1, fmt = -1, chans = -1, bits = -1;
        FMOD_RESULT fr = real_Sound_getFormat(snd, &type, &fmt, &chans, &bits);
        LOGI("  Sound::getFormat = %d type=%d (MIDI=%d) format=%d channels=%d bits=%d",
             fr, type, FMOD_SOUND_TYPE_MIDI, fmt, chans, bits);
    }
    if (!engine_up())
        return r;

    struct smf_song *song;
    char err[128];
    if (smf_parse(p, len, &song, err, sizeof err)) {
        LOGI("  not registered, stock playback: %s", err);
        return r;
    }
    struct song_ref *ref = song_ref_new(song);
    if (!ref) {
        smf_free(song);
        return r;
    }
    LOGI("  registered: format %u, %u tracks, %u events, %.3f s", song->format, song->ntracks,
         song->nev, (double)song->length_us / 1e6);

    AcquireSRWLockExclusive(&g_lock);
    int i = sound_index(snd);
    if (i >= 0) {
        song_ref_put(g_sounds[i].song);   /* stale entry for a recycled Sound* */
    } else if (g_nsounds < g_capsounds || grow((void **)&g_sounds, &g_capsounds, sizeof *g_sounds)) {
        i = (int)g_nsounds++;
    }
    if (i >= 0) {
        g_sounds[i] = (struct midi_sound){ snd, ref, (mode & (FMOD_LOOP_NORMAL | FMOD_LOOP_BIDI)) != 0 };
        ref = NULL;
    }
    ReleaseSRWLockExclusive(&g_lock);
    song_ref_put(ref);   /* only if the table was full */
    return r;
}

FMOD_RESULT __stdcall hk_System_getMasterChannelGroup(void *sys, void **group)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(System_getMasterChannelGroup, (void *, void **), sys, group);
    if (r == FMOD_OK && group)
        g_master = *group;
    LOGI("System::getMasterChannelGroup(%p) = %d group=%p", sys, r, (r == FMOD_OK && group) ? *group : NULL);
    return r;
}

FMOD_RESULT __stdcall hk_System_playSound(void *sys, int channelid, void *sound, int paused, void **channel)
{
    ensure_init();
    void *in_ch = channel ? *channel : NULL;

    /* FMOD_CHANNEL_REUSE replaces whatever the channel was playing. */
    if (channelid == FMOD_CHANNEL_REUSE && in_ch) {
        struct midi_channel *c = chan_lock(in_ch);
        if (c) {
            chan_drop((unsigned)(c - g_chans));
            unlock();
        }
    }

    FMOD_RESULT r = CALL_REAL(System_playSound, (void *, int, void *, int, void **),
                              sys, channelid, sound, paused, channel);
    void *ch = (r == FMOD_OK && channel) ? *channel : NULL;

    struct song_ref *song = NULL;
    int loop = 0;
    engine_up();   /* notices an asynchronous backend failure */
    AcquireSRWLockExclusive(&g_lock);
    if (ch) {
        int ci = chan_index(ch);   /* a recycled handle: its old voice is gone */
        if (ci >= 0)
            chan_drop((unsigned)ci);
    }
    int si = sound_index(sound);
    if (si >= 0 && ch && g_engine_state > 0) {
        song = g_sounds[si].song;
        song_ref_get(song);
        loop = g_sounds[si].loop;
    }
    ReleaseSRWLockExclusive(&g_lock);

    float f0 = 0;
    int took = 0;
    if (song) {
        if (real_Channel_getFrequency)
            real_Channel_getFrequency(ch, &f0);
        FMOD_RESULT mr = real_setMute(ch, 1);
        if (mr == FMOD_OK) {
            AcquireSRWLockExclusive(&g_lock);
            if (g_nchans < g_capchans || grow((void **)&g_chans, &g_capchans, sizeof *g_chans)) {
                struct midi_channel *c = &g_chans[g_nchans++];
                *c = (struct midi_channel){ ch, sound, g_next_id++, FBOOL(paused), 0, 1.0f, f0, f0 };
                engine_voice_new(c->id, song, loop, eff_gain(c), eff_ratio(c));
                if (!eff_paused(c))
                    engine_voice_paused(c->id, 0);
                took = c->id;
            }
            ReleaseSRWLockExclusive(&g_lock);
            if (!took)
                real_setMute(ch, 0);
        } else {
            LOGI("  setMute(%p, true) = %d: leaving it to FMOD", ch, mr);
        }
        song_ref_put(song);
    }

    if (si >= 0 || g_log_level >= LOG_TRACE)
        log_write("System::playSound(id=%d, sound=%p, paused=%d, in_ch=%p) = %d ch=%p%s f0=%.1f voice=%d",
                  channelid, sound, FBOOL(paused), in_ch, r, ch, si >= 0 ? " [MIDI]" : "", (double)f0, took);
    return r;
}

/* ---- Sound ------------------------------------------------------------- */

FMOD_RESULT __stdcall hk_Sound_release(void *snd)
{
    ensure_init();
    struct song_ref *drop = NULL;
    AcquireSRWLockExclusive(&g_lock);
    int i = sound_index(snd);
    if (i >= 0) {
        drop = g_sounds[i].song;   /* voices hold their own references */
        g_sounds[i] = g_sounds[--g_nsounds];
    }
    ReleaseSRWLockExclusive(&g_lock);
    song_ref_put(drop);
    FMOD_RESULT r = CALL_REAL(Sound_release, (void *), snd);
    LOGCH(i >= 0, "Sound::release(%p)%s = %d", snd, i >= 0 ? " [MIDI]" : "", r);
    return r;
}

FMOD_RESULT __stdcall hk_Sound_getLength(void *snd, unsigned *len, unsigned unit)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Sound_getLength, (void *, unsigned *, unsigned), snd, len, unit);
    AcquireSRWLockShared(&g_lock);
    int midi = sound_index(snd) >= 0;
    ReleaseSRWLockShared(&g_lock);
    LOGCH(midi, "Sound::getLength(%p, unit=0x%x) = %d len=%u%s", snd, unit, r,
          (r == FMOD_OK && len) ? *len : 0, midi ? " [MIDI]" : "");
    return r;
}

/* ---- Channel ----------------------------------------------------------- */

FMOD_RESULT __stdcall hk_Channel_stop(void *ch)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_stop, (void *), ch);
    int id = 0;
    struct midi_channel *c = chan_lock(ch);
    if (c) {
        id = c->id;
        chan_drop((unsigned)(c - g_chans));
        unlock();
    }
    LOGCH(id, "Channel::stop(%p) = %d voice=%d", ch, r, id);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_setPaused(void *ch, int paused)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_setPaused, (void *, int), ch, paused);
    int id = 0;
    struct midi_channel *c = r == FMOD_OK ? chan_lock(ch) : NULL;
    if (c) {
        id = c->id;
        int before = eff_paused(c);
        c->paused = FBOOL(paused);
        if (eff_paused(c) != before)
            engine_voice_paused(c->id, eff_paused(c));
        unlock();
    }
    LOGCH(id, "Channel::setPaused(%p, %d) = %d voice=%d", ch, FBOOL(paused), r, id);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_setVolume(void *ch, float v)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_setVolume, (void *, float), ch, v);
    int id = 0;
    struct midi_channel *c = r == FMOD_OK ? chan_lock(ch) : NULL;
    if (c) {
        id = c->id;
        c->volume = v < 0 ? 0 : v > 1 ? 1 : v;   /* FMOD clamps to 0..1 */
        engine_voice_gain(c->id, eff_gain(c));
        unlock();
    }
    LOGCH(id, "Channel::setVolume(%p, %.4f) = %d voice=%d", ch, (double)v, r, id);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_setFrequency(void *ch, float f)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_setFrequency, (void *, float), ch, f);
    int id = 0, rev = 0;
    struct midi_channel *c = r == FMOD_OK ? chan_lock(ch) : NULL;
    if (c) {
        id = c->id;
        if (f < 0) {
            rev = 1;
        } else {
            c->freq = f;
            engine_voice_ratio(c->id, eff_ratio(c));
        }
        unlock();
    }
    LOGCH(id, "Channel::setFrequency(%p, %.2f) = %d voice=%d", ch, (double)f, r, id);
    if (rev)
        escape(ch, "reverse playback");
    return r;
}

FMOD_RESULT __stdcall hk_Channel_setPan(void *ch, float pan)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_setPan, (void *, float), ch, pan);
    LOGT("Channel::setPan(%p, %.3f) = %d", ch, (double)pan, r);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_setMute(void *ch, int mute)
{
    ensure_init();
    int id = 0;
    struct midi_channel *c = chan_lock(ch);
    if (c) {
        id = c->id;
        c->desired_mute = FBOOL(mute);
        engine_voice_gain(c->id, eff_gain(c));
        unlock();
    }
    /* A MIDI channel stays muted in FMOD; we apply the host's mute ourselves. */
    FMOD_RESULT r = real_setMute(ch, id ? 1 : mute);
    LOGCH(id, "Channel::setMute(%p, %d) = %d voice=%d", ch, FBOOL(mute), r, id);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_setPriority(void *ch, int prio)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_setPriority, (void *, int), ch, prio);
    LOGT("Channel::setPriority(%p, %d) = %d", ch, prio, r);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_setPosition(void *ch, unsigned pos, unsigned unit)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_setPosition, (void *, unsigned, unsigned), ch, pos, unit);
    int id = 0;
    struct midi_channel *c = r == FMOD_OK ? chan_lock(ch) : NULL;
    if (c) {
        id = c->id;
        int64_t us = -1;
        if (unit == FMOD_TIMEUNIT_MS)
            us = (int64_t)pos * 1000;
        else if (unit == FMOD_TIMEUNIT_PCM && c->f0 > 0)
            us = (int64_t)((double)pos * 1e6 / (double)c->f0);
        if (us >= 0)
            engine_voice_seek(c->id, us);
        unlock();
    }
    LOGCH(id, "Channel::setPosition(%p, %u, unit=0x%x) = %d voice=%d", ch, pos, unit, r, id);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_getPosition(void *ch, unsigned *pos, unsigned unit)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_getPosition, (void *, unsigned *, unsigned), ch, pos, unit);
    LOGT("Channel::getPosition(%p, unit=0x%x) = %d pos=%u", ch, unit, r, (r == FMOD_OK && pos) ? *pos : 0);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_setMode(void *ch, unsigned mode)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_setMode, (void *, unsigned), ch, mode);
    int id = 0;
    struct midi_channel *c = r == FMOD_OK ? chan_lock(ch) : NULL;
    if (c) {
        id = c->id;
        if (mode & (FMOD_LOOP_NORMAL | FMOD_LOOP_BIDI))
            engine_voice_loop(c->id, 1);
        else if (mode & FMOD_LOOP_OFF)
            engine_voice_loop(c->id, 0);
        unlock();
    }
    LOGCH(id, "Channel::setMode(%p, 0x%08x) = %d voice=%d", ch, mode, r, id);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_setCallback(void *ch, FMOD_CHANNEL_CALLBACK cb)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_setCallback, (void *, FMOD_CHANNEL_CALLBACK), ch, cb);
    LOGI("Channel::setCallback(%p, %p) = %d", ch, (void *)cb, r);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_set3DAttributes(void *ch, const float *pos, const float *vel)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(Channel_set3DAttributes, (void *, const float *, const float *), ch, pos, vel);
    LOGT("Channel::set3DAttributes(%p, pos=%s(%.2f,%.2f,%.2f)) = %d", ch, pos ? "" : "null",
         pos ? (double)pos[0] : 0.0, pos ? (double)pos[1] : 0.0, pos ? (double)pos[2] : 0.0, r);
    return r;
}

FMOD_RESULT __stdcall hk_Channel_isPlaying(void *ch, unsigned char *playing)
{
    ensure_init();
    FMOD_RESULT r = real_isPlaying(ch, playing);
    LOGT("Channel::isPlaying(%p) = %d playing=%d", ch, r, (r == FMOD_OK && playing) ? *playing : -1);
    return r;
}

/* ---- ChannelGroup (mirrored only for the master group) ----------------- */

/* Re-push gain / ratio / pause for every voice after a master change. */
static void master_changed(int gain, int ratio, int was_paused)
{
    AcquireSRWLockExclusive(&g_lock);
    for (unsigned i = 0; i < g_nchans; i++) {
        struct midi_channel *c = &g_chans[i];
        if (gain)
            engine_voice_gain(c->id, eff_gain(c));
        if (ratio)
            engine_voice_ratio(c->id, eff_ratio(c));
        if (was_paused >= 0 && (c->paused || was_paused) != eff_paused(c))
            engine_voice_paused(c->id, eff_paused(c));
    }
    ReleaseSRWLockExclusive(&g_lock);
}

FMOD_RESULT __stdcall hk_ChannelGroup_setVolume(void *g, float v)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(ChannelGroup_setVolume, (void *, float), g, v);
    if (r == FMOD_OK && g == g_master) {
        g_master_volume = v < 0 ? 0 : v > 1 ? 1 : v;
        master_changed(1, 0, -1);
    }
    LOGI("ChannelGroup::setVolume(%p, %.4f) = %d", g, (double)v, r);
    return r;
}

FMOD_RESULT __stdcall hk_ChannelGroup_setPitch(void *g, float p)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(ChannelGroup_setPitch, (void *, float), g, p);
    if (r == FMOD_OK && g == g_master && p >= 0) {
        g_master_pitch = p;
        master_changed(0, 1, -1);
    }
    LOGI("ChannelGroup::setPitch(%p, %.4f) = %d", g, (double)p, r);
    return r;
}

FMOD_RESULT __stdcall hk_ChannelGroup_setPaused(void *g, int paused)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(ChannelGroup_setPaused, (void *, int), g, paused);
    if (r == FMOD_OK && g == g_master) {
        int was = g_master_paused;
        g_master_paused = FBOOL(paused);
        master_changed(0, 0, was);
    }
    LOGI("ChannelGroup::setPaused(%p, %d) = %d", g, FBOOL(paused), r);
    return r;
}

FMOD_RESULT __stdcall hk_ChannelGroup_setMute(void *g, int mute)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(ChannelGroup_setMute, (void *, int), g, mute);
    if (r == FMOD_OK && g == g_master) {
        g_master_mute = FBOOL(mute);
        master_changed(1, 0, -1);
    }
    LOGI("ChannelGroup::setMute(%p, %d) = %d", g, FBOOL(mute), r);
    return r;
}

FMOD_RESULT __stdcall hk_ChannelGroup_stop(void *g)
{
    ensure_init();
    FMOD_RESULT r = CALL_REAL(ChannelGroup_stop, (void *), g);
    if (r == FMOD_OK && g == g_master) {
        AcquireSRWLockExclusive(&g_lock);
        while (g_nchans)
            chan_drop(0);
        ReleaseSRWLockExclusive(&g_lock);
    }
    LOGI("ChannelGroup::stop(%p) = %d", g, r);
    return r;
}

/* ---- DllMain ----------------------------------------------------------- */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        /* Process exit: just push out what's buffered. No waits, no frees. */
        log_flush();
    }
    return TRUE;
}
