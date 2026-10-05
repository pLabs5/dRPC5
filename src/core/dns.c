/* A minimal A-record resolver, and the libcurl glue that makes every request
   in the payload use it.

   Scope is deliberately small: IPv4 only, plain UDP/53, one question per
   query, no TCP retry, no DNSSEC, no negative-answer handling beyond a short
   cache. Everything it hands back is checked before use, because the answer
   arrives as untrusted bytes off the network and the whole point of this file
   is to decide which names we are willing to connect to. */
#define _GNU_SOURCE
#include "core/dns.h"

#include "core/config.h"
#include "core/util.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Overridable so the resolver can be pointed at a local one (and so the host
   test harness can run without privileges to bind 53). */
#ifndef DNS_PORT
#define DNS_PORT 53
#endif

#define DNS_MAX_SERVERS 4
#define DNS_QUERY_MAX 512
#define DNS_REPLY_MAX 2048
#define DNS_TIMEOUT_MS 3000
#define DNS_HOST_MAX 200
#define DNS_HOSTS_MAX 128

/* Answers are stable for far longer than this, but pinning a stale address for
   a few minutes is cheaper than re-querying on every request. Failures are
   cached for far less time, so a resolver that was down recovers quickly
   instead of being written off until the next reboot. */
#define DNS_TTL_S 300
#define DNS_FAIL_TTL_S 15

#define DNS_CACHE_SLOTS 8

static const char *kDnsDefaults[] = {"1.1.1.1", "8.8.8.8", "9.9.9.9"};

struct dns_slot {
  char host[DNS_HOSTS_MAX];
  char ip[32];
  long long at_ms;
  int ok;
};

static struct dns_slot g_cache[DNS_CACHE_SLOTS];
static pthread_mutex_t g_cache_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_next_slot;

/* The resolver list is read per query so a config change takes effect without
   a restart, but it is bounded and validated rather than trusted: anything
   that is not a dotted quad or a name is dropped. */
static int dns_servers(char out[][DNS_HOSTS_MAX], int max) {
  char cfg[512], list[512];
  char *tok, *save;
  int n = 0, i;

  out[0][0] = 0;
  if (cfg_get("dns_servers", cfg, sizeof(cfg)) == 0 && cfg[0])
    snprintf(list, sizeof(list), "%s", cfg);

  for (tok = strtok_r(list, ", \t", &save); tok && n < max;
       tok = strtok_r(NULL, ", \t", &save)) {
    struct in_addr probe;

    if (inet_pton(AF_INET, tok, &probe) == 1)
      snprintf(out[n++], DNS_HOSTS_MAX, "%s", tok);
  }
  if (n > 0) return n;

  for (i = 0; i < (int)(sizeof(kDnsDefaults) / sizeof(kDnsDefaults[0])) &&
                  i < max;
       i++)
    snprintf(out[n++], DNS_HOSTS_MAX, "%s", kDnsDefaults[i]);
  return n;
}

/* Walk a (possibly compressed) name, leaving *off just past it. Returns -1 if
   the name runs off the end of the message, which is how a truncated or
   hostile reply gets rejected. */
static int dns_skip_name(const unsigned char *r, size_t n, size_t *off) {
  while (*off < n) {
    unsigned char c = r[*off];

    if ((c & 0xc0) == 0xc0) {
      *off += 2;
      return *off <= n ? 0 : -1;
    }
    if (c == 0) {
      *off += 1;
      return 0;
    }
    *off += 1u + c;
  }
  return -1;
}

/* Every field is bounds-checked before it is read or trusted, and an answer of
   0.0.0.0 is treated as no answer: it is what a sinkholed resolver returns,
   and connecting to it would silently black-hole the request. */
