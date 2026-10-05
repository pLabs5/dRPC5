/* Remote Auth sessions: QR / remote login handshakes.

   Each session owns a curl easy handle and a worker thread. The thread polls Discord
   for frames and parks them in a small ring; ra_poll() hands them to the HTTP thread,
   which is what the browser is long-polling. */
#define _GNU_SOURCE

#include "discord/discord.h"
#include "core/curl_api.h"
#include "core/curlx.h"
#include "core/dns.h"
#include "core/util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RA_MAX 4
#define RA_QUEUE 32
#define RA_FRAME 2048

struct ra {
  int used;
  int id;
  int running;
  CURL *easy;
  pthread_t th;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  char frames[RA_QUEUE][RA_FRAME];
  int fh;
  int count;
  long hb_ms;
  long long next_hb;
};
static struct ra g_ra[RA_MAX];
static pthread_mutex_t g_ra_mu = PTHREAD_MUTEX_INITIALIZER;

static void ra_queue_frame_locked(struct ra *s, const char *frame) {
  size_t len;
  char *dst;

  if (s->count >= RA_QUEUE)
    return;
  len = strlen(frame);
  if (len >= RA_FRAME)
    return;
  dst = s->frames[(s->fh + s->count) % RA_QUEUE];
  memcpy(dst, frame, len + 1);
  s->count++;
  pthread_cond_broadcast(&s->cv);
}

static void ra_parse_hello(struct ra *s, const char *frame) {
  const char *p = strstr(frame, "\"heartbeat_interval\":");
  if (!p)
    return;
  p += strlen("\"heartbeat_interval\":");
  while (*p == ' ')
    p++;
  s->hb_ms = strtol(p, NULL, 10);
  if (s->hb_ms < 1000)
    s->hb_ms = 1000;
  s->next_hb = now_ms() + s->hb_ms;
}

static void *ra_thread(void *arg) {
  struct ra *s = (struct ra *)arg;
  curl_socket_t fd = (curl_socket_t)-1;
  char buf[4096];
  int flags;

  curl_easy_getinfo(s->easy, CURLINFO_ACTIVESOCKET, &fd);
  flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  while (__atomic_load_n(&s->running, __ATOMIC_RELAXED)) {
    struct pollfd p;
    long wait = 1000;
    long long now;
    int due;
    int dead = 0;

    if (s->hb_ms > 0) {
      now = now_ms();
      wait = s->next_hb > now ? (long)(s->next_hb - now) : 0;
    }
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    poll(&p, 1, (int)wait);

    now = now_ms();
    due = s->hb_ms > 0 && now >= s->next_hb;
    if (due) {
      static const char hb[] = "{\"op\":\"heartbeat\"}";
      size_t sent = 0;
      pthread_mutex_lock(&s->mu);
      curl_ws_send(s->easy, hb, sizeof(hb) - 1, &sent, sizeof(hb) - 1,
                   CURLWS_TEXT);
      pthread_mutex_unlock(&s->mu);
      s->next_hb = now + s->hb_ms;
    }

    if (p.revents & POLLIN) {
      for (;;) {
        size_t n = 0;
        const struct curl_ws_frame *meta = NULL;
        CURLcode rc;
        pthread_mutex_lock(&s->mu);
        rc = curl_ws_recv(s->easy, buf, sizeof(buf) - 1, &n, &meta);
        pthread_mutex_unlock(&s->mu);
        if (rc != CURLE_OK) {
          if (rc != CURLE_AGAIN) {
            dead = 1;
          }
          break;
        }
        if (meta && (meta->flags & CURLWS_CLOSE)) {
          if (n >= sizeof(buf))
            n = sizeof(buf) - 1;
          buf[n] = 0;
          dead = 1;
          break;
        }
        if (!(meta && (meta->flags & CURLWS_TEXT)))
          continue;
        if (n >= sizeof(buf))
          n = sizeof(buf) - 1;
        buf[n] = 0;
        ra_parse_hello(s, buf);
        pthread_mutex_lock(&s->mu);
        ra_queue_frame_locked(s, buf);
        pthread_mutex_unlock(&s->mu);
      }
    }
    if (dead) {
      break;
    }
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      break;
    }
  }

  pthread_mutex_lock(&s->mu);
  ra_queue_frame_locked(s, "{\"op\":\"_closed\",\"d\":null}");
  pthread_mutex_unlock(&s->mu);
  __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
  return NULL;
}

