#include <windows.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "config.h"
#include "log.h"

struct config g_cfg = {
    .enabled = 1,
    .backend = BACKEND_AUTO,
    .winmm_device = L"mapper",
    .curve = CURVE_GM,
    .log_level = LOG_INFO,
};

/* Relative paths in the ini are taken relative to the DLL's directory. */
static void resolve_path(wchar_t *dst, size_t cap, const wchar_t *dir, const wchar_t *val)
{
    if (!val[0]) {
        dst[0] = 0;
    } else if (val[0] == L'\\' || val[0] == L'/' || (val[0] && val[1] == L':')) {
        lstrcpynW(dst, val, (int)cap);
    } else {
        _snwprintf(dst, cap, L"%ls%ls", dir, val);
        dst[cap - 1] = 0;
    }
}

/* GetPrivateProfileString keeps inline comments ("value ; note"): cut at the
 * first ';' or '#' and trim surrounding whitespace. */
static void clean(wchar_t *v)
{
    wchar_t *c = wcspbrk(v, L";#");
    if (c)
        *c = 0;
    size_t n = wcslen(v);
    while (n && iswspace(v[n - 1]))
        v[--n] = 0;
    size_t i = 0;
    while (v[i] && iswspace(v[i]))
        i++;
    if (i)
        memmove(v, v + i, (n - i + 1) * sizeof *v);
}

static void get(const wchar_t *key, const wchar_t *def, wchar_t *out, DWORD cap, const wchar_t *ini)
{
    GetPrivateProfileStringW(L"midi", key, def, out, cap, ini);
    clean(out);
}

void config_load(void *module)
{
    wchar_t dir[MAX_PATH], ini[MAX_PATH], v[260];
    DWORD n = GetModuleFileNameW((HMODULE)module, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash)
        return;
    slash[1] = 0;
    _snwprintf(ini, MAX_PATH, L"%lsfmodex-midi.ini", dir);
    ini[MAX_PATH - 1] = 0;

    get(L"enabled", L"1", v, 260, ini);
    g_cfg.enabled = wcstol(v, NULL, 10) != 0;

    get(L"backend", L"auto", v, 260, ini);
    if (!_wcsicmp(v, L"kdmapi"))      g_cfg.backend = BACKEND_KDMAPI;
    else if (!_wcsicmp(v, L"winmm"))  g_cfg.backend = BACKEND_WINMM;
    else if (!_wcsicmp(v, L"log"))    g_cfg.backend = BACKEND_LOG;
    else if (!_wcsicmp(v, L"off"))    g_cfg.backend = BACKEND_OFF;
    else                              g_cfg.backend = BACKEND_AUTO;

    get(L"winmm_device", L"mapper", g_cfg.winmm_device, 128, ini);

    get(L"volume_curve", L"gm", v, 260, ini);
    g_cfg.curve = !_wcsicmp(v, L"linear") ? CURVE_LINEAR : CURVE_GM;

    get(L"log_file", L"", v, 260, ini);
    resolve_path(g_cfg.log_file, 260, dir, v);

    get(L"log_level", L"info", v, 260, ini);
    g_cfg.log_level = !_wcsicmp(v, L"trace") ? LOG_TRACE
                    : !_wcsicmp(v, L"off")   ? LOG_OFF : LOG_INFO;

    get(L"event_log", L"", v, 260, ini);
    resolve_path(g_cfg.event_log, 260, dir, v);
}
