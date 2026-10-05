#define _GNU_SOURCE
#include "core/util.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "paths.h"

/* The payload is sent with no terminal attached, so stdout goes nowhere once
   the loader detaches. Every diagnostic therefore also goes to a file, capped
   so a long-lived install cannot fill /data. dlogf() is safe to call from any
   thread and is a no-op once init_log() has failed. */
static FILE *g_log;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

static void log_open_locked(void) {
  long sz;

  if (g_log) return;
  g_log = fopen(LOG_PATH, "ab");
  if (!g_log) return;
  /* Truncate rather than rotate: one file, trimmed at startup, keeps the most
     recent run and avoids leaving .1 files behind in a directory the payload
     may not have permission to create. */
  sz = ftell(g_log);
  if (sz > LOG_MAX_BYTES) {
    fclose(g_log);
    g_log = fopen(LOG_PATH, "wb");
    if (!g_log) return;
    fprintf(g_log, "--- log truncated (was %ld bytes) ---\n", sz);
  }
}

void init_log(void) {
  pthread_mutex_lock(&g_log_mu);
  log_open_locked();
  pthread_mutex_unlock(&g_log_mu);
}

void dlogf(const char *fmt, ...) {
  va_list ap;
  struct timespec ts;
  struct tm tm;

  clock_gettime(CLOCK_REALTIME, &ts);
  localtime_r(&ts.tv_sec, &tm);
  pthread_mutex_lock(&g_log_mu);
  log_open_locked();
  if (g_log) {
    fprintf(g_log, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec,
            (int)(ts.tv_nsec / 1000000));
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
  }
  pthread_mutex_unlock(&g_log_mu);

  va_start(ap, fmt);
  vfprintf(stdout, fmt, ap);
  va_end(ap);
  fputc('\n', stdout);
}

/* Truncating read that reports overflow, so callers can tell a complete file
   from one that was cut short by the buffer. Returns bytes read, -1 on open
   failure, and sets *out_overflow when the file did not fit. */
int read_file_ex(const char *path, char *buf, size_t cap, size_t *out_len,
                 int *out_overflow) {
  FILE *f;
  size_t n;

  if (out_overflow) *out_overflow = 0;
  if (cap == 0) return -1;
  f = fopen(path, "rb");
  if (!f) return -1;
  n = fread(buf, 1, cap - 1, f);
  /* fread can return a short count without setting EOF when the file ends
     exactly on the boundary, so probe explicitly instead of trusting feof. */
  if (n == cap - 1 && fgetc(f) != EOF && out_overflow) *out_overflow = 1;
  fclose(f);
  buf[n] = 0;
  if (out_len) *out_len = n;
  return (int)n;
}

int write_file_atomic(const char *path, const char *data, size_t len,
                      unsigned mode) {
  char tmp[512];
  FILE *f;
  int fd;

  if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
    return -1;
  /* O_CREAT|O_EXCL would need a stale-tmp cleanup; O_TRUNC is enough because
     the temp path is ours and any leftover from a killed run is overwritten. */
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) return -1;
  /* Set the mode before writing: a concurrent opener must never be able to
     read the contents while they are still group/world readable. */
  if (fchmod(fd, (mode_t)mode) != 0) {
    close(fd);
    unlink(tmp);
    return -1;
  }
  f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmp);
    return -1;
  }
  if (len && fwrite(data, 1, len, f) != len) {
    fclose(f);
    unlink(tmp);
    return -1;
  }
  if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
    fclose(f);
    unlink(tmp);
    return -1;
  }
  if (fclose(f) != 0) {
    unlink(tmp);
    return -1;
  }
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

int read_whole_file(const char *path, char *buf, size_t cap) {
  FILE *f;
  size_t n;

  f = fopen(path, "rb");
  if(!f) return -1;
  n = fread(buf, 1, cap, f);
  fclose(f);
  return (int)n;
}

int read_file_all(const char *path, char *buf, size_t cap, size_t *out_len) {
  FILE *f;
  size_t n;

  if(cap == 0) return -1;
  f = fopen(path, "rb");
  if(!f) return -1;
  n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = 0;
  if(out_len) *out_len = n;
  return (int)n;
}

long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

long long mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void ts_after_ms(struct timespec *ts, long ms) {
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

void nap_ms(int ms) {
  struct timespec ts;
  if (ms <= 0) return;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts_after_ms(&ts, ms);
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
}

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64(const char *in, char *out, size_t cap) {
  size_t o = 0;
  unsigned long acc = 0;
  int bits = 0;
  const unsigned char *p = (const unsigned char *)in;

  for (; *p; p++) {
    acc = (acc << 8) | *p;
    bits += 8;
    while (bits >= 6) {
      if (o + 1 >= cap) goto done;
      out[o++] = b64tab[(acc >> (bits - 6)) & 0x3f];
      bits -= 6;
    }
  }
  if (bits > 0 && o + 4 < cap) {
    out[o++] = b64tab[(acc << (6 - bits)) & 0x3f];
    while ((o - (size_t)0) % 4 != 0 && o + 1 < cap) out[o++] = '=';
  }
done:
  if (cap) out[o < cap ? o : cap - 1] = 0;
}