/* Stage 1: requests to the PlayStation Store.

   All name resolution is core/dns.c's job now, so this file is only the
   request itself: pinned TLS, a browser-ish user agent, the store's language
   header, and a bounded response buffer. */
#define _GNU_SOURCE
#include "psn/psn.h"
#include "psn/internal.h"

#include "core/config.h"
#include "core/curl_api.h"
#include "core/curlx.h"
#include "core/dns.h"
#include "core/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kUa =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

void psn_set_proxy(psn_proxy_fn fn) {
  g_proxy = fn;
}

int psn_get(const char *url, int head, long *code, char *ct, size_t ctc,
            char *body) {
  struct curl_slist *hdrs = NULL;
  struct curl_slist *pins;
  CURL *h;
  CURLcode rc;
  long c = 0;
  curlx_buf sink;

  if (code) *code = 0;
  if (ct && ctc) ct[0] = 0;
  if (body) body[0] = 0;

  pins = dns_pin_url(url);
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
  if (pins) curl_easy_setopt(h, CURLOPT_RESOLVE, pins);
  if (head) {
    curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
  } else {
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
