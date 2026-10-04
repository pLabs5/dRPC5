#include "gw_common.h"
int proxy_external_asset(const char *url, char *out, size_t cap) {
  char token[TOKEN_MAX], app_id[32], ep[192], auth[TOKEN_MAX + 32];
  char body[700], esc[600], resp[4096];
  curlx_buf sink;
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
  sink.buf = resp;
  sink.cap = 4094;
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curlx_buf_cb);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_setup(h, 15000L, 25000L);

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
