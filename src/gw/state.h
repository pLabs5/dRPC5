#pragma once

#include "gw/gw_internal.h"

#include <pthread.h>

/* ===========================================================================
   Shared gateway state.

   Every field of g_gw is guarded by mu, except th_valid/rng which are only
   touched by the gateway thread and the start/stop callers before it runs.
   Code that needs more than one field at a time must hold the mutex for the
   whole read; the note_*() and cached_frame() helpers below do exactly that.
   =========================================================================== */
typedef struct {
  pthread_mutex_t mu;
  pthread_t th;
  int th_valid;
  int stop;
  int connected;
  int ready;
  int auth_failed;
  int in_sync;
  char activity[192];
  char status[32];
  char frame[GW_FRAME];
  int has_frame;
  unsigned long long rng;
} gw_state;

extern gw_state g_gw;

/* Reassembly scratch for inbound gateway frames. One buffer for the whole
   session: a frame larger than this is unrecoverable, so the session is
   retried rather than grown. */
extern char g_gw_msg[GW_MSG];

/* xorshift64, seeded once per gateway thread. Used for heartbeat jitter and
   reconnect backoff so a fleet of consoles does not reconnect in lockstep. */
unsigned long long rng_next(void);
void rng_seed(void);

/* Record what we last published, so /api/status can show it. */
void note_activity(const char *published);

/* Record the last presence frame so a reconnect can replay it before the next
   poll, making the tile appear immediately instead of after poll_ms. */
void note_frame(const char *frame);
int cached_frame(char *out, size_t cap);

/* Record the status string from the config. Empty means "online". */
void note_status(const char *status);
