#include "gw/gw_common.h"
#include "gw/rest.h"
#include "gw/session.h"
#include "gw/state.h"

/* Reconnect supervisor. Owns the retry policy: it decides whether a finished
   session was healthy, when to throw away the resume credentials, how long to
   wait before the next attempt, and when a rejected token means we should sit
   idle until the user signs in again. */
static void *gw_thread(void *arg) {
  char token[TOKEN_MAX];
  char resume_url[GW_URL_MAX];
  char session_id[128];
  long last_seq = -1;
  int attempts = 0;

  (void)arg;
  rng_seed();
  resume_url[0] = 0;
  session_id[0] = 0;
  psn_set_proxy(proxy_external_asset);

  for (;;) {
    int rc = GW_RET_RETRY;
    int healthy;
    char n_url[GW_URL_MAX];
    char n_sid[128];
    long n_seq = -1;

    if (__atomic_load_n(&g_gw.stop, __ATOMIC_RELAXED)) break;

    if (rest_active()) {
      /* The console is in rest mode, so the whole process is frozen. The OS
         could have cut us off mid-write, which makes the resume credentials
         untrustworthy; park here until the console wakes, then start over with
         a fresh session. */
      while (rest_active() &&
             !__atomic_load_n(&g_gw.stop, __ATOMIC_RELAXED)) {
        nap_ms(1000);
      }
      if (__atomic_load_n(&g_gw.stop, __ATOMIC_RELAXED)) break;
      resume_url[0] = 0;
      session_id[0] = 0;
      last_seq = -1;
      attempts = 0;
      continue;
    }

    if (!cfg_bool("enabled", 1)) {
      nap_ms(2000);
      continue;
    }
    if (read_token(token, sizeof(token)) != 0) {
      nap_ms(3000);
      continue;
    }

    healthy = session_once(token, resume_url[0] ? resume_url : NULL, session_id,
                           last_seq, &rc, n_url, sizeof(n_url), n_sid,
                           sizeof(n_sid), &n_seq);
    snprintf(resume_url, sizeof(resume_url), "%s", n_url);
    snprintf(session_id, sizeof(session_id), "%s", n_sid);
    last_seq = n_seq;

    if (__atomic_load_n(&g_gw.stop, __ATOMIC_RELAXED)) break;

    if (rc == GW_RET_STOP) break;

    if (rc == GW_RET_DISABLED) {
      resume_url[0] = 0;
      session_id[0] = 0;
      last_seq = -1;
      attempts = 0;
      nap_ms(2000);
      continue;
    }

    if (rc == GW_RET_REST) {
      /* Dropped out of a live session partway into rest mode. The loop-top
         park normally handles the wait and drops the credentials; this branch
         exists for the race where the console wakes again before we get
         there. */
      resume_url[0] = 0;
      session_id[0] = 0;
      last_seq = -1;
      attempts = 0;
      continue;
    }

    if (rc == GW_RET_FATAL) {
      /* The token itself is bad, so retrying cannot help. Park here and poll
         for a new token; the UI writes it without restarting the payload. */
      dlogf("drpc5: gateway stopped, token rejected");
      resume_url[0] = 0;
      session_id[0] = 0;
      last_seq = -1;
      for (;;) {
        if (__atomic_load_n(&g_gw.stop, __ATOMIC_RELAXED)) break;
        nap_ms(10000);
        if (read_token(token, sizeof(token)) == 0) {
          pthread_mutex_lock(&g_gw.mu);
          g_gw.auth_failed = 0;
          pthread_mutex_unlock(&g_gw.mu);
          break;
        }
      }
      continue;
    }

    if (healthy) {
      attempts = 0;
    } else {
      /* A session that never reached READY repeatedly will not start by
         resuming, so stop carrying stale credentials after a few tries. */
      if (attempts >= GW_RESET_SESSION_AFTER) {
        resume_url[0] = 0;
        session_id[0] = 0;
        last_seq = -1;
        attempts = 0;
      }
      attempts++;
    }

    /* Exponential backoff with jitter, capped, so a gateway outage does not
       turn into a reconnect storm and a fleet does not sync up. */
    {
      int shift = attempts - 1;
      long long b;
      if (shift > GW_BACKOFF_MAX_SHIFT) shift = GW_BACKOFF_MAX_SHIFT;
      b = (long long)GW_BACKOFF_BASE << shift;
      if (b > GW_BACKOFF_MAX) b = GW_BACKOFF_MAX;
      b += (long long)(rng_next() % (unsigned long long)GW_BACKOFF_BASE);
      if (b > GW_BACKOFF_MAX) b = GW_BACKOFF_MAX;
      nap_ms((int)b);
    }
  }

  return NULL;
}

int gateway_start(void) {
  if (g_gw.th_valid) return 0;
  g_gw.stop = 0;
  g_gw.auth_failed = 0;
  if (rest_start() != 0) return -1;
  if (pthread_create(&g_gw.th, NULL, gw_thread, NULL) != 0) return -1;
  g_gw.th_valid = 1;
  return 0;
}

void gateway_stop(void) {
  pthread_t th;
  if (!g_gw.th_valid) return;
  th = g_gw.th;
  __atomic_store_n(&g_gw.stop, 1, __ATOMIC_RELEASE);
  pthread_join(th, NULL);
  g_gw.th_valid = 0;
  rest_stop();
}
