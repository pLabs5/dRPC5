#define _GNU_SOURCE
#include "gw/rest.h"

#include "core/config.h"
#include "core/util.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* Event flag imports, resolved against libkernel by the payload loader exactly
   like the sceKernel* functions in src/notify and src/presence. */
int sceKernelOpenEventFlag(intptr_t *ef, const char *name);
int sceKernelPollEventFlag(intptr_t ef, unsigned long long bit_pattern,
                           unsigned int wait_mode,
                           unsigned long long *result_pattern);
int sceKernelCloseEventFlag(intptr_t ef);

/* The state field occupies bits [0..15] of the SceSystemStateMgrInfo event
   flag. These are the codes shellcore writes, not the public
   sceSystemStateMgr* enums. */
enum {
  STATE_SHUTDOWN_ON_GOING = 100,
  STATE_SUSPEND_ON_GOING = 300,
  STATE_MAIN_ON_STANDBY = 500,
  STATE_WORKING = 1000,
};

#define REST_FLAG_NAME       "SceSystemStateMgrInfo"
#define REST_POLL_MS_AWAKE   100
#define REST_POLL_MS_ASLEEP  500
#define REST_OPEN_RETRY_MS   2000
#define REST_CONFIG_CHECK_MS 1000

static pthread_t g_th;
static int g_th_valid;
static int g_stop;
static int g_in_rest;

int rest_active(void) {
  return __atomic_load_n(&g_in_rest, __ATOMIC_RELAXED);
}

static int is_rest_state(int state) {
  return state == STATE_SHUTDOWN_ON_GOING ||
         state == STATE_SUSPEND_ON_GOING ||
         state == STATE_MAIN_ON_STANDBY;
}

static const char *state_name(int state) {
  switch (state) {
  case STATE_SHUTDOWN_ON_GOING:
    return "SHUTDOWN_ON_GOING";
  case STATE_SUSPEND_ON_GOING:
    return "SUSPEND_ON_GOING";
  case STATE_MAIN_ON_STANDBY:
    return "MAIN_ON_STANDBY";
  case STATE_WORKING:
    return "WORKING";
  default:
    return "OTHER";
  }
}

static void *rest_thread(void *arg) {
  intptr_t ef = -1;
  int enabled = 1;
  int unknown = 1;
  int last_rest = 0;
  long long next_cfg = 0;

  (void)arg;
  for (;;) {
    unsigned long long pattern = 0;
    int state;
    int rest;

    if (__atomic_load_n(&g_stop, __ATOMIC_RELAXED)) break;

    /* The config is re-read on a slow cadence so an opt-out does not cost a
       config-file read on every wake poll. */
    if (now_ms() >= next_cfg) {
      next_cfg = now_ms() + REST_CONFIG_CHECK_MS;
      enabled = cfg_bool("rest_mode", 1);
    }
    if (!enabled) {
      if (ef != -1) {
        sceKernelCloseEventFlag(ef);
        ef = -1;
      }
      if (last_rest) {
        last_rest = 0;
        __atomic_store_n(&g_in_rest, 0, __ATOMIC_RELAXED);
        dlogf("drpc5: rest handling disabled");
      }
      unknown = 1;
      nap_ms(200);
      continue;
    }

    if (ef == -1) {
      if (sceKernelOpenEventFlag(&ef, REST_FLAG_NAME) != 0) {
        nap_ms(REST_OPEN_RETRY_MS);
        continue;
      }
      unknown = 1;
    }

    if (sceKernelPollEventFlag(ef, (unsigned long long)-1, 2, &pattern) != 0) {
      /* The flag vanished, so shellcore restarted; reopen rather than guess. */
      sceKernelCloseEventFlag(ef);
      ef = -1;
      unknown = 1;
      nap_ms(REST_OPEN_RETRY_MS);
      continue;
    }

    state = (int)(pattern & 0xFFFFu);
    rest = is_rest_state(state);

    if (unknown) {
      /* First read records the current state without acting on it, so a
         payload started mid-transition does not invent a rest cycle. */
      unknown = 0;
      last_rest = rest;
      dlogf("drpc5: rest state=%s", state_name(state));
    } else if (rest != last_rest) {
      last_rest = rest;
      __atomic_store_n(&g_in_rest, rest, __ATOMIC_RELAXED);
      dlogf("drpc5: console %s", rest ? "entered rest mode, parking gateway"
                                      : "woke from rest, resuming gateway");
    }

    nap_ms(rest ? REST_POLL_MS_ASLEEP : REST_POLL_MS_AWAKE);
  }

  if (ef != -1) sceKernelCloseEventFlag(ef);
  return NULL;
}

int rest_start(void) {
  if (g_th_valid) return 0;
  __atomic_store_n(&g_stop, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_in_rest, 0, __ATOMIC_RELAXED);
  if (pthread_create(&g_th, NULL, rest_thread, NULL) != 0) return -1;
  g_th_valid = 1;
  return 0;
}

void rest_stop(void) {
  pthread_t th;
  if (!g_th_valid) return;
  th = g_th;
  __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
  pthread_join(th, NULL);
  g_th_valid = 0;
}