#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "log.h"

#define LOG_CAP (1u << 20)

int g_log_level = LOG_OFF;

static SRWLOCK g_lock = SRWLOCK_INIT;
static HANDLE g_file = INVALID_HANDLE_VALUE;
static char g_buf[LOG_CAP];
static unsigned g_len;
static unsigned g_dropped;
static LARGE_INTEGER g_t0, g_freq;

void log_open(const wchar_t *path, int level)
{
    if (level <= LOG_OFF || !path || !*path)
        return;
    g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_file == INVALID_HANDLE_VALUE)
        return;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_log_level = level;
}

void log_write(const char *fmt, ...)
{
    char line[1024];
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double ms = (double)(now.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;

    int n = _snprintf(line, sizeof line, "%10.3f [%5lu] ", ms, GetCurrentThreadId());
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
    va_end(ap);
    if (m < 0 || (unsigned)m >= sizeof line - n - 1)
        m = (int)(sizeof line - n - 2);  /* truncated */
    n += m;
    line[n++] = '\n';

    AcquireSRWLockExclusive(&g_lock);
    if (g_len + (unsigned)n <= LOG_CAP) {
        memcpy(g_buf + g_len, line, (size_t)n);
        g_len += (unsigned)n;
    } else {
        g_dropped++;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void log_flush(void)
{
    static char out[LOG_CAP + 64];
    unsigned len, dropped;

    if (g_file == INVALID_HANDLE_VALUE)
        return;
    AcquireSRWLockExclusive(&g_lock);
    len = g_len;
    dropped = g_dropped;
    memcpy(out, g_buf, len);
    g_len = 0;
    g_dropped = 0;
    ReleaseSRWLockExclusive(&g_lock);

    if (dropped)
        len += (unsigned)_snprintf(out + len, 64, "*** %u log lines dropped\n", dropped);
    if (len) {
        DWORD wr;
        WriteFile(g_file, out, len, &wr, NULL);
    }
}

void log_close(void)
{
    log_flush();
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_log_level = LOG_OFF;
}
