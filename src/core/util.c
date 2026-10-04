#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

void notifyf(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;

  memset(&req, 0, sizeof req);
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
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

long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void nap_ms(int ms) {
  struct timespec ts;
  if (ms <= 0) return;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
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
      if (o + 1 >= cap) return;
      out[o++] = b64tab[(acc >> (bits - 6)) & 0x3f];
      bits -= 6;
    }
  }
  if (bits > 0 && o + 4 < cap) {
    out[o++] = b64tab[(acc << (6 - bits)) & 0x3f];
    while ((o - (size_t)0) % 4 != 0 && o + 1 < cap) out[o++] = '=';
  }
  if (cap) out[o < cap ? o : cap - 1] = 0;
}