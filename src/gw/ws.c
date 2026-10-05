#include "gw/gw_common.h"
static void prop_str(const char *key, char *out, size_t cap,
                     const char *dflt) {
  if (cfg_get(key, out, cap) != 0 || out[0] == 0)
    snprintf(out, cap, "%s", dflt);
}

static void build_properties(char *out, size_t cap) {
  char os[64] = "", browser[128] = "", version[64] = "", device[64] = "";
  char e_os[128], e_browser[256], e_version[128], e_device[128];

  /* Defaults mirror the console's own DiscordAccessor identify:
     properties.os="Playstation", browser="PS5 GameBase", version="1.00".
     Discord treats a session identified this way as a console session, which
     is what surfaces the embedded indicator. Override with gw_os, gw_browser,
     gw_version, gw_device. */
  prop_str("gw_os", os, sizeof(os), "Playstation");
  prop_str("gw_browser", browser, sizeof(browser), "PS5 GameBase");
  prop_str("gw_version", version, sizeof(version), kManifest.client_version);
  prop_str("gw_device", device, sizeof(device), "PS5");

  json_copy(e_os, sizeof(e_os), os);
  json_copy(e_browser, sizeof(e_browser), browser);
  json_copy(e_version, sizeof(e_version), version);
  json_copy(e_device, sizeof(e_device), device);

  snprintf(out, cap,
           "{\"os\":\"%s\",\"browser\":\"%s\",\"version\":\"%s\","
           "\"device\":\"%s\",\"system_locale\":\"en-US\","
           "\"browser_user_agent\":\"\",\"browser_version\":\"%s\","
           "\"os_version\":\"\",\"client_version\":\"" GW_CLIENT_VERSION "\","
           "\"release_channel\":\"" GW_RELEASE_CHANNEL "\","
           "\"client_build_number\":%d,\"design_id\":0}",
           e_os, e_browser, e_version, e_device, e_version, GW_CLIENT_BUILD);
}

int ws_send(CURL *easy, const char *frame) {
  size_t sent = 0;
  size_t len = strlen(frame);
  if (len == 0) return -1;
  return curl_ws_send(easy, frame, len, &sent, (curl_off_t)len,
                      CURLWS_TEXT) == CURLE_OK
             ? 0
             : -1;
}

int connect_ws(CURL *easy, const char *url) {
  struct curl_slist *hdrs = NULL;
  struct curl_slist *pins;
  char props[512];
  char b64[768];
  CURLcode rc;
  char err[256];

  build_properties(props, sizeof(props));
  base64(props, b64, sizeof(b64));

  hdrs = curl_slist_append(hdrs, "Origin: " GW_ORIGIN);
  if (hdrs)
    hdrs = curl_slist_append(hdrs, "User-Agent: " GW_USER_AGENT);
  if (hdrs) {
    char buf[800];
    snprintf(buf, sizeof(buf), "X-Super-Properties: %s", b64);
    hdrs = curl_slist_append(hdrs, buf);
  }

  pins = dns_pin_url(url);

  curl_easy_setopt(easy, CURLOPT_URL, url);
  curl_easy_setopt(easy, CURLOPT_HTTPHEADER, hdrs);
  if (pins) curl_easy_setopt(easy, CURLOPT_RESOLVE, pins);
curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);
    curl_setup(easy, 30000L, 0);
  err[0] = 0;
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, err);

  rc = curl_easy_perform(easy);
  if (rc != CURLE_OK) {
    dlogf("drpc5: gateway connect failed: %s\n",
           err[0] ? err : curl_easy_strerror(rc));
    curl_slist_free_all(hdrs);
  /* The connection is up, so nothing will consult the pin again. Drop the
     option before freeing the list it points at: libcurl keeps the pointer
     until the option is replaced, so freeing first would leave it dangling. */
  curl_easy_setopt(easy, CURLOPT_RESOLVE, NULL);
  curl_slist_free_all(pins);
    return -1;
  }
  curl_slist_free_all(hdrs);
  /* The connection is up, so nothing will consult the pin again. Drop the
     option before freeing the list it points at: libcurl keeps the pointer
     until the option is replaced, so freeing first would leave it dangling. */
  curl_easy_setopt(easy, CURLOPT_RESOLVE, NULL);
  curl_slist_free_all(pins);
  return 0;
}

int send_identify(CURL *easy, const char *token) {
  char frame[GW_FRAME];
  char tok[TOKEN_MAX * 2];
  char props[512];

  json_copy(tok, sizeof(tok), token);
  build_properties(props, sizeof(props));
  snprintf(frame, sizeof(frame),
           "{\"op\":2,\"d\":{\"token\":\"%s\",\"capabilities\":%d,"
           "\"compress\":false,\"largeThreshold\":100,\"properties\":%s}}",
           tok, GW_CAPABILITIES, props);
  return ws_send(easy, frame);
}

int send_resume(CURL *easy, const char *token, const char *session_id,
                       long seq) {
  char frame[GW_FRAME];
  char tok[TOKEN_MAX * 2];
  char sid[128];

  json_copy(tok, sizeof(tok), token);
  json_copy(sid, sizeof(sid), session_id ? session_id : "");
  snprintf(frame, sizeof(frame),
           "{\"op\":6,\"d\":{\"token\":\"%s\",\"session_id\":\"%s\",\"seq\":%ld}}",
           tok, sid, seq < 0 ? 0 : seq);
  return ws_send(easy, frame);
}

void gw_build_url(char *out, size_t cap, const char *base) {
  size_t len;

  if (!base || !base[0]) {
    snprintf(out, cap, "%s", GW_URL GW_QUERY);
    return;
  }
  snprintf(out, cap, "%s", base);
  len = strlen(out);
  if (len == 0) return;
  if (out[len - 1] == '/') {
    if (len + sizeof(GW_QUERY_NOSLASH) < cap)
      snprintf(out + len, cap - len, "%s", GW_QUERY_NOSLASH);
  } else if (len + sizeof(GW_QUERY) < cap) {
    snprintf(out + len, cap - len, "%s", GW_QUERY);
  }
}

int send_heartbeat(CURL *easy) {
  static const char frame[] = "{\"op\":1,\"d\":null}";
  return ws_send(easy, frame);
}

int clear_presence(CURL *easy) {
  static const char frame[] =
      "{\"op\":3,\"d\":{\"since\":0,\"activities\":[],\"status\":\"dnd\","
      "\"afk\":false,\"flags\":0}}";
  return ws_send(easy, frame);
}

/* Only codes that mean "this token will never work again" belong here. 4007
   (invalid seq) and 4009 (invalid gateway version) are recoverable but not
   resumable: session.c handles those separately by discarding the session. */
int non_resumable(int code) {
  return code == 4004 || code == 4010 || code == 4011 || code == 4012 ||
         code == 4013 || code == 4014;
}