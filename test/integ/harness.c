/* Integration harness: drives the proxy fmodex.dll the way a typical host does
 * (mangled __stdcall C++ exports, this first), on top of mock_fmod as
 * fmodex_real.dll. One scenario per run; test/integ/run.sh checks the
 * log-backend event log afterwards.
 *
 * usage: harness <scenario> <piano.mid> <tempo.mid> */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int R;
static R (__stdcall *System_Create)(void **);
static R (__stdcall *sys_init)(void *, int, unsigned, void *);
static R (__stdcall *sys_update)(void *);
static R (__stdcall *sys_release)(void *);
static R (__stdcall *createSound)(void *, const char *, unsigned, void *, void **);
static R (__stdcall *getMaster)(void *, void **);
static R (__stdcall *playSound)(void *, int, void *, int, void **);
static R (__stdcall *ch_stop)(void *);
static R (__stdcall *ch_setPaused)(void *, int);
static R (__stdcall *ch_setVolume)(void *, float);
static R (__stdcall *ch_setFrequency)(void *, float);
static R (__stdcall *ch_setMute)(void *, int);
static R (__stdcall *ch_setPosition)(void *, unsigned, unsigned);
static R (__stdcall *grp_setPaused)(void *, int);
static R (__stdcall *grp_setVolume)(void *, float);
static int (__stdcall *mock_get_mute)(void *);
static void (__stdcall *mock_end)(void *);

static void *sys, *master;
static int failures;

#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void *sym(HMODULE m, const char *name)
{
    void *p = (void *)GetProcAddress(m, name);
    if (!p) { printf("missing export %s\n", name); exit(2); }
    return p;
}

static unsigned char *slurp(const char *path, unsigned *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    static unsigned char bufs[4][1 << 16];
    static int nb;
    unsigned char *b = bufs[nb++ & 3];
    *len = (unsigned)fread(b, 1, sizeof bufs[0], f);
    fclose(f);
    return b;
}

/* Typical host createSound: FMOD_OPENMEMORY, exinfo with cbsize 0x7c and length. */
static void *mksound(const void *data, unsigned len, unsigned extra_mode)
{
    unsigned char exinfo[0x7c] = { 0 };
    *(int *)exinfo = 0x7c;
    *(unsigned *)(exinfo + 4) = len;
    void *snd = NULL;
    R r = createSound(sys, data, 0x08000850u | extra_mode, exinfo, &snd);
    CHECK(r == 0 && snd, "createSound = %d", r);
    return snd;
}

/* Typical host playSound: paused, then setters, then unpause. */
static void *play(void *snd, float vol)
{
    void *ch = NULL;
    R r = playSound(sys, -1, snd, 1, &ch);
    CHECK(r == 0 && ch, "playSound = %d", r);
    ch_setVolume(ch, vol);
    ch_setPaused(ch, 0);
    return ch;
}

/* Run the host's System::update loop for ms milliseconds. */
static void pump(int ms)
{
    DWORD t0 = GetTickCount();
    while ((int)(GetTickCount() - t0) < ms) {
        sys_update(sys);
        Sleep(10);
    }
}

