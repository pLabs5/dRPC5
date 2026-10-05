/* Stage 2: turning a concept id into a direct image URL.

   The store page only references its icon indirectly, so this walks the
   returned HTML, keeps only URLs on the PlayStation image hosts, checks the
   content type is a raster image, and HEADs it before it is trusted. A
   non-raster or redirected URL is dropped rather than cached. */
#define _GNU_SOURCE
#include "psn/psn.h"
#include "psn/internal.h"

#include "core/curl_api.h"
#include "core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kArtHosts[] = {"image.api.playstation.com",
                                  "vulcan.dl.playstation.net"};
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

int fetch_art(const char *concept_id, const char *content_id,
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
