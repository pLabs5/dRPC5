#define _GNU_SOURCE
#include "gw_internal.h"

#include "core/config.h"
#include "core/json.h"
#include "core/util.h"
#include "paths.h"
#include "presence.h"
#include "psn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static size_t http_body_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
  char *buf = (char *)ud;
  size_t want = size * nmemb;
  size_t used = strlen(buf);

  if (used + want >= 4095) return 0;
  memcpy(buf + used, ptr, want);
  buf[used + want] = 0;
  return want;
}

int proxy_external_asset(const char *url, char *out, size_t cap) {
  char token[TOKEN_MAX], app_id[32], ep[192], auth[TOKEN_MAX + 32];
  char body[700], esc[600], resp[4096];
  struct curl_slist *hdrs = NULL;
  struct curl_slist *pins;
  CURL *h;
  CURLcode rc;
  const char *arr, *ob, *v;
  long code = 0;
  size_t o = 0, i;

  if (cap == 0) return -1;
  out[0] = 0;
  if (!url || strncmp(url, "https://", 8) != 0) return -1;
  if (read_token(token, sizeof(token)) != 0) return -1;
  if (cfg_get("app_id", app_id, sizeof(app_id)) != 0 || !app_id[0]) return -1;

  for (i = 0; url[i] && o + 7 < sizeof(esc); i++) {
    char c = url[i];

    if (c == '"' || c == '\\') {
      esc[o++] = '\\';
      esc[o++] = c;
    } else if ((unsigned char)c < 0x20) {
      continue;
    } else {
      esc[o++] = c;
    }
  }
  esc[o] = 0;

  snprintf(body, sizeof(body), "{\"urls\":[\"%s\"]}", esc);
  snprintf(ep, sizeof(ep),
           "https://discord.com/api/v9/applications/%s/external-assets",
           app_id);
  snprintf(auth, sizeof(auth), "Authorization: %s", token);
  resp[0] = 0;

  hdrs = curl_slist_append(hdrs, auth);
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, "User-Agent: " GW_USER_AGENT);
  pins = psn_pinned_hosts();

  h = curl_easy_init();
  if (!h) {
    curl_slist_free_all(hdrs);
    curl_slist_free_all(pins);
    return -1;
  }
  curl_easy_setopt(h, CURLOPT_URL, ep);
  curl_easy_setopt(h, CURLOPT_POST, 1L);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(h, CURLOPT_RESOLVE, pins);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, http_body_cb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, resp);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 25000L);
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(h, CURLOPT_CAINFO, CA_PATH);

  rc = curl_easy_perform(h);
  if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(h);
  curl_slist_free_all(hdrs);
  curl_slist_free_all(pins);

  if (rc != CURLE_OK || code < 200 || code >= 300) {
    printf("drpc5: external-assets rc=%d http=%ld\n", (int)rc, code);
    return -1;
  }

  arr = strchr(resp, '[');
  ob = arr ? strchr(arr, '{') : NULL;
  if (!ob) return -1;
  v = obj_find(ob, "external_asset_path");
  if (json_read_str(v, esc, sizeof(esc)) != 0 || !esc[0]) return -1;
  if (strncmp(esc, "mp:", 3) == 0) {
    snprintf(out, cap, "%s", esc);
  } else {
    char tmp[400];

    snprintf(tmp, sizeof(tmp), "mp:%s", esc);
    snprintf(out, cap, "%s", tmp);
  }
  return 0;
}