int ra_start(const char *public_key, char *err, size_t errcap) {
  struct curl_slist *hdrs = NULL;
  struct curl_slist *pins;
  CURL *h;
  CURLcode rc;
  struct ra *s = NULL;
  int i;
  char curl_err[256];

  if (discord_init() != 0) {
    snprintf(err, errcap, "network init failed");
    return -1;
  }

  pthread_mutex_lock(&g_ra_mu);
  for (i = 0; i < RA_MAX; i++) {
    if (!g_ra[i].used) {
      s = &g_ra[i];
      break;
    }
  }
  if (!s) {
    pthread_mutex_unlock(&g_ra_mu);
    snprintf(err, errcap, "too many sessions");
    return -1;
  }
  memset(s, 0, sizeof(*s));
  s->used = 1;
  s->id = i + 1;
  pthread_mutex_init(&s->mu, NULL);
  pthread_cond_init(&s->cv, NULL);
  pthread_mutex_unlock(&g_ra_mu);

  h = curl_easy_init();
  if (!h) {
    snprintf(err, errcap, "curl init failed");
    goto fail;
  }
  s->easy = h;
  hdrs = curl_slist_append(hdrs, "Origin: https://discord.com");
  hdrs = curl_slist_append(
      hdrs,
      "User-Agent: Mozilla/5.0 (PlayStation 5 6.00) AppleWebKit/605.1.15 "
      "(KHTML, like Gecko) dRPC5/1.0");

  pins = dns_pin_url("wss://remote-auth-gateway.discord.gg/?v=2");

  curl_easy_setopt(h, CURLOPT_URL,
                   "wss://remote-auth-gateway.discord.gg/?v=2");
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
  if (pins) curl_easy_setopt(h, CURLOPT_RESOLVE, pins);
curl_easy_setopt(h, CURLOPT_CONNECT_ONLY, 2L);
    curl_setup(h, 20000L, 0);

  curl_err[0] = 0;
  curl_easy_setopt(h, CURLOPT_ERRORBUFFER, curl_err);
  rc = curl_easy_perform(h);
  if (rc != CURLE_OK) {
    snprintf(err, errcap, "ws connect: %s",
             curl_err[0] ? curl_err : curl_easy_strerror(rc));
    curl_slist_free_all(hdrs);
    goto fail;
  }
  /* Connected: stop referencing the pin before freeing it. */
  curl_easy_setopt(h, CURLOPT_RESOLVE, NULL);
  curl_slist_free_all(pins);

  {
    char init[1024];
    size_t sent = 0;
    snprintf(init, sizeof(init),
             "{\"op\":\"init\",\"encoded_public_key\":\"%s\"}", public_key);
    pthread_mutex_lock(&s->mu);
    rc = curl_ws_send(h, init, strlen(init), &sent, (curl_off_t)strlen(init),
                      CURLWS_TEXT);
    pthread_mutex_unlock(&s->mu);
    if (rc != CURLE_OK) {
      snprintf(err, errcap, "ws init send: %s", curl_easy_strerror(rc));
      curl_slist_free_all(hdrs);
      goto fail;
    }
  }

  __atomic_store_n(&s->running, 1, __ATOMIC_RELEASE);
  if (pthread_create(&s->th, NULL, ra_thread, s) != 0) {
    __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
    snprintf(err, errcap, "thread create failed");
    curl_slist_free_all(hdrs);
    goto fail;
  }
  curl_slist_free_all(hdrs);
  return s->id;

fail:
  if (s->easy) {
    curl_easy_cleanup(s->easy);
    s->easy = NULL;
  }
  pthread_cond_destroy(&s->cv);
  pthread_mutex_destroy(&s->mu);
  s->used = 0;
  return -1;
}