static int dns_parse_a(const unsigned char *r, size_t n, char *ip, size_t cap) {
  uint16_t qd, an, i;
  size_t off = 12;

  if (n < 12) return -1;
  if ((r[2] & 0x80) == 0) return -1; /* not a response */
  if ((r[3] & 0x0f) != 0) return -1; /* rcode != NOERROR */
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
    /* A CNAME in the answer chain is fine - the A record that matters is
       usually in the same reply - so keep scanning rather than giving up. */
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

/* Standard recursive query. RD is set so the resolver does the walking for us,
   and the question count is exactly one. */
static int dns_build_query(const char *host, unsigned char *q, size_t cap,
                           size_t *qlen) {
  size_t hlen = strlen(host), off = 12, i = 0;

  if (hlen == 0 || hlen > DNS_HOST_MAX) return -1;
  memset(q, 0, cap);
  q[0] = 0x51;
  q[1] = 0xc3;
  q[2] = 0x01;
  q[5] = 0x01;

  while (i < hlen) {
    const char *dot = (const char *)memchr(host + i, '.', hlen - i);
    size_t lab = dot ? (size_t)(dot - (host + i)) : (hlen - i);

    if (lab == 0 || lab > 63 || off + lab + 1 >= cap) return -1;
    q[off++] = (unsigned char)lab;
    memcpy(q + off, host + i, lab);
    off += lab;
    i += lab;
    if (i < hlen) i++;
  }
  if (off + 5 >= cap) return -1;
  q[off++] = 0;      /* end of the name */
  q[off++] = 0;      /* QTYPE  A */
  q[off++] = 1;
  q[off++] = 0;      /* QCLASS IN */
  q[off++] = 1;
  *qlen = off;
  return 0;
}

/* One round trip to one resolver. The transaction id is echoed back and
   checked, so a stray or spoofed reply to the wrong question is discarded
   rather than parsed. */
static int dns_ask(const char *server, const unsigned char *q, size_t qlen,
                   unsigned char *r, size_t rcap) {
  struct sockaddr_in sin;
  struct pollfd pfd;
  size_t n;
  int fd;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(DNS_PORT);
  if (inet_pton(AF_INET, server, &sin.sin_addr) != 1) {
    close(fd);
    return -1;
  }

  if (sendto(fd, q, qlen, 0, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
    close(fd);
    return -1;
  }
  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  /* A dead resolver must not hold up a connect; try the next one instead. */
  if (poll(&pfd, 1, DNS_TIMEOUT_MS) <= 0) {
    close(fd);
    return -1;
  }
  n = (size_t)recv(fd, r, rcap, 0);
  close(fd);
  if (n < 12 || r[0] != q[0] || r[1] != q[1]) return -1;
  return (int)n;
}

static int dns_exchange(const char *host, char *ip, size_t cap) {
  char servers[DNS_MAX_SERVERS][DNS_HOSTS_MAX];
  unsigned char q[DNS_QUERY_MAX], r[DNS_REPLY_MAX];
  size_t qlen = 0;
  int n, i;

  if (dns_build_query(host, q, sizeof(q), &qlen) != 0) return -1;
  n = dns_servers(servers, DNS_MAX_SERVERS);
  for (i = 0; i < n; i++) {
    int got = dns_ask(servers[i], q, qlen, r, sizeof(r));

    if (got > 0 && dns_parse_a(r, (size_t)got, ip, cap) == 0) return 0;
  }
  return -1;
}

int dns_resolve(const char *host, char *ip, size_t cap) {
  long long now;
  unsigned i;
  int rc = -1, fresh = 0;

  if (!host || !host[0] || cap < 16) return -1;
  if (strlen(host) >= DNS_HOSTS_MAX) return -1;

  now = mono_ms();
  pthread_mutex_lock(&g_cache_mu);
  for (i = 0; i < DNS_CACHE_SLOTS; i++) {
    struct dns_slot *s = &g_cache[i];
    long long ttl = s->ok ? DNS_TTL_S * 1000LL : DNS_FAIL_TTL_S * 1000LL;

    if (!s->host[0] || strcmp(s->host, host) != 0) continue;
    if (now - s->at_ms >= ttl) break; /* stale: re-query below */
    if (s->ok) {
      snprintf(ip, cap, "%s", s->ip);
      rc = 0;
    }
    /* A remembered failure counts too. Without this the entry would only stop
       the answer being reused, and every request would ask again - which is the
       query loop the cache exists to prevent, and the worst thing to do to a
       resolver that is already struggling. */
    fresh = 1;
    break;
  }
  pthread_mutex_unlock(&g_cache_mu);
  if (fresh) {
    if (rc != 0) ip[0] = 0;
    return rc;
  }

  if (dns_exchange(host, ip, cap) != 0) {
    dlogf("drpc5: dns %s failed, falling back\n", host);
    pthread_mutex_lock(&g_cache_mu);
    {
      struct dns_slot *s = &g_cache[g_next_slot % DNS_CACHE_SLOTS];
      g_next_slot++;
      snprintf(s->host, sizeof(s->host), "%s", host);
      s->ip[0] = 0;
      s->at_ms = now;
      s->ok = 0;
    }
    pthread_mutex_unlock(&g_cache_mu);
    ip[0] = 0;
    return -1;
  }

  pthread_mutex_lock(&g_cache_mu);
  {
    struct dns_slot *s = &g_cache[g_next_slot % DNS_CACHE_SLOTS];
    g_next_slot++;
    snprintf(s->host, sizeof(s->host), "%s", host);
    snprintf(s->ip, sizeof(s->ip), "%s", ip);
    s->at_ms = now;
    s->ok = 1;
  }
  pthread_mutex_unlock(&g_cache_mu);
  return 0;
}

struct curl_slist *dns_pin(const char *host, int port) {
  char ip[32], pin[DNS_HOSTS_MAX + 32];

  if (!host || !host[0] || port <= 0 || port > 65535) return NULL;
  if (dns_resolve(host, ip, sizeof(ip)) != 0) return NULL;
  snprintf(pin, sizeof(pin), "%s:%d:%s", host, port, ip);
  return curl_slist_append(NULL, pin);
}

/* A label is what a hostname is actually made of. Rejecting anything else here
   means a string that is not a URL at all never becomes a DNS question: the
   resolver is only ever asked about names that could be real. */
static int is_host_char(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
}

/* Host and port out of a URL string. Our URLs never carry credentials, but
   userinfo is skipped anyway so a redirect cannot smuggle one past this. */
static int url_host_port(const char *url, char *out, size_t cap, int *port) {
  const char *p, *e, *colon;
  size_t n, i;

  *port = 443;
  if (!url) return -1;
  p = strstr(url, "://");
  p = p ? p + 3 : url;
  if ((e = strchr(p, '@')) != NULL &&
      (strchr(p, '/') == NULL || e < strchr(p, '/')))
    p = e + 1;

  e = p;
  while (*e && *e != '/' && *e != '?' && *e != '#') e++;
  colon = memchr(p, ':', (size_t)(e - p));
  if (colon) {
    *port = atoi(colon + 1);
    if (*port <= 0 || *port > 65535) return -1;
    n = (size_t)(colon - p);
  } else {
    n = (size_t)(e - p);
  }
  if (n == 0 || n >= cap) return -1;
  for (i = 0; i < n; i++) {
    if (!is_host_char((unsigned char)p[i])) return -1;
  }
  /* A name cannot begin or end with a dot, and cannot have two in a row: those
     are the shapes a name has when it is really a fragment of something else. */
  if (p[0] == '.' || p[n - 1] == '.') return -1;
  for (i = 1; i + 1 < n; i++) {
    if (p[i] == '.' && p[i + 1] == '.') return -1;
  }
  memcpy(out, p, n);
  out[n] = 0;
  return 0;
}

struct curl_slist *dns_pin_url(const char *url) {
  char host[DNS_HOSTS_MAX];
  int port;

  if (url_host_port(url, host, sizeof(host), &port) != 0) return NULL;
  return dns_pin(host, port);
}