int main(int argc, char **argv)
{
    if (argc != 4) { printf("usage: harness <scenario> piano.mid tempo.mid\n"); return 2; }
    const char *sc = argv[1];
    unsigned plen, tlen;
    unsigned char *piano = slurp(argv[2], &plen), *tempo = slurp(argv[3], &tlen);

    HMODULE fm = LoadLibraryW(L"fmodex.dll");
    if (!fm) { printf("LoadLibrary(fmodex.dll) failed: %lu\n", GetLastError()); return 2; }
    System_Create = sym(fm, "FMOD_System_Create");
    sys_init = sym(fm, "?init@System@FMOD@@QAG?AW4FMOD_RESULT@@HIPAX@Z");
    sys_update = sym(fm, "?update@System@FMOD@@QAG?AW4FMOD_RESULT@@XZ");
    sys_release = sym(fm, "?release@System@FMOD@@QAG?AW4FMOD_RESULT@@XZ");
    createSound = sym(fm, "?createSound@System@FMOD@@QAG?AW4FMOD_RESULT@@PBDIPAUFMOD_CREATESOUNDEXINFO@@PAPAVSound@2@@Z");
    getMaster = sym(fm, "?getMasterChannelGroup@System@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVChannelGroup@2@@Z");
    playSound = sym(fm, "?playSound@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PAVSound@2@_NPAPAVChannel@2@@Z");
    ch_stop = sym(fm, "?stop@Channel@FMOD@@QAG?AW4FMOD_RESULT@@XZ");
    ch_setPaused = sym(fm, "?setPaused@Channel@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z");
    ch_setVolume = sym(fm, "?setVolume@Channel@FMOD@@QAG?AW4FMOD_RESULT@@M@Z");
    ch_setFrequency = sym(fm, "?setFrequency@Channel@FMOD@@QAG?AW4FMOD_RESULT@@M@Z");
    ch_setMute = sym(fm, "?setMute@Channel@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z");
    ch_setPosition = sym(fm, "?setPosition@Channel@FMOD@@QAG?AW4FMOD_RESULT@@II@Z");
    grp_setPaused = sym(fm, "?setPaused@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@_N@Z");
    grp_setVolume = sym(fm, "?setVolume@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@M@Z");
    HMODULE real = GetModuleHandleW(L"fmodex_real.dll");
    if (!real) { printf("fmodex_real.dll not loaded after resolving a forwarder\n"); return 2; }
    mock_get_mute = sym(real, "mock_get_mute");
    mock_end = sym(real, "mock_end");

    CHECK(System_Create(&sys) == 0, "System_Create");
    CHECK(sys_init(sys, 64, 0, NULL) == 0, "init");
    CHECK(getMaster(sys, &master) == 0 && master, "getMasterChannelGroup");

    if (!strcmp(sc, "basic")) {
        void *ch = play(mksound(piano, plen, 0), 0.5f);
        CHECK(mock_get_mute(ch) == 1, "MIDI channel must be muted in FMOD (got %d)", mock_get_mute(ch));
        ch_setMute(ch, 0);   /* the host unmuting must not unmute FMOD */
        CHECK(mock_get_mute(ch) == 1, "setMute(false) leaked through to FMOD");
        pump(2200);
        mock_end(ch);
        pump(50);
    } else if (!strcmp(sc, "pause")) {
        void *ch = play(mksound(piano, plen, 0), 1.0f);
        pump(200);
        ch_setPaused(ch, 1);
        pump(500);
        ch_setPaused(ch, 0);
        pump(2000);
    } else if (!strcmp(sc, "stop")) {
        void *ch = play(mksound(piano, plen, 0), 1.0f);
        pump(100);
        ch_stop(ch);
        pump(600);
    } else if (!strcmp(sc, "cutover")) {
        void *snd = mksound(piano, plen, 0);
        play(snd, 1.0f);
        pump(100);
        play(snd, 1.0f);   /* same cached Sound*, second channel */
        pump(700);
    } else if (!strcmp(sc, "reverse")) {
        void *ch = play(mksound(piano, plen, 0), 1.0f);
        pump(100);
        ch_setFrequency(ch, -44100.0f);
        CHECK(mock_get_mute(ch) == 0, "reverse playback must hand the channel back unmuted (got %d)", mock_get_mute(ch));
        pump(600);
    } else if (!strcmp(sc, "corrupt")) {
        static unsigned char bad[64] = "MThd\0\0\0\6\0\1\0\1\0\x60MTrk\0\0\0\x20\x00\x90";
        bad[24] = 0x3c; bad[25] = 0xff;   /* data byte >= 0x80: parse error */
        void *ch = play(mksound(bad, sizeof bad, 0), 1.0f);
        CHECK(mock_get_mute(ch) == 0, "unparseable MIDI must stay stock (mute=%d)", mock_get_mute(ch));
        pump(300);
    } else if (!strcmp(sc, "wav")) {
        static const unsigned char wav[44] = "RIFF\x24\0\0\0WAVEfmt ";
        void *ch = play(mksound(wav, sizeof wav, 0), 1.0f);
        CHECK(mock_get_mute(ch) == 0, "non-MIDI must not be touched (mute=%d)", mock_get_mute(ch));
        pump(200);
    } else if (!strcmp(sc, "master")) {
        play(mksound(piano, plen, 0), 1.0f);
        pump(100);
        grp_setPaused(master, 1);
        pump(300);
        grp_setPaused(master, 0);
        grp_setVolume(master, 0.25f);
        pump(1000);
    } else if (!strcmp(sc, "backendfail")) {
        /* fmodex-midi.ini names a backend that cannot open: the proxy must
         * hand the channel back to FMOD once it notices. */
        void *ch = play(mksound(piano, plen, 0), 1.0f);
        pump(500);
        CHECK(mock_get_mute(ch) == 0, "channel must be unmuted after backend failure (mute=%d)", mock_get_mute(ch));
        void *ch2 = play(mksound(tempo, tlen, 0), 1.0f);
        CHECK(mock_get_mute(ch2) == 0, "later MIDI must play stock (mute=%d)", mock_get_mute(ch2));
    } else if (!strcmp(sc, "loop")) {
        play(mksound(piano, plen, 0x2 /* FMOD_LOOP_NORMAL */), 1.0f);
        pump(4000);
    } else if (!strcmp(sc, "seek")) {
        void *snd = mksound(tempo, tlen, 0), *ch = NULL;
        playSound(sys, -1, snd, 1, &ch);
        ch_setPosition(ch, 1500, 1 /* FMOD_TIMEUNIT_MS */);
        ch_setPaused(ch, 0);
        pump(3000);
    } else {
        printf("unknown scenario %s\n", sc);
        return 2;
    }

    sys_release(sys);
    printf("%s: %s\n", sc, failures ? "FAIL" : "ok");
    return failures != 0;
}