static struct ra *ra_get_locked(int id) {
  if (id < 1 || id > RA_MAX)
    return NULL;
  if (!g_ra[id - 1].used || g_ra[id - 1].id != id)
    return NULL;
  return &g_ra[id - 1];
}

int ra_poll(int id, long wait_ms, char *out, size_t cap, int *closed) {
  struct ra *s;
  size_t off = 0;

  pthread_mutex_lock(&g_ra_mu);
  s = ra_get_locked(id);
  pthread_mutex_unlock(&g_ra_mu);
  if (!s)
    return -1;

  if (wait_ms < 0)
    wait_ms = 0;
  if (wait_ms > 60000)
    wait_ms = 60000;

  pthread_mutex_lock(&s->mu);
  if (s->count == 0 && s->running && wait_ms > 0) {
struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts_after_ms(&ts, wait_ms);
    while (s->count == 0 && s->running) {
      if (pthread_cond_timedwait(&s->cv, &s->mu, &ts) == ETIMEDOUT)
        break;
    }
  }

  off += snprintf(out + off, cap - off, "[");
  while (s->count > 0 && off + RA_FRAME + 8 < cap) {
    const char *src = s->frames[s->fh];
    if (off > 1)
      off += snprintf(out + off, cap - off, ",");
    off += snprintf(out + off, cap - off, "\"");
    for (; *src && off + 8 < cap; src++) {
      unsigned char c = (unsigned char)*src;
      if (c == '"' || c == '\\')
        off += snprintf(out + off, cap - off, "\\%c", c);
      else if (c < 0x20)
        off += snprintf(out + off, cap - off, "\\u%04x", c);
      else
        off += snprintf(out + off, cap - off, "%c", c);
    }
    off += snprintf(out + off, cap - off, "\"");
    s->fh = (s->fh + 1) % RA_QUEUE;
    s->count--;
  }
  off += snprintf(out + off, cap - off, "]");
  *closed = !s->running;
  pthread_mutex_unlock(&s->mu);
  return 0;
}

int ra_send(int id, const char *frame) {
  struct ra *s;
  size_t sent = 0;
  CURLcode rc;

  pthread_mutex_lock(&g_ra_mu);
  s = ra_get_locked(id);
  pthread_mutex_unlock(&g_ra_mu);
  if (!s || !s->easy || !s->running)
    return -1;
  if (strlen(frame) > 2048)
    return -1;

  pthread_mutex_lock(&s->mu);
  rc = curl_ws_send(s->easy, frame, strlen(frame), &sent,
                    (curl_off_t)strlen(frame), CURLWS_TEXT);
  pthread_mutex_unlock(&s->mu);
  return rc == CURLE_OK ? 0 : -1;
}

void ra_close(int id) {
  struct ra *s;
  CURL *easy;
  pthread_t th;

  pthread_mutex_lock(&g_ra_mu);
  s = ra_get_locked(id);
  if (!s) {
    pthread_mutex_unlock(&g_ra_mu);
    return;
  }
  easy = s->easy;
  th = s->th;
  __atomic_store_n(&s->running, 0, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&g_ra_mu);

  if (easy) {
    curl_socket_t fd = (curl_socket_t)-1;
    curl_easy_getinfo(easy, CURLINFO_ACTIVESOCKET, &fd);
    if (fd != (curl_socket_t)-1)
      shutdown(fd, SHUT_RDWR);
  }
  pthread_join(th, NULL);
  if (easy)
    curl_easy_cleanup(easy);

  pthread_mutex_lock(&g_ra_mu);
  if (s->easy == easy)
    s->easy = NULL;
  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&g_ra_mu);

  pthread_cond_destroy(&s->cv);
  pthread_mutex_destroy(&s->mu);
  pthread_mutex_lock(&g_ra_mu);
  s->used = 0;
  s->id = 0;
  pthread_mutex_unlock(&g_ra_mu);
}
