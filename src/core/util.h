#pragma once

#include <stddef.h>
#include <time.h>

void notifyf(const char *fmt, ...);
int read_whole_file(const char *path, char *buf, size_t cap);
/* Read a whole text file, always NUL-terminating and reserving one byte for the
   terminator. Returns the byte count, or -1 if the file could not be read. */
int read_file_all(const char *path, char *buf, size_t cap, size_t *out_len);
long long now_ms(void);
/* Monotonic milliseconds. Use for intervals and rate limits, where a wall-clock
   adjustment must not make a deadline jump. */
long long mono_ms(void);
/* Add ms to an absolute CLOCK_REALTIME timespec, for pthread_cond_timedwait. */
void ts_after_ms(struct timespec *ts, long ms);
void nap_ms(int ms);
void base64(const char *in, char *out, size_t cap);
