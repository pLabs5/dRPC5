#pragma once

#include "gw/gw_internal.h"

/* Why a gateway session ended. The values are internal to src/gw; only
   session_once(), gw_thread() and gw_recv_frames() agree on them. */
#define GW_RET_RETRY   1 /* transient: reconnect, keeping the session if we can */
#define GW_RET_FATAL   2 /* the token was refused: stop until a new one appears */
#define GW_RET_STOP    0 /* gateway_stop() was called */
#define GW_RET_DISABLED 3 /* "enabled" was turned off in the config */
#define GW_RET_REST    4 /* the console is entering rest mode: park until wake */

/* Mutable state of one gateway connection, shared by the session loop, the
   frame dispatcher and the presence publisher. Bundling it keeps those three
   apart in separate files without inventing a dozen out-parameters. */
typedef struct {
  CURL *easy;
  curl_socket_t fd;

  /* Heartbeat pacing. acked tracks whether the last heartbeat was answered,
     which is how a socket that died silently gets noticed. */
  long hb_ms;
  long long next_hb;
  int acked;

  /* Resume credentials, refreshed from READY and cleared when the gateway
     says they are stale. */
  char resume_url[GW_URL_MAX];
  char session_id[128];
  long seq;
  int want_resume;

  int identified;
  int got_ready;
  long long reidentify_at;
  long long next_check;
  int need_sync;

  /* Inbound frame reassembly, over g_gw_msg. */
  char *buf;
  size_t blen;

  /* Last presence frame and the summary of it we log. */
  char cached[GW_FRAME];
  char published[192];
} gw_conn;

/* Run one gateway connection to completion: connect, identify or resume, pump
   frames, publish presence on the poll interval, then close. Returns 0 if the
   session reached READY/RESUMED at least once (so a caller can treat it as
   healthy and reset its backoff), -1 otherwise. *rc_out always receives the
   GW_RET_* reason. out_url/out_sid/out_seq receive the resume credentials to
   pass to the next call. */
int session_once(const char *token, const char *resume_url,
                 const char *session_id, long last_seq, int *rc_out,
                 char *out_url, size_t out_urlcap, char *out_sid,
                 size_t out_sidcap, long *out_seq);

/* Drain readable websocket frames and act on them. Returns 0 to keep the
   session running, 1 when the caller should leave its loop, in which case
   *rc holds the reason and *close_code the websocket close status if the
   session ended because of a CLOSE frame. Implemented in recv.c. */
int gw_recv_frames(gw_conn *c, const char *token, int *rc, int *close_code);

/* Refresh the Discord clock offset if stale, then build and send one presence
   update. Returns 0 on success, -1 if the frame could not be sent, which the
   caller treats as a reason to reconnect. Implemented in publish.c. */
int gw_publish(gw_conn *c, long long now);
