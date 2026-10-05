#include "gw/gw_common.h"
#include "gw/state.h"

gw_state g_gw;
char g_gw_msg[GW_MSG];

unsigned long long rng_next(void) {
  unsigned long long x = g_gw.rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  g_gw.rng = x;
  return x;
}

void rng_seed(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  g_gw.rng = (unsigned long long)ts.tv_nsec * 6364136223846793005ULL;
  g_gw.rng ^= (unsigned long long)ts.tv_sec << 21;
  g_gw.rng ^= (unsigned long long)getpid() * 2654435761ULL;
  if (!g_gw.rng) g_gw.rng = 0x9E3779B97F4A7C15ULL;
}

void note_activity(const char *published) {
  pthread_mutex_lock(&g_gw.mu);
  snprintf(g_gw.activity, sizeof(g_gw.activity), "%s", published);
  pthread_mutex_unlock(&g_gw.mu);
}

void note_frame(const char *frame) {
  pthread_mutex_lock(&g_gw.mu);
  snprintf(g_gw.frame, sizeof(g_gw.frame), "%s", frame);
  g_gw.has_frame = 1;
  pthread_mutex_unlock(&g_gw.mu);
}

int cached_frame(char *out, size_t cap) {
  int has;
  pthread_mutex_lock(&g_gw.mu);
  has = g_gw.has_frame;
  if (has) snprintf(out, cap, "%s", g_gw.frame);
  pthread_mutex_unlock(&g_gw.mu);
  return has;
}

void note_status(const char *status) {
  pthread_mutex_lock(&g_gw.mu);
  snprintf(g_gw.status, sizeof(g_gw.status), "%s",
           status && status[0] ? status : "online");
  pthread_mutex_unlock(&g_gw.mu);
}

int gateway_frame(char *out, size_t cap) {
  return cached_frame(out, cap);
}

void gateway_status(int *connected, int *ready, int *auth_failed,
                    char *activity, size_t cap) {
  pthread_mutex_lock(&g_gw.mu);
  if (connected) *connected = g_gw.connected;
  if (ready) *ready = g_gw.ready;
  if (auth_failed) *auth_failed = g_gw.auth_failed;
  if (activity && cap) snprintf(activity, cap, "%s", g_gw.activity);
  pthread_mutex_unlock(&g_gw.mu);
}

const char *gateway_last_activity(void) {
  static char buf[192];
  pthread_mutex_lock(&g_gw.mu);
  snprintf(buf, sizeof(buf), "%s", g_gw.activity);
  pthread_mutex_unlock(&g_gw.mu);
  return buf;
}
