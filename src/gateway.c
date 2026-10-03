#define _GNU_SOURCE
#include "gateway.h"
#include "curl_api.h"
#include "paths.h"
#include "presence.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define GW_URL       "wss://gateway.discord.gg"
#define GW_QUERY     "/?v=10&encoding=json"
#define GW_QUERY_NOSLASH "?v=10&encoding=json"
#define GW_FRAME     8192
#define GW_URL_MAX   256
#define GW_CHECK_MS  10000
#define GW_HB_FLOOR  5000
#define GW_BACKOFF_BASE 1000
#define GW_BACKOFF_MAX  60000
#define GW_BACKOFF_MAX_SHIFT 6
#define GW_RESET_SESSION_AFTER 4
#define GW_CAPABILITIES 30717
#define GW_CLIENT_BUILD 267206
#define GW_CLIENT_VERSION "267.0"
#define GW_RELEASE_CHANNEL "googleRelease"

#define GW_USER_AGENT "Discord-Android/267206;RNA"
#define GW_ORIGIN     "https://discord.com"

static struct {
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

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap_ms(int ms) {
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

static int cfg_get(const char *key, char *out, size_t cap) {
  char line[512];
  FILE *f;
  size_t klen = strlen(key);
  int found = 0;

  if (cap == 0) return -1;
  out[0] = 0;
  f = fopen(CONFIG_PATH, "r");
  if (!f) return -1;
  while (fgets(line, sizeof(line), f)) {
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = 0;
    if (strncmp(line, key, klen) != 0 || line[klen] != '=') continue;
    snprintf(out, cap, "%s", line + klen + 1);
    found = 1;
    break;
  }
  fclose(f);
  return found ? 0 : -1;
}

static int cfg_bool(const char *key, int dflt) {
  char buf[64];
  if (cfg_get(key, buf, sizeof(buf)) != 0 || buf[0] == 0) return dflt;
  if (!strcmp(buf, "1") || !strcasecmp(buf, "true") || !strcasecmp(buf, "on") ||
      !strcasecmp(buf, "yes"))
    return 1;
  if (!strcmp(buf, "0") || !strcasecmp(buf, "false") || !strcasecmp(buf, "off") ||
      !strcasecmp(buf, "no"))
    return 0;
  return dflt;
}

static long long cfg_int64(const char *key, long long dflt) {
  char buf[32];
  if (cfg_get(key, buf, sizeof(buf)) != 0 || buf[0] == 0) return dflt;
  return atoll(buf);
}

static int read_token(char *out, size_t cap) {
  FILE *f;
  size_t n;

  if (cap == 0) return -1;
  out[0] = 0;
  f = fopen(TOKEN_PATH, "rb");
  if (!f) return -1;
  n = fread(out, 1, cap - 1, f);
  fclose(f);
  out[n] = 0;
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
    out[--n] = 0;
  return n >= 20 ? 0 : -1;
}

static void json_copy(char *dst, size_t cap, const char *src) {
  size_t o = 0;
  if (!cap) return;
  if (!src) src = "";
  while (*src && o + 7 < cap) {
    unsigned char c = (unsigned char)*src++;
    if (c == '"' || c == '\\') {
      dst[o++] = '\\';
      dst[o++] = (char)c;
    } else if (c == '\n') {
      dst[o++] = '\\';
      dst[o++] = 'n';
    } else if (c == '\r') {
      dst[o++] = '\\';
      dst[o++] = 'r';
    } else if (c == '\t') {
      dst[o++] = '\\';
      dst[o++] = 't';
    } else if (c < 0x20) {
      o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
    } else {
      dst[o++] = (char)c;
    }
  }
  dst[o] = 0;
}

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const char *in, char *out, size_t cap) {
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

static void build_properties(char *out, size_t cap) {
  snprintf(out, cap,
           "{\"os\":\"Android\",\"browser\":\"Discord Android\","
           "\"device\":\"PS5\",\"system_locale\":\"en-US\","
           "\"browser_user_agent\":\"\",\"browser_version\":\"\","
           "\"os_version\":\"\",\"client_version\":\"" GW_CLIENT_VERSION "\","
           "\"release_channel\":\"" GW_RELEASE_CHANNEL "\","
           "\"client_build_number\":%d,\"design_id\":0}",
           GW_CLIENT_BUILD);
}

static int frame_op(const char *frame, long *op) {
  const char *p = strstr(frame, "\"op\":");
  if (!p) return -1;
  p += 5;
  while (*p == ' ') p++;
  *op = strtol(p, NULL, 10);
  return 0;
}

static int frame_has(const char *frame, const char *needle) {
  return strstr(frame, needle) != NULL;
}

static long frame_long(const char *frame, const char *key, long dflt) {
  const char *p = strstr(frame, key);
  char *end;
  long v;
  if (!p) return dflt;
  p += strlen(key);
  while (*p == ' ') p++;
  if (*p < '0' || *p > '9') return dflt;
  v = strtol(p, &end, 10);
  return end == p ? dflt : v;
}

static int frame_str(const char *frame, const char *key, char *out,
                     size_t cap) {
  const char *p = strstr(frame, key);
  size_t i = 0;
  if (!p || cap == 0) return -1;
  p += strlen(key);
  while (*p == ' ') p++;
  if (*p != '"') return -1;
  p++;
  while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
  out[i] = 0;
  return 0;
}

static int ws_send(CURL *easy, const char *frame) {
  size_t sent = 0;
  size_t len = strlen(frame);
  if (len == 0) return -1;
  return curl_ws_send(easy, frame, len, &sent, (curl_off_t)len,
                      CURLWS_TEXT) == CURLE_OK
             ? 0
             : -1;
}

static size_t gw_append(char *out, size_t cap, size_t off, int *trunc,
                        const char *fmt, ...) {
  va_list ap;
  int n;

  if (off >= cap) {
    *trunc = 1;
    return off;
  }
  va_start(ap, fmt);
  n = vsnprintf(out + off, cap - off, fmt, ap);
  va_end(ap);
  if (n < 0) {
    *trunc = 1;
    return off;
  }
  if ((size_t)n >= cap - off) {
    *trunc = 1;
    return cap - 1;
  }
  return off + (size_t)n;
}

int gateway_build_presence(char *out, size_t cap, const gw_activity *act,
                           const char *status, char *published,
                           size_t pubcap) {
  char esc[512];
  const char *name = act && act->name ? act->name : "";
  size_t off = 0;
  int trunc = 0;

  if (cap == 0) return -1;
  if (published && pubcap) published[0] = 0;

  off = gw_append(out, cap, off, &trunc,
                  "{\"op\":3,\"d\":{\"since\":0,\"activities\":[");
  if (act && act->has_game) {
    json_copy(esc, sizeof(esc), name);
    off = gw_append(out, cap, off, &trunc, "{\"name\":\"%s\",\"type\":%d,",
                    esc, act->type);
    if (act->details && act->details[0]) {
      json_copy(esc, sizeof(esc), act->details);
      off = gw_append(out, cap, off, &trunc, "\"details\":\"%s\",", esc);
    }
    if (act->state && act->state[0]) {
      json_copy(esc, sizeof(esc), act->state);
      off = gw_append(out, cap, off, &trunc, "\"state\":\"%s\",", esc);
    }
    if (act->start_epoch > 0 || act->end_epoch > 0) {
      off = gw_append(out, cap, off, &trunc, "\"timestamps\":{");
      if (act->start_epoch > 0)
        off = gw_append(out, cap, off, &trunc, "\"start\":%lld",
                        (long long)act->start_epoch);
      if (act->start_epoch > 0 && act->end_epoch > 0)
        off = gw_append(out, cap, off, &trunc, ",");
      if (act->end_epoch > 0)
        off = gw_append(out, cap, off, &trunc, "\"end\":%lld",
                        (long long)act->end_epoch);
      off = gw_append(out, cap, off, &trunc, "},");
    }
    off = gw_append(out, cap, off, &trunc, "\"assets\":{");
    {
      int wrote = 0;
      if (act->large_key && act->large_key[0]) {
        json_copy(esc, sizeof(esc), act->large_key);
        off = gw_append(out, cap, off, &trunc, "\"large_image\":\"%s\",", esc);
        wrote = 1;
      }
      if (act->large_text && act->large_text[0]) {
        json_copy(esc, sizeof(esc), act->large_text);
        off = gw_append(out, cap, off, &trunc, "\"large_text\":\"%s\",", esc);
        wrote = 1;
      }
      if (act->small_key && act->small_key[0]) {
        json_copy(esc, sizeof(esc), act->small_key);
        off = gw_append(out, cap, off, &trunc, "\"small_image\":\"%s\",", esc);
        wrote = 1;
      }
      if (act->small_text && act->small_text[0]) {
        json_copy(esc, sizeof(esc), act->small_text);
        off = gw_append(out, cap, off, &trunc, "\"small_text\":\"%s\",", esc);
        wrote = 1;
      }
      if (wrote && off > 0 && out[off - 1] == ',') {
        off--;
        out[off] = 0;
      }
    }
    off = gw_append(out, cap, off, &trunc, "},\"flags\":0}]");
    if (published && pubcap) snprintf(published, pubcap, "%s", name);
  } else {
    off = gw_append(out, cap, off, &trunc, "]");
  }
  off = gw_append(out, cap, off, &trunc,
                  ",\"status\":\"%s\",\"afk\":false,\"flags\":0}}",
                  status && status[0] ? status : "online");

  if (trunc) {
    if (published && pubcap) published[0] = 0;
    snprintf(out, cap,
             "{\"op\":3,\"d\":{\"since\":0,\"activities\":[],"
             "\"status\":\"%s\",\"afk\":false,\"flags\":0}}",
             status && status[0] ? status : "online");
  }
  return 0;
}

static void reload_config(char *status, size_t status_cap, char *mode,
                          size_t mode_cap, char *name, size_t name_cap,
                          char *details, size_t details_cap, char *state,
                          size_t state_cap, char *large_key,
                          size_t large_key_cap, char *large_text,
                          size_t large_text_cap, char *small_key,
                          size_t small_key_cap, char *small_text,
                          size_t small_text_cap) {
  cfg_get("status", status, status_cap);
  cfg_get("mode", mode, mode_cap);
  cfg_get("name", name, name_cap);
  cfg_get("details", details, details_cap);
  cfg_get("state", state, state_cap);
  cfg_get("asset_key", large_key, large_key_cap);
  cfg_get("asset_text", large_text, large_text_cap);
  cfg_get("asset_small_key", small_key, small_key_cap);
  cfg_get("asset_small_text", small_text, small_text_cap);
}

static void detect_activity(gw_activity *act, int src_game, int show_platform,
                            int show_artwork, const char *manual_name,
                            const char *manual_details,
                            const char *manual_state,
                            const char *large_key, const char *large_text,
                            const char *small_key, const char *small_text) {
  static char name[192], details[192], state[192], lt[128], st[128];
  ps5_app_t app;

  memset(act, 0, sizeof(*act));
  act->type = 0;

  if (presence_foreground(&app) && app.title_id[0] && src_game) {
    act->has_game = 1;
    act->name = app.name;
    act->start_epoch = app.start_epoch;
    snprintf(details, sizeof(details), "%s",
             show_platform ? "PlayStation 5" : app.title_id);
    act->details = details;
    act->state = "";
    if (show_artwork && large_key && large_key[0]) {
      act->large_key = large_key;
      act->large_text = large_text && large_text[0] ? large_text : app.name;
    } else if (app.name[0]) {
      act->large_text = app.name;
    }
    act->small_key = small_key;
    act->small_text = small_text;
    return;
  }

  if (manual_name && manual_name[0]) {
    act->has_game = 1;
    act->name = manual_name;
    act->details = manual_details;
    act->state = manual_state;
    act->large_key = large_key;
    act->large_text = large_text;
    act->small_key = small_key;
    act->small_text = small_text;
    (void)name;
    (void)state;
    (void)lt;
    (void)st;
  }
}

static int connect_ws(CURL *easy, const char *url) {
  struct curl_slist *hdrs = NULL;
  char props[512];
  char b64[768];
  CURLcode rc;
  char err[256];

  build_properties(props, sizeof(props));
  base64(props, b64, sizeof(b64));

  hdrs = curl_slist_append(hdrs, "Origin: " GW_ORIGIN);
  if (hdrs)
    hdrs = curl_slist_append(hdrs, "User-Agent: " GW_USER_AGENT);
  if (hdrs) {
    char buf[800];
    snprintf(buf, sizeof(buf), "X-Super-Properties: %s", b64);
    hdrs = curl_slist_append(hdrs, buf);
  }

  curl_easy_setopt(easy, CURLOPT_URL, url);
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
  err[0] = 0;
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, err);

  rc = curl_easy_perform(easy);
  if (rc != CURLE_OK) {
    printf("drpc5: gateway connect failed: %s\n",
           err[0] ? err : curl_easy_strerror(rc));
    curl_slist_free_all(hdrs);
    return -1;
  }
  curl_slist_free_all(hdrs);
  return 0;
}

static int send_identify(CURL *easy, const char *token) {
  char frame[GW_FRAME];
  char tok[TOKEN_MAX * 2];
  char props[512];

  json_copy(tok, sizeof(tok), token);
  build_properties(props, sizeof(props));
  snprintf(frame, sizeof(frame),
           "{\"op\":2,\"d\":{\"token\":\"%s\",\"capabilities\":%d,"
           "\"compress\":false,\"largeThreshold\":100,\"properties\":%s}}",
           tok, GW_CAPABILITIES, props);
  return ws_send(easy, frame);
}

static int send_resume(CURL *easy, const char *token, const char *session_id,
                       long seq) {
  char frame[GW_FRAME];
  char tok[TOKEN_MAX * 2];
  char sid[128];

  json_copy(tok, sizeof(tok), token);
  json_copy(sid, sizeof(sid), session_id ? session_id : "");
  snprintf(frame, sizeof(frame),
           "{\"op\":6,\"d\":{\"token\":\"%s\",\"session_id\":\"%s\",\"seq\":%ld}}",
           tok, sid, seq < 0 ? 0 : seq);
  return ws_send(easy, frame);
}

static void gw_build_url(char *out, size_t cap, const char *base) {
  size_t len;

  if (!base || !base[0]) {
    snprintf(out, cap, "%s", GW_URL GW_QUERY);
    return;
  }
  snprintf(out, cap, "%s", base);
  len = strlen(out);
  if (len == 0) return;
  if (out[len - 1] == '/') {
    if (len + sizeof(GW_QUERY_NOSLASH) < cap)
      snprintf(out + len, cap - len, "%s", GW_QUERY_NOSLASH);
  } else if (len + sizeof(GW_QUERY) < cap) {
    snprintf(out + len, cap - len, "%s", GW_QUERY);
  }
}

static int send_heartbeat(CURL *easy) {
  static const char frame[] = "{\"op\":1,\"d\":null}";
  return ws_send(easy, frame);
}

static int clear_presence(CURL *easy) {
  static const char frame[] =
      "{\"op\":3,\"d\":{\"since\":0,\"activities\":[],\"status\":\"dnd\","
      "\"afk\":false,\"flags\":0}}";
  return ws_send(easy, frame);
}

static int non_resumable(int code) {
  return code == 4004 || code == 4010 || code == 4011 || code == 4012 ||
         code == 4013 || code == 4014;
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
  char buf[GW_FRAME];
  curl_socket_t fd = (curl_socket_t)-1;
  char hb_url[GW_URL_MAX];
  char hb_sid[128];
  long hb_seq = last_seq;
  long hb_ms = GW_HB_FLOOR;
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

  hb_ms = GW_HB_FLOOR;
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

        cr = curl_ws_recv(easy, buf, sizeof(buf) - 1, &n, &meta);
        if (cr != CURLE_OK) {
          if (cr == CURLE_AGAIN) break;
          rc = GW_RET_RETRY;
          break;
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) {
          close_code = meta->close_code;
          if (non_resumable(close_code)) {
            printf("drpc5: gateway closed with fatal code %d\n", close_code);
            if (close_code == 4004)
              printf("drpc5: token rejected, sign in again\n");
            rc = GW_RET_FATAL;
          } else {
            rc = GW_RET_RETRY;
          }
          break;
        }
        if (!(meta && (meta->flags & CURLWS_TEXT))) continue;
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        buf[n] = 0;

        if (frame_op(buf, &op) != 0) continue;

        if (op == 10) {
          long iv = frame_long(buf, "\"heartbeat_interval\":", 0);
          if (iv > 0) hb_ms = iv;
          if (hb_ms < GW_HB_FLOOR) hb_ms = GW_HB_FLOOR;
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
        } else if (op == 11) {
          acked = 1;
        } else if (op == 0) {
          long s = frame_long(buf, "\"s\":", -1);
          if (s >= 0) hb_seq = s;
          if (frame_has(buf, "\"t\":\"READY\"") ||
              frame_has(buf, "\"t\": \"READY\"")) {
            char sid[128], rurl[GW_URL_MAX];
            frame_str(buf, "\"session_id\":\"", sid, sizeof(sid));
            frame_str(buf, "\"resume_gateway_url\":\"", rurl, sizeof(rurl));
            snprintf(hb_sid, sizeof(hb_sid), "%s", sid);
            snprintf(hb_url, sizeof(hb_url), "%s", rurl);
            hb_seq = s >= 0 ? s : hb_seq;
            got_ready = 1;
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
          reidentify_at = now_ms() + 150;
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
    next_check = now + GW_CHECK_MS;

    {
      char status[32], mode[32], name[192], details[192], state[192];
      char lk[128], lt[128], sk[128], st[128];
      gw_activity act;
      char frame[GW_FRAME];

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