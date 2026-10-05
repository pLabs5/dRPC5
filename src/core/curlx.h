#pragma once

#include "core/curl_api.h"

/* Common transport policy for every outbound request: certificate and host
   verification on, our own CA bundle, and no signals so curl never trips the
   SIGALRM alarm-handler machinery in a threaded payload.
   total_ms of 0 leaves curl's total timeout unset, which is what the
   long-lived websocket connections want. */
void curl_setup(CURL *easy, long connect_ms, long total_ms);

/* Bounded response-body sink for CURLOPT_WRITEFUNCTION. Appends into buf and
   aborts the transfer rather than overflowing. cap is the number of usable
   bytes, so buf must have cap + 1 allocated to fit the NUL terminator.
   Pass a curlx_buf as CURLOPT_WRITEDATA. */
typedef struct {
  char *buf;
  size_t cap;
} curlx_buf;

size_t curlx_buf_cb(char *ptr, size_t size, size_t nmemb, void *ud);