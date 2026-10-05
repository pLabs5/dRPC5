#define _GNU_SOURCE

#include "psn.h"
#include "curl_api.h"
#include "core/config.h"
#include "core/curlx.h"
#include "core/util.h"
#include "paths.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PSN_PAGE (1 << 20)
#define PSN_RETRY_MS 30000

static const char *kDnsServers[] = {"1.1.1.1", "8.8.8.8", "9.9.9.9"};
static const char *kPinnedHosts[] = {"store.playstation.com",
                                     "image.api.playstation.com",
                                     "vulcan.dl.playstation.net",
                                     "discord.com"};
static const char *kArtHosts[] = {"image.api.playstation.com",
                                  "vulcan.dl.playstation.net"};
static const char *kUa =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_pending[16];
static char g_last_try[16];
static long long g_last_ms;
static psn_proxy_fn g_proxy;

static int dns_skip_name(const unsigned char *r, size_t n, size_t *off) {
  while (*off < n) {
    unsigned char c = r[*off];
    if ((c & 0xc0) == 0xc0) {
      *off += 2;
      return 0;
    }
    if (c == 0) {
      *off += 1;
      return 0;
    }
    *off += 1u + c;
  }
  return -1;
}

static int dns_parse_a(const unsigned char *r, size_t n, char *ip, size_t cap) {
  uint16_t qd, an, i;
  size_t off = 12;

  if (n < 12) return -1;
  qd = (uint16_t)((r[4] << 8) | r[5]);
  an = (uint16_t)((r[6] << 8) | r[7]);

  for (i = 0; i < qd; i++) {
    if (dns_skip_name(r, n, &off) != 0) return -1;
    if (off + 4 > n) return -1;
    off += 4;
  }
  for (i = 0; i < an && off < n; i++) {
    uint16_t type, cls, rdlen;

    if (dns_skip_name(r, n, &off) != 0) return -1;
    if (off + 10 > n) return -1;
    type = (uint16_t)((r[off] << 8) | r[off + 1]);
    cls = (uint16_t)((r[off + 2] << 8) | r[off + 3]);
    rdlen = (uint16_t)((r[off + 8] << 8) | r[off + 9]);
    off += 10;
    if (off + rdlen > n) return -1;
    if (type == 1 && cls == 1 && rdlen == 4 &&
        (r[off] | r[off + 1] | r[off + 2] | r[off + 3])) {
      snprintf(ip, cap, "%u.%u.%u.%u", (unsigned)r[off], (unsigned)r[off + 1],
               (unsigned)r[off + 2], (unsigned)r[off + 3]);
      return 0;
    }
    off += rdlen;
  }
  return -1;
}

static int dns_a(const char *host, char *ip, size_t cap) {
  unsigned char q[512], r[2048];
  struct sockaddr_in sin;
  struct pollfd pfd;
  size_t off = 12, i = 0, hlen = strlen(host), n;
  int s, fd;

  if (hlen == 0 || hlen > 200) return -1;
  memset(q, 0, sizeof(q));
  q[0] = 0x51;
  q[1] = 0xc3;
  q[2] = 0x01;
  q[5] = 0x01;

  while (i < hlen) {
    const char *dot = (const char *)memchr(host + i, '.', hlen - i);
    size_t lab = dot ? (size_t)(dot - (host + i)) : (hlen - i);

    if (lab == 0 || lab > 63 || off + lab + 1 >= sizeof(q)) return -1;
    q[off++] = (unsigned char)lab;
    memcpy(q + off, host + i, lab);
    off += lab;
    i += lab;
    if (i < hlen) i++;
  }
  if (off + 5 >= sizeof(q)) return -1;
  q[off++] = 0;
  q[off++] = 0;
  q[off++] = 1;
  q[off++] = 0;
  q[off++] = 1;

  for (s = 0; s < (int)(sizeof(kDnsServers) / sizeof(kDnsServers[0])); s++) {
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(53);
    sin.sin_addr.s_addr = inet_addr(kDnsServers[s]);

    if (sendto(fd, q, off, 0, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
      close(fd);
      continue;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 3000) <= 0) {
      close(fd);
      continue;
    }
    n = (size_t)recv(fd, r, sizeof(r), 0);
    close(fd);
    if (n >= 12 && r[0] == q[0] && r[1] == q[1] &&
        dns_parse_a(r, n, ip, cap) == 0)
      return 0;
  }
  return -1;
}

static struct curl_slist *pin_hosts(void) {
  struct curl_slist *l = NULL;
  char pin[160], ip[32];
  int i;

  for (i = 0; i < (int)(sizeof(kPinnedHosts) / sizeof(kPinnedHosts[0])); i++) {
    if (dns_a(kPinnedHosts[i], ip, sizeof(ip)) != 0) continue;
    snprintf(pin, sizeof(pin), "%s:443:%s", kPinnedHosts[i], ip);
    l = curl_slist_append(l, pin);
  }
  return l;
}

void psn_set_proxy(psn_proxy_fn fn) {
  g_proxy = fn;
}

