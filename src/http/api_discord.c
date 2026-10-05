/* POST /api/discord - authenticated pass-through to Discord's REST API.

   Used by the sign-in flows in the browser UI, which cannot call Discord
   directly with a user token from a page. The caller supplies the method,
   path, body and captcha headers; the upstream status and body come back as
   JSON so the page has one shape to parse. */
#define _GNU_SOURCE
#include "http/handlers.h"
#include "http/http.h"

#include "core/curl_api.h"
#include "core/json.h"
#include "core/util.h"
#include "discord/discord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
handle_discord(int fd, const char *body) {
  static const char *capname[3] = {
    "captcha_key", "captcha_session_id", "captcha_rqtoken"
  };
  static const char *caphead[3] = {
    "X-Captcha-Key", "X-Captcha-Session-Id", "X-Captcha-Rqtoken"
  };
  char method[16];
  char path[256];
  char inner[BODY_MAX];
  char cap[3][1024];
  char line[1200];
  struct curl_slist *extra = NULL;
  char *resp;
  char *out;
  int status = 0;
  int i;

  resp = malloc(JSON_MAX);
  if(!resp) {
    send_json(fd, 500, "{\"error\":\"oom\"}");
    return;
  }
  if(json_get(body, "method", method, sizeof(method)) != 0)
    snprintf(method, sizeof(method), "GET");
  if(json_get(body, "path", path, sizeof(path)) != 0 || path[0] != '/') {
    free(resp);
    send_json(fd, 400, "{\"error\":\"missing path\"}");
    return;
  }
  inner[0] = 0;
  json_get(body, "body", inner, sizeof(inner));

  for(i = 0; i < 3; i++) {
    size_t k;
    cap[i][0] = 0;
    json_get(body, capname[i], cap[i], sizeof(cap[i]));
    if(!cap[i][0]) continue;
    for(k = 0; cap[i][k]; k++)
      if(cap[i][k] == '\r' || cap[i][k] == '\n') cap[i][k] = ' ';
    snprintf(line, sizeof(line), "%s: %s", caphead[i], cap[i]);
    extra = curl_slist_append(extra, line);
  }

  if(discord_http(method, path, inner[0] ? inner : NULL, extra, &status,
                  resp, JSON_MAX - 512) != 0) {
    free(resp);
    send_json(fd, 502, "{\"error\":\"discord request failed\"}");
    return;
  }
  out = malloc(JSON_MAX);
  if(!out) {
    free(resp);
    send_json(fd, 500, "{\"error\":\"oom\"}");
    return;
  }
  {
    size_t off = 0;
    int overflow = 0;
    json_append(out, JSON_MAX, &off, &overflow,
                "{\"status\":%d,\"body\":\"", status);
    if(!overflow) json_escape(out + off, JSON_MAX - off, resp);
    if(overflow) dlogf("drpc5: /api/discord response truncated");
    if(off + 3 < JSON_MAX) {
      out[off++] = '"';
      out[off++] = '}';
      out[off] = 0;
    }
  }
  send_json(fd, 200, out);
  free(out);
  free(resp);
}