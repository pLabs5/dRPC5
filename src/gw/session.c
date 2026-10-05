#include "gw/gw_common.h"
#include "gw/session.h"
#include "gw/state.h"

/* Run a single gateway connection: connect, identify or resume, then loop on
   poll() doing three things in order - service heartbeats, drain frames, and
   publish presence on the poll interval - until the socket or the config says
   to stop. */
int session_once(const char *token, const char *resume_url,
                 const char *session_id, long last_seq, int *rc_out,
                 char *out_url, size_t out_urlcap, char *out_sid,
                 size_t out_sidcap, long *out_seq) {
  gw_conn c;
  char url[GW_URL_MAX];
  CURL *easy;
  int rc = GW_RET_RETRY;
  int close_code = 0;

  memset(&c, 0, sizeof c);
  c.fd = (curl_socket_t)-1;
  c.hb_ms = 5000;
  c.seq = last_seq;
  c.want_resume = (session_id && session_id[0]) ? 1 : 0;
  /* No heartbeat is outstanding yet, so start acked. */
  c.acked = 1;
  c.buf = g_gw_msg;

  if (out_url && out_urlcap)
    snprintf(out_url, out_urlcap, "%s", resume_url ? resume_url : "");
  if (out_sid && out_sidcap)
    snprintf(out_sid, out_sidcap, "%s", session_id ? session_id : "");
  if (out_seq) *out_seq = last_seq;

  easy = curl_easy_init();
  if (!easy) {
    *rc_out = GW_RET_FATAL;
    return -1;
  }
  c.easy = easy;

  gw_build_url(url, sizeof(url), resume_url);

  if (connect_ws(c.easy, url) != 0) {
    curl_easy_cleanup(c.easy);
    *rc_out = GW_RET_RETRY;
    return -1;
  }

  curl_easy_getinfo(c.easy, CURLINFO_ACTIVESOCKET, &c.fd);
  if (c.fd != (curl_socket_t)-1) {
    int fl = fcntl(c.fd, F_GETFL, 0);
    fcntl(c.fd, F_SETFL, fl | O_NONBLOCK);
  }

  c.hb_ms = (long)cfg_clamped("hb_min_ms", 5000, 1000, 60000);
  {
    long long jitter =
        (long long)c.hb_ms * (long long)(rng_next() % 1000) / 1000;
    c.next_hb = now_ms() + (jitter > 0 ? jitter : 1);
  }
  c.next_check = now_ms();

  pthread_mutex_lock(&g_gw.mu);
  g_gw.connected = 1;
  g_gw.ready = 0;
  g_gw.auth_failed = 0;
  pthread_mutex_unlock(&g_gw.mu);

  snprintf(c.resume_url, sizeof(c.resume_url), "%s",
           resume_url ? resume_url : "");
  snprintf(c.session_id, sizeof(c.session_id), "%s",
           session_id ? session_id : "");

  for (;;) {
    struct pollfd p;
    long wait;
    long long now;

    if (__atomic_load_n(&g_gw.stop, __ATOMIC_RELAXED)) {
      rc = GW_RET_STOP;
      break;
    }
    if (!cfg_bool("enabled", 1)) {
      rc = GW_RET_DISABLED;
      break;
    }

    now = now_ms();
    if (c.reidentify_at && now >= c.reidentify_at) {
      c.reidentify_at = 0;
      if (send_identify(c.easy, token) != 0) {
        rc = GW_RET_RETRY;
        break;
      }
      c.identified = 1;
    }
    if (now >= c.next_hb) {
      /* A heartbeat that was never answered means the socket died without the
         close handshake, which is what a console waking from Rest looks like.
         Reconnect rather than sit on a dead connection. */
      if (!c.acked) {
        dlogf("drpc5: gateway heartbeat unacked, reconnecting");
        rc = GW_RET_RETRY;
        break;
      }
      c.acked = 0;
      if (send_heartbeat(c.easy) != 0) {
        rc = GW_RET_RETRY;
        break;
      }
      c.next_hb = now + c.hb_ms;
    }

    /* Cap the wait so the loop re-evaluates the stop flag and the config at
       least twice a second regardless of the heartbeat interval. */
    wait = c.next_hb - now_ms();
    if (wait > 500) wait = 500;
    if (wait < 0) wait = 0;

    p.fd = c.fd;
    p.events = POLLIN;
    p.revents = 0;
    poll(&p, 1, (int)wait);

    if (p.revents & (POLLERR | POLLNVAL)) {
      rc = GW_RET_RETRY;
      break;
    }

    if (p.revents & (POLLHUP | POLLIN)) {
      if (gw_recv_frames(&c, token, &rc, &close_code)) break;
    }

    if (!c.identified) continue;

    if (gw_publish(&c, now_ms()) != 0) {
      rc = GW_RET_RETRY;
      break;
    }
  }

  if ((rc == GW_RET_STOP || rc == GW_RET_DISABLED) && c.identified)
    clear_presence(c.easy);

  if (rc == GW_RET_FATAL) {
    if (out_url && out_urlcap) out_url[0] = 0;
    if (out_sid && out_sidcap) out_sid[0] = 0;
    if (out_seq) *out_seq = -1;
  } else {
    if (out_url && out_urlcap)
      snprintf(out_url, out_urlcap, "%s", c.resume_url);
    if (out_sid && out_sidcap)
      snprintf(out_sid, out_sidcap, "%s", c.session_id);
    if (out_seq) *out_seq = c.seq;
  }

  if (c.fd != (curl_socket_t)-1) shutdown(c.fd, SHUT_RDWR);

  pthread_mutex_lock(&g_gw.mu);
  g_gw.connected = 0;
  g_gw.ready = 0;
  if (rc == GW_RET_FATAL) g_gw.auth_failed = 1;
  pthread_mutex_unlock(&g_gw.mu);

  *rc_out = rc;
  curl_easy_cleanup(c.easy);
  return c.got_ready ? 0 : -1;
}
