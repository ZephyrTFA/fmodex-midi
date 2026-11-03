/* MIDI output backends. All calls happen on the sequencer thread. */
#ifndef BACKEND_H
#define BACKEND_H

#include <stdint.h>
#include <wchar.h>
#include "config.h"

struct backend {
    const char *name;
    void *ctx;
    void (*short_msg)(void *ctx, uint32_t msg);
    void (*long_msg)(void *ctx, const uint8_t *data, uint32_t len);
    void (*mark)(void *ctx, const char *text);   /* log backend only; may be NULL */
    void (*close)(void *ctx);                    /* resets the device, then closes */
};

/* Open the requested backend (BACKEND_AUTO: KDMAPI, then winmm). Returns 0
 * and fills *out on success; -1 if nothing could be opened. */
int backend_open(enum backend_kind kind, const wchar_t *winmm_device,
                 const wchar_t *event_log, struct backend *out);

#endif