struct curl_slist *psn_pinned_hosts(void) {
  return pin_hosts();
}

static int psn_get(const char *url, int head, long *code, char *ct,
                   size_t ctc, char *body) {
  struct curl_slist *hdrs = NULL;
struct curl_slist *pins;
    CURL *h;
    CURLcode rc;
    long c = 0;
    curlx_buf sink;

  if (code) *code = 0;
  if (ct && ctc) ct[0] = 0;
  if (body) body[0] = 0;

  pins = pin_hosts();
  h = curl_easy_init();
  if (!h) {
    curl_slist_free_all(pins);
    return -1;
  }

  hdrs = curl_slist_append(hdrs, "Accept: text/html,image/png,image/jpeg,*/*");
  hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9");

  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_USERAGENT, kUa);
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(h, CURLOPT_RESOLVE, pins);
  if (head)
    curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
  else {
    sink.buf = body;
    sink.cap = PSN_PAGE - 1;
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curlx_buf_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
  }
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_setup(h, 15000L, 25000L);

  rc = curl_easy_perform(h);
  if (rc == CURLE_OK) {
    char *t = NULL;

    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &c);
    if (code) *code = c;
    if (ct && ctc &&
        curl_easy_getinfo(h, CURLINFO_CONTENT_TYPE, &t) == CURLE_OK && t)
      snprintf(ct, ctc, "%s", t);
  }

  curl_easy_cleanup(h);
  curl_slist_free_all(hdrs);
  curl_slist_free_all(pins);
  return rc == CURLE_OK ? 0 : -1;
}

static void unescape(char *s) {
  char *w = s;

  while (*s) {
    if (*s == '\\' && s[1]) {
      s++;
      *w++ = *s++;
      continue;
    }
    *w++ = *s++;
  }
  *w = 0;
}

static int grab_after(const char *from, const char *key, char *out,
                      size_t cap) {
  const char *p = strstr(from, key);
  size_t i = 0;

  if (!p) return -1;
  p += strlen(key);
  while (*p && *p != '"') {
    if (i + 1 >= cap) return -1;
    out[i++] = *p++;
  }
  out[i] = 0;
  return i ? 0 : -1;
}

static int host_ok(const char *url) {
  const char *host;
  int i;

  if (strncmp(url, "https://", 8) != 0) return 0;
  host = url + 8;
  for (i = 0; i < (int)(sizeof(kArtHosts) / sizeof(kArtHosts[0])); i++) {
    size_t n = strlen(kArtHosts[i]);

    if (strncmp(host, kArtHosts[i], n) == 0 &&
        (host[n] == 0 || host[n] == ':' || host[n] == '/'))
      return 1;
  }
  return 0;
}

static int is_raster(const char *ct) {
  return ct && (strncmp(ct, "image/png", 9) == 0 ||
                strncmp(ct, "image/jpeg", 10) == 0);
}

static int host_of(const char *url, char *out, size_t cap) {
  const char *start = url + 8;
  const char *end = strchr(start, '/');
  size_t n = end ? (size_t)(end - start) : strlen(start);

  if (n == 0 || n >= cap) return -1;
  memcpy(out, start, n);
  out[n] = 0;
  return 0;
}

static int verify_image(const char *url) {
  char host[128], ct[64];
  long code = 0;

  if (!host_ok(url)) return -1;
  if (host_of(url, host, sizeof(host)) != 0) return -1;
  if (psn_get(url, 1, &code, ct, sizeof(ct), NULL) != 0) return -1;
  if (code < 200 || code >= 300) return -1;
  return is_raster(ct) ? 0 : -1;
}

static int extract_art(char *html, char *url, size_t cap) {
  const char *m;

  unescape(html);
  m = strstr(html, "\"role\":\"MASTER\"");
  if (m && grab_after(m, "\"url\":\"", url, cap) == 0 && host_ok(url))
    return 0;
  m = strstr(html, "\"image\":\"");
  if (m && grab_after(m, "\"image\":\"", url, cap) == 0 && host_ok(url))
    return 0;
  return -1;
}

static void pick_locales(const char *content_id, char *first, size_t cap,
                         char *second, size_t cap2) {
  char def[8];

  snprintf(def, sizeof(def), "en-us");
  if (content_id && content_id[0] == 'E')
    snprintf(def, sizeof(def), "en-gb");
  else if (content_id && content_id[0] == 'J')
    snprintf(def, sizeof(def), "ja-jp");

  snprintf(first, cap, "%s", def);
  snprintf(second, cap2, "%s",
           strcmp(def, "en-gb") == 0 ? "en-us" : "en-gb");
}

static int fetch_art(const char *concept_id, const char *content_id,
                     char *url, size_t cap) {
  char *page;
  char loc[8], loc2[8];
  char page_url[256];
  const char *locs[2];
  int i;

  if (!concept_id || !concept_id[0]) return -1;
  page = calloc(1, PSN_PAGE);
  if (!page) return -1;

  pick_locales(content_id, loc, sizeof(loc), loc2, sizeof(loc2));
  locs[0] = loc;
  locs[1] = loc2;

  for (i = 0; i < 2; i++) {
    snprintf(page_url, sizeof(page_url),
             "https://store.playstation.com/%s/concept/%s", locs[i], concept_id);
    if (psn_get(page_url, 0, NULL, NULL, 0, page) != 0) continue;
    if (extract_art(page, url, cap) != 0) continue;
    if (verify_image(url) == 0) {
      free(page);
      return 0;
    }
  }

  free(page);
  return -1;
}

