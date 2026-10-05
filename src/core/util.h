#pragma once

#include <stddef.h>
#include <time.h>

void notifyf(const char *fmt, ...);
/* Open the log file, truncating it if it has grown past LOG_MAX_BYTES. Call
   once at startup; dlogf() opens it on demand if this was never called. */
void init_log(void);
/* Log to LOG_PATH and stdout. Thread-safe, no-op on the file if the log cannot
   be opened. Use for anything that would otherwise only reach a detached
   payload's unreachable stdout. */
void dlogf(const char *fmt, ...);
int read_whole_file(const char *path, char *buf, size_t cap);
/* Read a whole text file, always NUL-terminating and reserving one byte for the
   terminator. Returns the byte count, or -1 if the file could not be read. */
int read_file_all(const char *path, char *buf, size_t cap, size_t *out_len);
/* As read_file_all(), but reports whether the file was longer than the buffer,
   so a silently truncated config or token cannot be mistaken for a short one. */
int read_file_ex(const char *path, char *buf, size_t cap, size_t *out_len,
                 int *out_overflow);
/* Replace a file's contents in one step: write a temp file in the same
   directory, restrict its mode before any bytes land, fsync, then rename over
   the target. rename() is atomic, so a concurrent reader sees either the whole
   old file or the whole new one, and a crash mid-write cannot leave a truncated
   file that later parses as valid-but-wrong. */
int write_file_atomic(const char *path, const char *data, size_t len,
                      unsigned mode);
long long now_ms(void);
/* Monotonic milliseconds. Use for intervals and rate limits, where a wall-clock
   adjustment must not make a deadline jump. */
long long mono_ms(void);
/* Add ms to an absolute CLOCK_REALTIME timespec, for pthread_cond_timedwait. */
void ts_after_ms(struct timespec *ts, long ms);
void nap_ms(int ms);
void base64(const char *in, char *out, size_t cap);
