#ifndef CONFIG_H
#define CONFIG_H

#include <wchar.h>

enum backend_kind { BACKEND_AUTO, BACKEND_KDMAPI, BACKEND_WINMM, BACKEND_LOG, BACKEND_OFF };
enum volume_curve { CURVE_GM, CURVE_LINEAR };

struct config {
    int enabled;
    enum backend_kind backend;
    wchar_t winmm_device[128];   /* "mapper", an index, or a name substring */
    enum volume_curve curve;
    wchar_t log_file[260];
    int log_level;               /* LOG_OFF / LOG_INFO / LOG_TRACE */
    wchar_t event_log[260];
};

extern struct config g_cfg;

/* Load fmodex-midi.ini from the directory containing `module`. */
void config_load(void *module);

#endif
