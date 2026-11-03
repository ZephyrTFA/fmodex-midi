#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include "backend.h"
#include "log.h"

static void all_off(struct backend *b)
{
    for (int ch = 0; ch < 16; ch++) {
        b->short_msg(b->ctx, 0xb0 | ch | 64 << 8);
        b->short_msg(b->ctx, 0xb0 | ch | 123 << 8);
        b->short_msg(b->ctx, 0xb0 | ch | 120 << 8);
        b->short_msg(b->ctx, 0xb0 | ch | 121 << 8);
    }
}

/* ---- KDMAPI (OmniMIDI) -------------------------------------------------- */

static struct {
    HMODULE dll;
    BOOL (WINAPI *IsKDMAPIAvailable)(void);
    BOOL (WINAPI *InitializeKDMAPIStream)(void);
    BOOL (WINAPI *TerminateKDMAPIStream)(void);
    void (WINAPI *ResetKDMAPIStream)(void);
    void (WINAPI *SendDirectData)(DWORD);
    MMRESULT (WINAPI *SendDirectLongData)(MIDIHDR *, UINT);
    MMRESULT (WINAPI *PrepareLongData)(MIDIHDR *, UINT);
    MMRESULT (WINAPI *UnprepareLongData)(MIDIHDR *, UINT);
} kd;

static void kd_short(void *ctx, uint32_t m) { (void)ctx; kd.SendDirectData(m); }

static void kd_long(void *ctx, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    MIDIHDR h = { .lpData = (LPSTR)data, .dwBufferLength = len, .dwBytesRecorded = len };
    int prepared = kd.PrepareLongData && kd.PrepareLongData(&h, sizeof h) == MMSYSERR_NOERROR;
    MMRESULT r = kd.SendDirectLongData(&h, sizeof h);
    if (prepared && kd.UnprepareLongData)
        kd.UnprepareLongData(&h, sizeof h);
    if (r != MMSYSERR_NOERROR)
        LOGI("kdmapi: SendDirectLongData(%u bytes) = %u", len, r);
}

static void kd_close(void *ctx)
{
    (void)ctx;
    if (kd.ResetKDMAPIStream)
        kd.ResetKDMAPIStream();
    kd.TerminateKDMAPIStream();
    FreeLibrary(kd.dll);
    kd.dll = NULL;
}

static int open_kdmapi(struct backend *b)
{
    /* OmniMIDI installs into System32 (SysWOW64 for us); don't pick one up
     * from the host application's directory or the working directory. */
    HMODULE dll = LoadLibraryExW(L"OmniMIDI.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) {
        LOGI("kdmapi: OmniMIDI.dll not found (err %lu)", GetLastError());
        return -1;
    }
#define K(name) kd.name = (void *)GetProcAddress(dll, #name)
    K(IsKDMAPIAvailable); K(InitializeKDMAPIStream); K(TerminateKDMAPIStream);
    K(ResetKDMAPIStream); K(SendDirectData); K(SendDirectLongData);
    K(PrepareLongData); K(UnprepareLongData);
#undef K
    if (!kd.IsKDMAPIAvailable || !kd.InitializeKDMAPIStream || !kd.TerminateKDMAPIStream ||
        !kd.SendDirectData || !kd.SendDirectLongData) {
        LOGI("kdmapi: OmniMIDI.dll lacks required exports");
        FreeLibrary(dll);
        return -1;
    }
    if (!kd.IsKDMAPIAvailable()) {
        LOGI("kdmapi: IsKDMAPIAvailable() = FALSE");
        FreeLibrary(dll);
        return -1;
    }
    if (!kd.InitializeKDMAPIStream()) {
        LOGI("kdmapi: InitializeKDMAPIStream() failed");
        FreeLibrary(dll);
        return -1;
    }
    kd.dll = dll;
    *b = (struct backend){ "kdmapi", NULL, kd_short, kd_long, NULL, kd_close };
    return 0;
}

/* ---- winmm --------------------------------------------------------------- */

static void mm_short(void *ctx, uint32_t m) { midiOutShortMsg((HMIDIOUT)ctx, m); }

static void mm_long(void *ctx, const uint8_t *data, uint32_t len)
{
    HMIDIOUT h = (HMIDIOUT)ctx;
    MIDIHDR hdr = { .lpData = (LPSTR)data, .dwBufferLength = len, .dwBytesRecorded = len };
    if (midiOutPrepareHeader(h, &hdr, sizeof hdr) != MMSYSERR_NOERROR)
        return;
    if (midiOutLongMsg(h, &hdr, sizeof hdr) == MMSYSERR_NOERROR) {
        /* Synchronous wait (sequencer thread only), bounded at ~1s. */
        for (int i = 0; i < 1000 && !(hdr.dwFlags & MHDR_DONE); i++)
            Sleep(1);
    }
    for (int i = 0; i < 100 && midiOutUnprepareHeader(h, &hdr, sizeof hdr) == MIDIERR_STILLPLAYING; i++)
        Sleep(1);
}

