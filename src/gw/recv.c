#include "gw/gw_common.h"
#include "gw/session.h"
#include "gw/state.h"

/* Drain whatever frames are readable and act on each complete one.
   Partial frames stay in c->buf across calls; c->blen is reset once a frame is
   dispatched. Only a CLOSE frame, a recv error or a fatal opcode ends the
   session. */
int gw_recv_frames(gw_conn *c, const char *token, int *rc, int *close_code) {
  for (;;) {
    size_t n = 0;
    const struct curl_ws_frame *meta = NULL;
    long op = 0;
    CURLcode cr;
    size_t avail = GW_MSG - 1 - c->blen;

    if (avail == 0) {
      dlogf("drpc5: gateway frame too large (%llu), retrying",
            (unsigned long long)c->blen);
      c->blen = 0;
      *rc = GW_RET_RETRY;
      return 1;
    }
    cr = curl_ws_recv(c->easy, c->buf + c->blen, avail, &n, &meta);
    if (cr != CURLE_OK) {
      /* CURLE_AGAIN just means the socket drained; wait for the next poll. */
      if (cr == CURLE_AGAIN) return 0;
      *rc = GW_RET_RETRY;
      return 1;
    }
    if (meta && (meta->flags & CURLWS_CLOSE)) {
      *close_code = n >= 2 ? (((int)(unsigned char)c->buf[c->blen] << 8) |
                              (int)(unsigned char)c->buf[c->blen + 1])
                           : 1005;
      if (non_resumable(*close_code)) {
        dlogf("drpc5: gateway closed with fatal code %d", *close_code);
        if (*close_code == 4004) dlogf("drpc5: token rejected, sign in again");
        *rc = GW_RET_FATAL;
      } else if (*close_code == 4007 || *close_code == 4009) {
        /* 4007 means our session_id/seq pair is stale, 4009 that the gateway
           version was refused. Resuming with the same values fails
           identically, so drop the session and identify fresh instead of
           burning GW_RESET_SESSION_AFTER attempts on it. */
        dlogf("drpc5: gateway closed %d, discarding session", *close_code);
        c->session_id[0] = 0;
        c->resume_url[0] = 0;
        c->seq = -1;
        c->want_resume = 0;
        *rc = GW_RET_RETRY;
      } else {
        dlogf("drpc5: gateway closed with code %d, retrying", *close_code);
        *rc = GW_RET_RETRY;
      }
      return 1;
    }
    if (!(meta && (meta->flags & CURLWS_TEXT))) continue;

    c->blen += n;
    /* Keep reading while curl says the frame is still arriving. A fragment
       that exactly fills the buffer (n == avail) is also still incomplete,
       because the terminating frame has not been seen. */
    if (n == avail || (meta->flags & CURLWS_OFFSET) || meta->bytesleft > 0)
      continue;
    c->buf[c->blen] = 0;
    c->blen = 0;

    if (frame_op(c->buf, &op) != 0) continue;

    if (op == 10) {
      /* HELLO: adopt the gateway's heartbeat interval and (re)identify. */
      long iv = frame_long(c->buf, "\"heartbeat_interval\":", 0);
      long hb_floor = (long)cfg_clamped("hb_min_ms", 5000, 1000, 60000);
      if (iv > 0) c->hb_ms = iv;
      if (c->hb_ms < hb_floor) c->hb_ms = hb_floor;
      /* A HELLO starts a fresh heartbeat cycle. Clearing acked here stops the
         next tick from dropping a healthy connection because a heartbeat from
         the previous cycle never got its op 11 back. */
      c->acked = 1;
      {
        long long jitter =
            (long long)c->hb_ms * (long long)(rng_next() % 1000) / 1000;
        c->next_hb = now_ms() + (jitter > 0 ? jitter : 1);
      }
      if (c->want_resume && c->session_id[0]) {
        if (send_resume(c->easy, token, c->session_id, c->seq) != 0) {
          *rc = GW_RET_RETRY;
          return 1;
        }
      } else if (send_identify(c->easy, token) != 0) {
        *rc = GW_RET_RETRY;
        return 1;
      }
      c->identified = 1;
      dlogf("drpc5: gateway identify sent");
    } else if (op == 11) {
      c->acked = 1;
    } else if (op == 0) {
      /* DISPATCH: sequence numbers keep the resume cursor current; READY and
         RESUMED are the two frames that mean the session is usable. */
      long s = frame_long(c->buf, "\"s\":", -1);
      if (s >= 0) c->seq = s;
      if (frame_has(c->buf, "\"t\":\"READY\"") ||
          frame_has(c->buf, "\"t\": \"READY\"")) {
        char sid[128] = "", rurl[GW_URL_MAX] = "";
        const char *dobj = json_obj(c->buf, NULL);
        json_read_str(obj_find(dobj, "session_id"), sid, sizeof(sid));
        json_read_str(obj_find(dobj, "resume_gateway_url"), rurl, sizeof(rurl));
        snprintf(c->session_id, sizeof(c->session_id), "%s", sid);
        snprintf(c->resume_url, sizeof(c->resume_url), "%s", rurl);
        c->seq = s >= 0 ? s : c->seq;
        c->got_ready = 1;
        {
          char who[64] = "";
          const char *uobj = json_obj(c->buf, "user");
          if (uobj && *uobj == '{')
            json_read_str(obj_find(uobj, "username"), who, sizeof(who));
          dlogf("drpc5: gateway READY as %s (session %s)",
                who[0] ? who : "?", sid);
        }
        pthread_mutex_lock(&g_gw.mu);
        g_gw.ready = 1;
        pthread_mutex_unlock(&g_gw.mu);
        /* Replay the last presence so the tile appears now rather than after
           the next poll_ms tick. */
        if (cached_frame(c->cached, sizeof(c->cached)))
          ws_send(c->easy, c->cached);
      } else if (frame_has(c->buf, "\"t\":\"RESUMED\"") ||
                 frame_has(c->buf, "\"t\": \"RESUMED\"")) {
        c->got_ready = 1;
        pthread_mutex_lock(&g_gw.mu);
        g_gw.ready = 1;
        pthread_mutex_unlock(&g_gw.mu);
        if (cached_frame(c->cached, sizeof(c->cached)))
          ws_send(c->easy, c->cached);
      }
    } else if (op == 9) {
      /* INVALID_SESSION: re-identify shortly, keeping the socket. */
      c->session_id[0] = 0;
      c->resume_url[0] = 0;
      c->seq = -1;
      c->want_resume = 0;
      c->reidentify_at = now_ms() + cfg_clamped("reidentify_ms", 150, 0, 10000);
    } else if (op == 7) {
      /* RECONNECT: the gateway wants a fresh connection. */
      *rc = GW_RET_RETRY;
      return 1;
    }
  }
}
