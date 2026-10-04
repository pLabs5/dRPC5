#include "core/curlx.h"

#include "paths.h"

#include <string.h>

void curl_setup(CURL *easy, long connect_ms, long total_ms) {
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, connect_ms);
  if(total_ms > 0) curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, total_ms);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(easy, CURLOPT_CAINFO, CA_PATH);
}

size_t curlx_buf_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
  curlx_buf *b = (curlx_buf *)ud;
  size_t want = size * nmemb;
  size_t used = strlen(b->buf);

  if(used + want > b->cap) return 0;
  memcpy(b->buf + used, ptr, want);
  b->buf[used + want] = 0;
  return want;
}