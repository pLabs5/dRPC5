#include "gw/gw_common.h"
#include "gw/session.h"
#include "gw/state.h"

/* Last thing we successfully published, so the log records transitions
   instead of one identical line every poll_ms. */
static char g_last_pub[192];

/* Refresh the Discord clock offset when it is missing or stale, then build and
   send one presence update. Called at most once per poll_ms, from between
   heartbeats. */
int gw_publish(gw_conn *c, long long now) {
  char status[32], mode[32], name[192], details[192], state[192];
  char lk[320], lt[128], sk[128], st[128];
  gw_activity act;
  char frame[GW_FRAME];

  /* Time sync is an HTTPS round trip; doing it inline blocks heartbeats for
     up to the 15s timeout, which is long enough to lose the connection.
     Only pay for it when the offset is actually stale. */
  if (!g_time_known ||
      now_ms() - g_time_synced_ms >
          cfg_clamped("resync_ms", 600000, 60000, 86400000))
    c->need_sync = 1;

  if (now < c->next_check) return 0;
  c->next_check = now + (long)cfg_clamped("poll_ms", 10000, 1000, 300000);

  /* Sync here rather than mid-loop: the wait happens while we are already
     between heartbeats, and the frame below is sent either way. Doing it
     before ws_send would put the stall back on the heartbeat path. */
  if (c->need_sync) {
    c->need_sync = 0;
    /* Track the connection before the blocking call so /api/status does
       not read connected=1 while we are stuck in a network round trip. */
    pthread_mutex_lock(&g_gw.mu);
    g_gw.in_sync = 1;
    pthread_mutex_unlock(&g_gw.mu);
    sync_time_offset();
    pthread_mutex_lock(&g_gw.mu);
    g_gw.in_sync = 0;
    pthread_mutex_unlock(&g_gw.mu);
  }

  reload_config(status, sizeof(status), mode, sizeof(mode), name, sizeof(name),
                details, sizeof(details), state, sizeof(state), lk, sizeof(lk),
                lt, sizeof(lt), sk, sizeof(sk), st, sizeof(st));

  if (strcasecmp(mode, "auto") == 0) {
    detect_activity(&act, cfg_bool("src_game", 1), cfg_bool("show_platform", 1),
                    cfg_bool("show_artwork", 1), name, details, state, lk, lt,
                    sk, st);
  } else {
    memset(&act, 0, sizeof(act));
    act.type = (int)cfg_int64("type", 0);
    act.start_epoch = cfg_int64("start_timestamp", 0);
    act.end_epoch = cfg_int64("end_timestamp", 0);
    if (name[0]) {
      act.has_game = 1;
      act.name = name;
      act.details = details;
      act.state = state;
      act.large_key = lk;
      act.large_text = lt;
      act.small_key = sk;
      act.small_text = st;
    }
  }

  gateway_build_presence(frame, sizeof(frame), &act, status, c->published,
                         sizeof(c->published));
  if (ws_send(c->easy, frame) != 0) {
    dlogf("drpc5: presence send failed");
    return -1;
  }
  /* ws_send only proves the bytes left the socket. A frame Discord rejects
     still looks like success here, so record what we published and let the
     next gateway error or close code show up in the log instead of the
     payload looping silently for ever. */
  /* gateway_build_presence() blanks published[] when the frame did not fit,
     so an empty publish means the update was dropped, not that the console is
     idle. */
  if (!c->published[0] && act.has_game) {
    dlogf("drpc5: presence frame truncated, published empty");
  } else if (!g_last_pub[0] || strcmp(g_last_pub, c->published) != 0) {
    dlogf("drpc5: presence -> %s", c->published[0] ? c->published : "(empty)");
    snprintf(g_last_pub, sizeof g_last_pub, "%s", c->published);
  }
  note_frame(frame);
  note_status(status);
  note_activity(c->published);
  return 0;
}
