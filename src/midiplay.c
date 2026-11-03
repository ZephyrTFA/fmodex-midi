/* midiplay.exe: play a .mid through the same parser, sequencer and backends
 * the proxy uses, without any FMOD host. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "config.h"
#include "engine.h"
#include "log.h"
#include "smf.h"

static void usage(void)
{
    fprintf(stderr,
        "usage: midiplay <file.mid> [options]\n"
        "  --backend auto|kdmapi|winmm|log   (default auto)\n"
        "  --device  mapper|<index>|<name>   winmm device\n"
        "  --out     <events.log>            event log for --backend log\n"
        "  --log     <midiplay.log>          diagnostic log\n"
        "  --loop --seconds <n>              loop, stop after n seconds\n"
        "  --gain <0..1> --ratio <r> --seek <ms>\n"
        "  --pause-at <ms> --pause-for <ms>  pause once during playback\n");
    exit(2);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
        usage();
    const wchar_t *file = argv[1];
    int loop = 0;
    double gain = 1.0, ratio = 1.0, seconds = 0;
    long seek_ms = 0, pause_at = -1, pause_for = 0;
    const wchar_t *diag = NULL;

    g_cfg.backend = BACKEND_AUTO;
    for (int i = 2; i < argc; i++) {
        const wchar_t *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEEDV() do { if (!v) usage(); i++; } while (0)
        if (!wcscmp(a, L"--backend")) {
            NEEDV();
            g_cfg.backend = !wcscmp(v, L"kdmapi") ? BACKEND_KDMAPI : !wcscmp(v, L"winmm") ? BACKEND_WINMM
                          : !wcscmp(v, L"log") ? BACKEND_LOG : BACKEND_AUTO;
        } else if (!wcscmp(a, L"--device")) { NEEDV(); lstrcpynW(g_cfg.winmm_device, v, 128); }
        else if (!wcscmp(a, L"--out"))       { NEEDV(); lstrcpynW(g_cfg.event_log, v, 260); }
        else if (!wcscmp(a, L"--log"))       { NEEDV(); diag = v; }
        else if (!wcscmp(a, L"--loop"))      loop = 1;
        else if (!wcscmp(a, L"--seconds"))   { NEEDV(); seconds = _wtof(v); }
        else if (!wcscmp(a, L"--gain"))      { NEEDV(); gain = _wtof(v); }
        else if (!wcscmp(a, L"--ratio"))     { NEEDV(); ratio = _wtof(v); }
        else if (!wcscmp(a, L"--seek"))      { NEEDV(); seek_ms = _wtol(v); }
        else if (!wcscmp(a, L"--pause-at"))  { NEEDV(); pause_at = _wtol(v); }
        else if (!wcscmp(a, L"--pause-for")) { NEEDV(); pause_for = _wtol(v); }
        else usage();
    }
    log_open(diag, diag ? LOG_TRACE : LOG_OFF);

    FILE *f = _wfopen(file, L"rb");
    if (!f) {
        fwprintf(stderr, L"cannot open %ls\n", file);
        return 1;
    }
    static unsigned char buf[SMF_MAX_INPUT + 1];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    struct smf_song *song;
    char err[128];
    if (smf_parse(buf, n, &song, err, sizeof err)) {
        fprintf(stderr, "parse error: %s\n", err);
        return 1;
    }
    fprintf(stderr, "format %u, %u tracks, %u events, %.3f s\n", song->format, song->ntracks,
            song->nev, (double)song->length_us / 1e6);

    if (engine_start(&g_cfg) == 0)
        while (engine_status() == ENGINE_STARTING)
            Sleep(1);
    if (engine_status() != ENGINE_UP) {
        fprintf(stderr, "no MIDI backend could be opened\n");
        engine_shutdown();
        log_close();
        return 1;
    }
    struct song_ref *ref = song_ref_new(song);
    engine_voice_new(1, ref, loop, gain, ratio);
    song_ref_put(ref);   /* the voice holds its own reference */
    if (seek_ms)
        engine_voice_seek(1, (int64_t)seek_ms * 1000);
    engine_voice_paused(1, 0);

    DWORD t0 = GetTickCount();
    int paused_once = 0;
    for (;;) {
        Sleep(5);
        log_flush();
        DWORD el = GetTickCount() - t0;
        if (pause_at >= 0 && !paused_once && el >= (DWORD)pause_at) {
            engine_voice_paused(1, 1);
            Sleep((DWORD)pause_for);
            engine_voice_paused(1, 0);
            paused_once = 1;
        }
        if (seconds > 0 && el >= (DWORD)(seconds * 1000))
            break;
        if (engine_voice_done(1))
            break;
    }
    engine_voice_free(1);
    engine_shutdown();
    log_close();
    return 0;
}
