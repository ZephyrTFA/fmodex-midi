#ifndef LOG_H
#define LOG_H

enum { LOG_OFF = 0, LOG_INFO = 1, LOG_TRACE = 2 };

extern int g_log_level;

/* Open the log file (UTF-16 path); level LOG_OFF disables everything. */
void log_open(const wchar_t *path, int level);
/* Format into the in-memory buffer. Safe from any thread; never does I/O. */
void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Write buffered lines to disk. Call from System::update / release only. */
void log_flush(void);
void log_close(void);

#define LOGI(...) do { if (g_log_level >= LOG_INFO)  log_write(__VA_ARGS__); } while (0)
#define LOGT(...) do { if (g_log_level >= LOG_TRACE) log_write(__VA_ARGS__); } while (0)

#endif