static int cache_get(const char *title_id, char *out, size_t cap) {
  char line[512];
  size_t n = strlen(title_id);
  FILE *f = fopen(ICONS_PATH, "rb");

  if (!f) return -1;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, title_id, n) != 0 || line[n] != '=') continue;
    {
      char *v = line + n + 1;
      char *e = strchr(v, '\n');

      if (e) *e = 0;
      e = strchr(v, '\r');
      if (e) *e = 0;
      if (*v) {
        snprintf(out, cap, "%s", v);
        fclose(f);
        return 0;
      }
    }
  }
  fclose(f);
  return -1;
}

static void cache_put(const char *title_id, const char *url) {
  char line[512];
  size_t n = strlen(title_id);
  FILE *f;

  pthread_mutex_lock(&g_mu);
  f = fopen(ICONS_PATH, "rb");
  if (f) {
    FILE *t = fopen(ICONS_PATH ".tmp", "wb");

    if (t) {
      while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, title_id, n) == 0 && line[n] == '=') continue;
        fputs(line, t);
      }
      fprintf(t, "%s=%s\n", title_id, url);
      fclose(t);
      fclose(f);
      rename(ICONS_PATH ".tmp", ICONS_PATH);
    } else {
      fclose(f);
    }
  } else {
    f = fopen(ICONS_PATH, "ab");
    if (f) {
      fprintf(f, "%s=%s\n", title_id, url);
      fclose(f);
    }
  }
  pthread_mutex_unlock(&g_mu);
}

struct psn_job {
  char title_id[16];
  char concept_id[16];
  char content_id[48];
};

static void *psn_worker(void *arg) {
  struct psn_job job = *(struct psn_job *)arg;
  char url[320], cached[320], proxied[400];

  free(arg);
  url[0] = 0;
  cached[0] = 0;

  if (cache_get(job.title_id, cached, sizeof(cached)) == 0 &&
      strncmp(cached, "mp:", 3) != 0) {
    snprintf(url, sizeof(url), "%s", cached);
  } else if (fetch_art(job.concept_id, job.content_id, url, sizeof(url)) != 0) {
    dlogf("drpc5: art %s lookup failed\n", job.title_id);
    goto done;
  }

  if (g_proxy && g_proxy(url, proxied, sizeof(proxied)) == 0) {
    dlogf("drpc5: art %s %s\n", job.title_id, proxied);
    cache_put(job.title_id, proxied);
  } else if (strcmp(url, cached) != 0) {
    dlogf("drpc5: art %s raw %s\n", job.title_id, url);
    cache_put(job.title_id, url);
  } else {
    dlogf("drpc5: art %s proxy retry\n", job.title_id);
  }

done:
  pthread_mutex_lock(&g_mu);
  g_pending[0] = 0;
  pthread_mutex_unlock(&g_mu);
  return NULL;
}

int psn_art_cached(const char *title_id, char *out, size_t cap) {
  if (!title_id || !title_id[0] || !out || cap == 0) return -1;
  out[0] = 0;
  return cache_get(title_id, out, cap);
}

void psn_art_refresh_async(const char *title_id, const char *concept_id,
                           const char *content_id) {
  struct psn_job *job;
  pthread_t th;
  long long now;

  if (!title_id || !title_id[0]) return;

  now = mono_ms();
  pthread_mutex_lock(&g_mu);
  if (g_pending[0] || (g_last_try[0] && !strcmp(g_last_try, title_id) &&
                       now - g_last_ms < cfg_clamped("psn_retry_ms", 30000, 1000, 3600000))) {
    pthread_mutex_unlock(&g_mu);
    return;
  }
  snprintf(g_pending, sizeof(g_pending), "%s", title_id);
  snprintf(g_last_try, sizeof(g_last_try), "%s", title_id);
  g_last_ms = now;
  pthread_mutex_unlock(&g_mu);

  job = calloc(1, sizeof(*job));
  if (!job) {
    pthread_mutex_lock(&g_mu);
    g_pending[0] = 0;
    pthread_mutex_unlock(&g_mu);
    return;
  }
  snprintf(job->title_id, sizeof(job->title_id), "%s", title_id);
  if (concept_id)
    snprintf(job->concept_id, sizeof(job->concept_id), "%s", concept_id);
  if (content_id)
    snprintf(job->content_id, sizeof(job->content_id), "%s", content_id);

  if (pthread_create(&th, NULL, psn_worker, job) != 0) {
    free(job);
    pthread_mutex_lock(&g_mu);
    g_pending[0] = 0;
    pthread_mutex_unlock(&g_mu);
    return;
  }
  pthread_detach(th);
}
