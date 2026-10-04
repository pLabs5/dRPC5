#include "gw_common.h"
struct {
  pthread_mutex_t mu;
  pthread_t th;
  int th_valid;
  int stop;
  int connected;
  int ready;
  int auth_failed;
  char activity[192];
  char status[32];
  char frame[GW_FRAME];
  int has_frame;
  unsigned long long rng;
} g_gw;

char g_gw_msg[GW_MSG];

static unsigned long long rng_next(void) {
  unsigned long long x = g_gw.rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  g_gw.rng = x;
  return x;
}

static void rng_seed(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  g_gw.rng = (unsigned long long)ts.tv_nsec * 6364136223846793005ULL;
  g_gw.rng ^= (unsigned long long)ts.tv_sec << 21;
  g_gw.rng ^= (unsigned long long)getpid() * 2654435761ULL;
  if (!g_gw.rng) g_gw.rng = 0x9E3779B97F4A7C15ULL;
}

static void note_activity(const char *published) {
  pthread_mutex_lock(&g_gw.mu);
  snprintf(g_gw.activity, sizeof(g_gw.activity), "%s", published);
  pthread_mutex_unlock(&g_gw.mu);
}

static void note_frame(const char *frame) {
  pthread_mutex_lock(&g_gw.mu);
  snprintf(g_gw.frame, sizeof(g_gw.frame), "%s", frame);
  g_gw.has_frame = 1;
  pthread_mutex_unlock(&g_gw.mu);
}

static int cached_frame(char *out, size_t cap) {
  int has;
  pthread_mutex_lock(&g_gw.mu);
  has = g_gw.has_frame;
  if (has) snprintf(out, cap, "%s", g_gw.frame);
  pthread_mutex_unlock(&g_gw.mu);
  return has;
}

int gateway_frame(char *out, size_t cap) {
  return cached_frame(out, cap);
}

static void note_status(const char *status) {
  pthread_mutex_lock(&g_gw.mu);
  snprintf(g_gw.status, sizeof(g_gw.status), "%s",
           status && status[0] ? status : "online");
  pthread_mutex_unlock(&g_gw.mu);
}

#define GW_RET_RETRY   1
#define GW_RET_FATAL   2
#define GW_RET_STOP    0
#define GW_RET_DISABLED 3