static void mm_close(void *ctx)
{
    HMIDIOUT h = (HMIDIOUT)ctx;
    midiOutReset(h);
    midiOutClose(h);
}

static UINT pick_winmm_device(const wchar_t *want)
{
    if (!want || !*want || !_wcsicmp(want, L"mapper"))
        return MIDI_MAPPER;
    wchar_t *end;
    unsigned long idx = wcstoul(want, &end, 10);
    if (*end == 0)
        return (UINT)idx;
    UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        MIDIOUTCAPSW caps;
        if (midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR) {
            wchar_t a[MAXPNAMELEN], w[128];
            lstrcpynW(a, caps.szPname, MAXPNAMELEN);
            lstrcpynW(w, want, 128);
            _wcslwr(a);
            _wcslwr(w);
            if (wcsstr(a, w))
                return i;
        }
    }
    LOGI("winmm: no device matching \"%ls\", using the mapper", want);
    return MIDI_MAPPER;
}

static int open_winmm(const wchar_t *device, struct backend *b)
{
    UINT id = pick_winmm_device(device);
    HMIDIOUT h;
    MMRESULT r = midiOutOpen(&h, id, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        LOGI("winmm: midiOutOpen(%d) = %u", (int)id, r);
        return -1;
    }
    MIDIOUTCAPSW caps;
    if (midiOutGetDevCapsW((UINT_PTR)h, &caps, sizeof caps) == MMSYSERR_NOERROR)
        LOGI("winmm: opened device %d \"%ls\"", (int)id, caps.szPname);
    *b = (struct backend){ "winmm", h, mm_short, mm_long, NULL, mm_close };
    return 0;
}

/* ---- log (tests) --------------------------------------------------------- */

struct logb {
    FILE *f;
    LARGE_INTEGER t0, freq;
};

static long long logb_now(struct logb *l)
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    long long d = n.QuadPart - l->t0.QuadPart;
    return d / l->freq.QuadPart * 1000000 + d % l->freq.QuadPart * 1000000 / l->freq.QuadPart;
}

static void lb_short(void *ctx, uint32_t m)
{
    struct logb *l = ctx;
    uint8_t st = m & 0xff;
    int n = (st & 0xe0) == 0xc0 ? 2 : 3;
    if (n == 2)
        fprintf(l->f, "%lld %02X %02X\n", logb_now(l), st, (m >> 8) & 0xff);
    else
        fprintf(l->f, "%lld %02X %02X %02X\n", logb_now(l), st, (m >> 8) & 0xff, (m >> 16) & 0xff);
}

static void lb_long(void *ctx, const uint8_t *d, uint32_t len)
{
    struct logb *l = ctx;
    fprintf(l->f, "%lld", logb_now(l));
    for (uint32_t i = 0; i < len; i++)
        fprintf(l->f, " %02X", d[i]);
    fputc('\n', l->f);
}

static void lb_mark(void *ctx, const char *text)
{
    struct logb *l = ctx;
    fprintf(l->f, "# %lld %s\n", logb_now(l), text);
}

static void lb_close(void *ctx)
{
    struct logb *l = ctx;
    fprintf(l->f, "# %lld close\n", logb_now(l));
    fclose(l->f);
    free(l);
}

static int open_log(const wchar_t *path, struct backend *b)
{
    if (!path || !*path) {
        LOGI("log backend: event_log is not set");
        return -1;
    }
    struct logb *l = calloc(1, sizeof *l);
    if (!l)
        return -1;
    l->f = _wfopen(path, L"w");
    if (!l->f) {
        LOGI("log backend: cannot open %ls", path);
        free(l);
        return -1;
    }
    setvbuf(l->f, NULL, _IOFBF, 1 << 16);
    QueryPerformanceFrequency(&l->freq);
    QueryPerformanceCounter(&l->t0);
    *b = (struct backend){ "log", l, lb_short, lb_long, lb_mark, lb_close };
    return 0;
}

int backend_open(enum backend_kind kind, const wchar_t *winmm_device,
                 const wchar_t *event_log, struct backend *out)
{
    int r = -1;
    switch (kind) {
    case BACKEND_KDMAPI: r = open_kdmapi(out); break;
    case BACKEND_WINMM:  r = open_winmm(winmm_device, out); break;
    case BACKEND_LOG:    r = open_log(event_log, out); break;
    case BACKEND_AUTO:   r = open_kdmapi(out) == 0 ? 0 : open_winmm(winmm_device, out); break;
    case BACKEND_OFF:    break;
    }
    if (r == 0) {
        LOGI("backend: %s", out->name);
        all_off(out);
    }
    return r;
}
