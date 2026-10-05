/* Discord REST: init, and one authenticated request helper.

   Everything here goes through libcurl over a pinned TLS config; nothing in this file
   talks to the gateway. */
#define _GNU_SOURCE
extern int sceNetPoolCreate(const char*, int, int);
int sceNetInit(void);

#include "discord/discord.h"
#include "core/curl_api.h"
#include "core/curlx.h"
#include "core/dns.h"
#include "core/util.h"
#include "paths.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_init_ok;

static void net_init_once(void) {
  int rc, pool;

  dlogf("net: stage=sceNetInit\n");
  rc = sceNetInit();
  dlogf("net: sceNetInit rc=%d\n", rc);

  dlogf("net: stage=sceNetPoolCreate\n");
  pool = sceNetPoolCreate("drpc5_curl", 512 * 1024, 0);
  dlogf("net: sceNetPoolCreate id=%d\n", pool);
  if (pool < 0) return;

  dlogf("net: stage=curl_global_init\n");
  rc = curl_global_init(CURL_GLOBAL_DEFAULT);
  dlogf("net: curl_global_init rc=%d\n", rc);
  g_init_ok = (rc == CURLE_OK);
}

int discord_init(void) {
  pthread_once(&g_once, net_init_once);
  return g_init_ok ? 0 : -1;
}

int discord_http(const char *method, const char *path, const char *body,
                 struct curl_slist *extra, int *status, char *out,
                 size_t outcap) {
  CURL *h;
  struct curl_slist *hdrs = extra;
  struct curl_slist *pins;
  char url[512];
  char *tmp;
  curlx_buf sink;
  CURLcode rc;
  long code = 0;

  if (discord_init() != 0) {
    curl_slist_free_all(hdrs);
    return -1;
  }
  if (path[0] != '/') {
    curl_slist_free_all(hdrs);
    return -1;
  }
  snprintf(url, sizeof(url), "https://discord.com%s", path);

  h = curl_easy_init();
  if (!h) {
    curl_slist_free_all(hdrs);
    return -1;
  }
  tmp = calloc(1, 32768);
  if (!tmp) {
    curl_easy_cleanup(h);
    curl_slist_free_all(hdrs);
    return -1;
  }
  out[0] = 0;
  *status = 0;

  pins = dns_pin_url(url);

  curl_easy_setopt(h, CURLOPT_URL, url);
  if (pins) curl_easy_setopt(h, CURLOPT_RESOLVE, pins);
  curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(h, CURLOPT_USERAGENT,
                   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                   "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
  hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
  hdrs = curl_slist_append(hdrs, "Accept: */*");
  hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9");
  hdrs = curl_slist_append(hdrs, "X-Discord-Locale: en-US");
  hdrs = curl_slist_append(hdrs, "X-Discord-Timezone: UTC");
  hdrs = curl_slist_append(hdrs,
                           "X-Super-Properties: "
                           "eyJvcyI6IldpbmRvd3MiLCJicm93c2VyIjoiQ2hyb21lIiwiZGV2aWNlIjoiIiwic3lzdGVtX2xvY2FsZSI6ImVuLVVTIiwiYnJvd3Nlcl91c2VyX2FnZW50IjoiTW96aWxsYS81LjAgKFdpbmRvd3MgTlQgMTAuMDsgV2luNjQ7IHg2NCkgQXBwbGVXZWJLaXQvNTM3LjM2IChLSFRNTCwgbGlrZSBHZWNrbykgQ2hyb21lLzEyNC4wLjAuMCBTYWZhcmkvNTM3LjM2IiwiYnJvd3Nlcl92ZXJzaW9uIjoiMTI0LjAuMC4wIiwib3NfdmVyc2lvbiI6IjEwLjAiLCJyZWZlcnJlciI6IiIsInJlZmVycmluZ19kb21haW4iOiIiLCJyZWZlcnJlcl9jdXJyZW50IjoiIiwicmVmZXJyaW5nX2RvbWFpbl9jdXJyZW50IjoiIiwicmVsZWFzZV9jaGFubmVsIjoic3RhYmxlIiwiY2xpZW50YnVpbGRudW1iZXIiOjI4NTExNCwiY2xpZW50X2V2ZW50X3NvdXJjZSI6bnVsbH0=");
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
  if (body && *body) {
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  }
sink.buf = tmp;
    sink.cap = 32767;
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curlx_buf_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_setup(h, 20000L, 30000L);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");

  rc = curl_easy_perform(h);
  if (rc == CURLE_OK)
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
  if (rc == CURLE_OK) {
    *status = (int)code;
    snprintf(out, outcap, "%s", tmp);
  }

  curl_easy_cleanup(h);
  curl_slist_free_all(hdrs);
  curl_slist_free_all(pins);
  free(tmp);
  return rc == CURLE_OK ? 0 : -1;
}