static int session_once(const char *token, const char *resume_url,
                        const char *session_id, long last_seq, int *rc_out,
                        char *out_url, size_t out_urlcap, char *out_sid,
                        size_t out_sidcap, long *out_seq) {
  CURL *easy;
  char url[GW_URL_MAX];
  char *buf = g_gw_msg;
  size_t blen = 0;
  curl_socket_t fd = (curl_socket_t)-1;
  char hb_url[GW_URL_MAX];
  char hb_sid[128];
  long hb_seq = last_seq;
  long hb_ms = 5000;
  long long next_hb;
  long long next_check;
  long long reidentify_at = 0;
  int identified = 0;
  int got_ready = 0;
  int want_resume = session_id && session_id[0];
  int acked = 1;
  int rc = GW_RET_RETRY;
  int close_code = 0;
  char cached[GW_FRAME];
  char published[192];

  published[0] = 0;
  if (out_url && out_urlcap) snprintf(out_url, out_urlcap, "%s", resume_url ? resume_url : "");
  if (out_sid && out_sidcap) snprintf(out_sid, out_sidcap, "%s", session_id ? session_id : "");
  if (out_seq) *out_seq = last_seq;

  easy = curl_easy_init();
  if (!easy) {
    *rc_out = GW_RET_FATAL;
    return -1;
  }
  gw_build_url(url, sizeof(url), resume_url);

  if (connect_ws(easy, url) != 0) {
    curl_easy_cleanup(easy);
    *rc_out = GW_RET_RETRY;
    return -1;
  }

  curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &fd);
  if (fd != (curl_socket_t)-1) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  }

  hb_ms = (long)cfg_clamped("hb_min_ms", 5000, 1000, 60000);
  {
    long long jitter = (long long)hb_ms * (long long)(rng_next() % 1000) / 1000;
    next_hb = now_ms() + (jitter > 0 ? jitter : 1);
  }
  next_check = now_ms();

  pthread_mutex_lock(&g_gw.mu);
  g_gw.connected = 1;
  g_gw.ready = 0;
  g_gw.auth_failed = 0;
  pthread_mutex_unlock(&g_gw.mu);

  snprintf(hb_url, sizeof(hb_url), "%s", resume_url ? resume_url : "");
  snprintf(hb_sid, sizeof(hb_sid), "%s", session_id ? session_id : "");

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
    if (reidentify_at && now >= reidentify_at) {
      reidentify_at = 0;
      if (send_identify(easy, token) != 0) {
        rc = GW_RET_RETRY;
        break;
      }
      identified = 1;
    }
    if (now >= next_hb) {
      if (!acked) {
        printf("drpc5: gateway heartbeat unacked, reconnecting\n");
        rc = GW_RET_RETRY;
        break;
      }
      acked = 0;
      if (send_heartbeat(easy) != 0) {
        rc = GW_RET_RETRY;
        break;
      }
      next_hb = now + hb_ms;
    }

    wait = next_hb - now_ms();
    if (wait > 500) wait = 500;
    if (wait < 0) wait = 0;

    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    poll(&p, 1, (int)wait);

    if (p.revents & (POLLERR | POLLNVAL)) {
      rc = GW_RET_RETRY;
      break;
    }

    if (p.revents & (POLLHUP | POLLIN)) {
      for (;;) {
        size_t n = 0;
        const struct curl_ws_frame *meta = NULL;
        long op = 0;
        CURLcode cr;

        size_t avail = GW_MSG - 1 - blen;

        if (avail == 0) {
          printf("drpc5: gateway frame too large (%u), retrying\n",
                 (unsigned)blen);
          blen = 0;
          rc = GW_RET_RETRY;
          break;
        }
        cr = curl_ws_recv(easy, buf + blen, avail, &n, &meta);
        if (cr != CURLE_OK) {
          if (cr == CURLE_AGAIN) break;
          rc = GW_RET_RETRY;
          break;
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) {
          close_code = n >= 2
                          ? (((int)(unsigned char)buf[blen] << 8) |
                             (int)(unsigned char)buf[blen + 1])
                          : 1005;
          if (non_resumable(close_code)) {
            printf("drpc5: gateway closed with fatal code %d\n", close_code);
            if (close_code == 4004)
              printf("drpc5: token rejected, sign in again\n");
            rc = GW_RET_FATAL;
          } else {
            printf("drpc5: gateway closed with code %d, retrying\n", close_code);
            rc = GW_RET_RETRY;
          }
          break;
        }
        if (!(meta && (meta->flags & CURLWS_TEXT))) continue;
        blen += n;
        if (n == avail || (meta->flags & CURLWS_OFFSET) || meta->bytesleft > 0)
          continue;
        buf[blen] = 0;
        n = blen;
        blen = 0;

        if (frame_op(buf, &op) != 0) continue;

        if (op == 10) {
          long iv = frame_long(buf, "\"heartbeat_interval\":", 0);
          long hb_floor = (long)cfg_clamped("hb_min_ms", 5000, 1000, 60000);
          if (iv > 0) hb_ms = iv;
          if (hb_ms < hb_floor) hb_ms = hb_floor;
          {
            long long jitter =
                (long long)hb_ms * (long long)(rng_next() % 1000) / 1000;
            next_hb = now_ms() + (jitter > 0 ? jitter : 1);
          }
          if (want_resume && hb_sid[0]) {
            if (send_resume(easy, token, hb_sid, hb_seq) != 0) {
              rc = GW_RET_RETRY;
              break;
            }
          } else if (send_identify(easy, token) != 0) {
            rc = GW_RET_RETRY;
            break;
          }
          identified = 1;
          printf("drpc5: gateway identify sent\n");
        } else if (op == 11) {
          acked = 1;
        } else if (op == 0) {
          long s = frame_long(buf, "\"s\":", -1);
          if (s >= 0) hb_seq = s;
          if (frame_has(buf, "\"t\":\"READY\"") ||
              frame_has(buf, "\"t\": \"READY\"")) {
            char sid[128] = "", rurl[GW_URL_MAX] = "";
            const char *dobj = json_obj(buf, NULL);
            json_read_str(obj_find(dobj, "session_id"), sid, sizeof(sid));
            json_read_str(obj_find(dobj, "resume_gateway_url"), rurl,
                          sizeof(rurl));
            snprintf(hb_sid, sizeof(hb_sid), "%s", sid);
            snprintf(hb_url, sizeof(hb_url), "%s", rurl);
            hb_seq = s >= 0 ? s : hb_seq;
            got_ready = 1;
            {
              char who[64] = "";
              const char *uobj = json_obj(buf, "user");
              if (uobj && *uobj == '{')
                json_read_str(obj_find(uobj, "username"), who, sizeof(who));
              printf("drpc5: gateway READY as %s (session %s)\n",
                     who[0] ? who : "?", sid);
            }
            pthread_mutex_lock(&g_gw.mu);
            g_gw.ready = 1;
            pthread_mutex_unlock(&g_gw.mu);
            if (cached_frame(cached, sizeof(cached)))
              ws_send(easy, cached);
          } else if (frame_has(buf, "\"t\":\"RESUMED\"") ||
                     frame_has(buf, "\"t\": \"RESUMED\"")) {
            got_ready = 1;
            pthread_mutex_lock(&g_gw.mu);
            g_gw.ready = 1;
            pthread_mutex_unlock(&g_gw.mu);
            if (cached_frame(cached, sizeof(cached)))
              ws_send(easy, cached);
          }
        } else if (op == 9) {
          hb_sid[0] = 0;
          hb_url[0] = 0;
          hb_seq = -1;
          want_resume = 0;
          reidentify_at = now_ms() + cfg_clamped("reidentify_ms", 150, 0, 10000);
        } else if (op == 7) {
          rc = GW_RET_RETRY;
          break;
        }
      }
      if (rc != GW_RET_RETRY && rc != GW_RET_FATAL && rc != GW_RET_STOP) break;
    }

    if (!identified) continue;

    now = now_ms();
    if (now < next_check) continue;
    next_check = now + (long)cfg_clamped("poll_ms", 10000, 1000, 300000);

    {
      char status[32], mode[32], name[192], details[192], state[192];
      char lk[320], lt[128], sk[128], st[128];
      gw_activity act;
      char frame[GW_FRAME];

      if (!g_time_known ||
              now_ms() - g_time_synced_ms >
                  cfg_clamped("resync_ms", 600000, 60000, 86400000))
        sync_time_offset();

      reload_config(status, sizeof(status), mode, sizeof(mode), name,
                    sizeof(name), details, sizeof(details), state,
                    sizeof(state), lk, sizeof(lk), lt, sizeof(lt), sk,
                    sizeof(sk), st, sizeof(st));

      if (strcasecmp(mode, "auto") == 0) {
        detect_activity(&act, cfg_bool("src_game", 1),
                        cfg_bool("show_platform", 1),
                        cfg_bool("show_artwork", 1), name, details, state, lk,
                        lt, sk, st);
      } else {
        char ts[32];
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
        (void)ts;
      }

      gateway_build_presence(frame, sizeof(frame), &act, status, published,
                             sizeof(published));
      if (ws_send(easy, frame) != 0) {
        printf("drpc5: presence send failed\n");
        rc = GW_RET_RETRY;
        break;
      }
      note_frame(frame);
      note_status(status);
      note_activity(published);
    }
  }

    if ((rc == GW_RET_STOP || rc == GW_RET_DISABLED) && identified)
    clear_presence(easy);

  if (rc == GW_RET_FATAL) {
    if (out_url && out_urlcap) out_url[0] = 0;
    if (out_sid && out_sidcap) out_sid[0] = 0;
    if (out_seq) *out_seq = -1;
  } else {
    if (out_url && out_urlcap) snprintf(out_url, out_urlcap, "%s", hb_url);
    if (out_sid && out_sidcap) snprintf(out_sid, out_sidcap, "%s", hb_sid);
    if (out_seq) *out_seq = hb_seq;
  }

  if (fd != (curl_socket_t)-1) shutdown(fd, SHUT_RDWR);

  pthread_mutex_lock(&g_gw.mu);
  g_gw.connected = 0;
  g_gw.ready = 0;
  if (rc == GW_RET_FATAL) g_gw.auth_failed = 1;
  pthread_mutex_unlock(&g_gw.mu);

  *rc_out = rc;
  curl_easy_cleanup(easy);
  return got_ready ? 0 : -1;
}

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

    if (rc == GW_RET_FATAL) {
      printf("drpc5: gateway stopped, token rejected\n");
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
      if (attempts >= GW_RESET_SESSION_AFTER) {
        resume_url[0] = 0;
        session_id[0] = 0;
        last_seq = -1;
        attempts = 0;
      }
      attempts++;
    }

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