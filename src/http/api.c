#define _GNU_SOURCE
#include "api.h"
#include "http.h"

#include "core/config.h"
#include "curl_api.h"
#include "core/json.h"
#include "core/util.h"
#include "discord.h"
#include "gateway.h"
#include "paths.h"
#include "presence.h"
#include "psn.h"
#include "web.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

static void
handle_status(int fd) {
  struct stat st;
  char cfg[4096];
  char esc[300];
  char json[8192];
  char activity[192];
  char ip[64];
  size_t off = 0;
  int overflow = 0;
  int has_token = stat(TOKEN_PATH, &st) == 0;
  int installed = stat(APPMETA_PATH, &st) == 0;
  int gw_connected = 0;
  int gw_ready = 0;
  int gw_auth_failed = 0;

  gateway_status(&gw_connected, &gw_ready, &gw_auth_failed, activity, sizeof(activity));
  lan_ip(ip, sizeof(ip));

  json_append(json, sizeof(json), &off, &overflow,
              "{\"installed\":%d,\"port\":%d,\"has_token\":%d,"
              "\"ip\":\"%s\","
              "\"gateway\":{\"connected\":%d,\"ready\":%d,"
              "\"auth_failed\":%d,\"activity\":\"%s\"},"
              "\"config\":{",
              installed, DRPC_PORT, has_token, ip, gw_connected,
              gw_ready, gw_auth_failed, activity);
  cfg[0] = 0;
  read_file_all(CONFIG_PATH, cfg, sizeof(cfg), NULL);
  {
    int first = 1;
    int i;
    for(i = 0; kConfigKeys[i]; i++) {
      char buf[256];
      const char *val = "";
      buf[0] = 0;
      if(cfg_lookup(cfg, kConfigKeys[i], buf, sizeof(buf)) == 0 && buf[0])
        val = buf;
      json_escape(esc, sizeof(esc), val);
      if(json_append(json, sizeof(json), &off, &overflow,
                     "%s\"%s\":\"%s\"", first ? "" : ",",
                     kConfigKeys[i], esc))
        break;
      first = 0;
    }
  }
  json_append(json, sizeof(json), &off, &overflow, "}}");
  if(overflow) dlogf("drpc5: /api/status response truncated");
  send_json(fd, 200, json);
}

static void
handle_config(int fd, const char *body) {
  char json[8192];
  char cfg[4096];
  char val[256];
  char out[16384];
  size_t off = 0;
  int i;

  cfg[0] = 0;
  read_file_all(CONFIG_PATH, cfg, sizeof(cfg), NULL);
  mkdir(STORE_DIR, 0777);
  /* Build the whole file in memory, then swap it in with one rename so the
     gateway thread never reads a half-written config. */
  for(i = 0; kConfigKeys[i]; i++) {
    int w;
    if(json_get(body, kConfigKeys[i], val, sizeof(val)) != 0 &&
       cfg_lookup(cfg, kConfigKeys[i], val, sizeof(val)) != 0)
      snprintf(val, sizeof(val), "%s",
               i < kConfigDefaultsN && kConfigDefaults[i] ? kConfigDefaults[i]
                                                       : "");
    sanitize_value(val, sizeof(val));
    w = snprintf(out + off, sizeof(out) - off, "%s=%s\n", kConfigKeys[i], val);
    /* Stop on truncation rather than retrying from out[0], which would drop
       every earlier key and leave a config that looks valid but is not. */
    if(w < 0 || (size_t)w >= sizeof(out) - off) {
      send_json(fd, 500, "{\"error\":\"config too large\"}");
      return;
    }
    off += (size_t)w;
  }
  if(write_file_atomic(CONFIG_PATH, out, off, 0644) != 0) {
    send_json(fd, 500, "{\"error\":\"cannot write config\"}");
    return;
  }
  json_append(json, sizeof(json), &off, NULL, "{\"ok\":1}");
  send_json(fd, 200, json);
}

static void
handle_token(int fd, const char *body) {
  char token[600];

  if(json_get(body, "token", token, sizeof(token)) != 0 || !token[0]) {
    send_json(fd, 400, "{\"error\":\"missing token\"}");
    return;
  }
  sanitize_value(token, sizeof(token));
  if(strlen(token) < 20) {
    send_json(fd, 400, "{\"error\":\"token too short\"}");
    return;
  }
  mkdir(STORE_DIR, 0777);
  /* 0600 before the bytes are written, and atomic: a torn token would still
     pass read_token's length check and permanently break gateway auth. */
  if(write_file_atomic(TOKEN_PATH, token, strlen(token), 0600) != 0) {
    send_json(fd, 500, "{\"error\":\"cannot write token\"}");
    return;
  }
  send_json(fd, 200, "{\"ok\":1}");
}

static void
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

static void
handle_ra_start(int fd, const char *body) {
  char pubkey[600];
  char err[160];
  char json[512];
  int id;

  if(json_get(body, "public_key", pubkey, sizeof(pubkey)) != 0 ||
     !pubkey[0]) {
    send_json(fd, 400, "{\"error\":\"missing public_key\"}");
    return;
  }
  err[0] = 0;
  id = ra_start(pubkey, err, sizeof(err));
  if(id < 0) {
    size_t off = 0;
    json_append(json, sizeof(json), &off, NULL, "{\"error\":\"");
    json_escape(json + off, sizeof(json) - off,
                err[0] ? err : "start failed");
    strcat(json, "\"}");
    send_json(fd, 500, json);
    return;
  }
  snprintf(json, sizeof(json), "{\"id\":%d}", id);
  send_json(fd, 200, json);
}

static void
handle_ra_poll(int fd, const char *query) {
  char idbuf[16];
  char waitbuf[16];
  char *frames;
  char *out;
  int id;
  long wait;
  int closed = 0;

  frames = malloc(JSON_MAX);
  if(!frames) {
    send_json(fd, 500, "{\"error\":\"oom\"}");
    return;
  }
  if(qs_get(query, "id", idbuf, sizeof(idbuf)) != 0 ||
     qs_get(query, "wait", waitbuf, sizeof(waitbuf)) != 0) {
    free(frames);
    send_json(fd, 400, "{\"error\":\"missing params\"}");
    return;
  }
  id = atoi(idbuf);
  wait = atol(waitbuf);
  if(ra_poll(id, wait, frames, JSON_MAX - 64, &closed) != 0) {
    free(frames);
    send_json(fd, 404, "{\"error\":\"no such session\"}");
    return;
  }
  out = malloc(JSON_MAX);
  if(!out) {
    free(frames);
    send_json(fd, 500, "{\"error\":\"oom\"}");
    return;
  }
  snprintf(out, JSON_MAX, "{\"frames\":%s,\"closed\":%d}", frames, closed);
  free(frames);
  send_json(fd, 200, out);
  free(out);
}

static void
handle_ra_send(int fd, const char *body) {
  char idbuf[16];
  char data[2048];

  if(json_get(body, "id", idbuf, sizeof(idbuf)) != 0 ||
     json_get(body, "data", data, sizeof(data)) != 0) {
    send_json(fd, 400, "{\"error\":\"missing params\"}");
    return;
  }
  if(ra_send(atoi(idbuf), data) != 0) {
    send_json(fd, 500, "{\"error\":\"send failed\"}");
    return;
  }
  send_json(fd, 200, "{\"ok\":1}");
}

static void
handle_ra_close(int fd, const char *body) {
  char idbuf[16];

  if(json_get(body, "id", idbuf, sizeof(idbuf)) != 0) {
    send_json(fd, 400, "{\"error\":\"missing id\"}");
    return;
  }
  ra_close(atoi(idbuf));
  send_json(fd, 200, "{\"ok\":1}");
}

static void
handle_presence(int fd) {
  ps5_app_t app;
  char esc[512];
  char json[4096];
  size_t off = 0;
  int overflow = 0;

  if(presence_foreground(&app) != 0) {
    send_json(fd, 200, "{\"active\":0}");
    return;
  }

  json_escape(esc, sizeof(esc), app.title_id);
  json_append(json, sizeof(json), &off, &overflow,
              "{\"active\":1,\"pid\":%d,\"app_id\":%u,"
              "\"running\":%d,"
              "\"start_timestamp\":%lld,\"title_id\":\"%s\",",
              app.pid, (unsigned)app.app_id,
              app.running,
              (long long)app.start_epoch, esc);

  json_escape(esc, sizeof(esc), app.name);
  json_append(json, sizeof(json), &off, &overflow, "\"name\":\"%s\",", esc);

  json_escape(esc, sizeof(esc), app.version);
  json_append(json, sizeof(json), &off, &overflow, "\"version\":\"%s\",", esc);

  json_escape(esc, sizeof(esc), app.icon_path);
  json_append(json, sizeof(json), &off, &overflow, "\"icon\":\"%s\"}", esc);
  if(overflow) dlogf("drpc5: /api/presence response truncated");
  send_json(fd, 200, json);
}

static void
handle_frame(int fd) {
  char buf[8192];

  if(gateway_frame(buf, sizeof(buf)) <= 0) {
    send_json(fd, 404, "{\"error\":\"no frame\"}");
    return;
  }
  send_response(fd, 200, "OK", "application/json; charset=utf-8", buf,
                strlen(buf));
}

static void
handle_icon(int fd) {
  ps5_app_t app;
  struct stat st;
  char *buf;
  int n;

  if(presence_foreground(&app) != 0 || !app.icon_path[0]) {
    send_json(fd, 404, "{\"error\":\"no icon\"}");
    return;
  }
  if(stat(app.icon_path, &st) != 0 || st.st_size <= 0 ||
     st.st_size > 8 * 1024 * 1024) {
    send_json(fd, 404, "{\"error\":\"no icon\"}");
    return;
  }
  buf = malloc((size_t)st.st_size);
  if(!buf) {
    send_json(fd, 500, "{\"error\":\"oom\"}");
    return;
  }
  n = read_whole_file(app.icon_path, buf, (size_t)st.st_size);
  if(n <= 0) {
    free(buf);
    send_json(fd, 404, "{\"error\":\"no icon\"}");
    return;
  }
  send_response(fd, 200, "OK", "image/png", buf, (size_t)n);
  free(buf);
}

/* Reachable from the rest of the LAN: the PC page, and the one thing it is for -
 * handing us a token. It also reads status to render the pills next to the token
 * box. Everything else (config, presence, token deletion, remote auth, the
 * console UI itself) is loopback-only. */
static int lan_allowed(const char *method, const char *path) {
  if(!strcmp(method, "POST")) return !strcmp(path, "/api/token");
  if(strcmp(method, "GET")) return 0;
  return !strcmp(path, "/pc.html") || !strcmp(path, "/token") ||
         !strcmp(path, "/qrcode.js") || !strcmp(path, "/api/status");
}

/* Reject requests a browser could have induced from another site. The config
   server is plain HTTP on loopback, so a page on the internet can reach it via
   the user's browser; without these checks the browser's loopback peer passes
   peer_is_local() and any site could rewrite config or read the token page. */
/* True when `origin` is an origin this server itself could have served, i.e.
   loopback over http/https. The LAN address is deliberately not accepted: it is
   reachable from other machines, and a page on those machines must not be able
   to drive the console through the browser. */
static int own_origin(const char *origin) {
  static const char *const hosts[] = {"127.0.0.1", "localhost", NULL};
  size_t i;

  if(strncasecmp(origin, "http://", 7) != 0 &&
     strncasecmp(origin, "https://", 8) != 0)
    return 0;
  origin += strncasecmp(origin, "https://", 8) == 0 ? 8 : 7;
  /* Host ends at '/', ':' or end of value. */
  for(i = 0; hosts[i]; i++) {
    size_t hl = strlen(hosts[i]);
    if(strncasecmp(origin, hosts[i], hl) == 0 &&
       (origin[hl] == 0 || origin[hl] == '/' || origin[hl] == ':'))
      return 1;
  }
  return 0;
}

/* Reject requests a browser could have induced from another site. The config
   server is plain HTTP on loopback, so a page on the internet can reach it via
   the user's browser; without these checks the browser's loopback peer passes
   peer_is_local() and any site could rewrite config or read the token page.
   Returns 1 to block. */
static int cross_site(char *hdrs) {
  char *v;

  if(!hdrs) return 0;
  v = find_header(hdrs, "Sec-Fetch-Site");
  if(v) {
    while(*v == ' ' || *v == '\t') v++;
    if(!strncasecmp(v, "cross-site", 10) || !strncasecmp(v, "same-site", 9))
      return 1;
  }
  /* Origin, when present, must be one of our own. "null" is an opaque origin
     (sandboxed iframe, file://, some redirects) and is never us. A missing
     Origin on a same-origin GET is normal, so absence alone is not a reason
     to block. */
  v = find_header(hdrs, "Origin");
  if(v) {
    char origin[128];
    size_t i = 0;
    while(*v == ' ' || *v == '\t') v++;
    while(v[i] && !isspace((unsigned char)v[i]) && v[i] != '\r' &&
          v[i] != '\n' && i + 1 < sizeof(origin)) {
      origin[i] = v[i];
      i++;
    }
    origin[i] = 0;
    if(!own_origin(origin)) {
      dlogf("drpc5: rejected cross-origin request from %s", origin);
      return 1;
    }
  }
  return 0;
}

void route(int fd, char *method, char *path, char *query, char *body,
           char *hdrs) {
  if(cross_site(hdrs)) {
    send_json(fd, 403, "{\"error\":\"cross-site request blocked\"}");
    return;
  }
  if(!peer_is_local(fd)) {
    if(!strcmp(path, "/") || !strcmp(path, "/index.html")) {
      send_redirect(fd, "/pc.html");
      return;
    }
    if(!lan_allowed(method, path)) {
      send_json(fd, 403, "{\"error\":\"local only\"}");
      return;
    }
  }
  if(!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html") ||
                                !strcmp(path, "/callback"))) {
    send_response(fd, 200, "OK", "text/html; charset=utf-8", kIndexHtml,
                  kIndexHtmlLen);
    return;
  }
  if(!strcmp(method, "GET") && (!strcmp(path, "/pc.html") ||
                                !strcmp(path, "/token"))) {
    send_response(fd, 200, "OK", "text/html; charset=utf-8", kPcHtml,
                  kPcHtmlLen);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/qrcode.js")) {
    send_response(fd, 200, "OK", "application/javascript", kQrcodeJs,
                  kQrcodeJsLen);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/style.css")) {
    send_response(fd, 200, "OK", "text/css; charset=utf-8", kStyleCss,
                  kStyleCssLen);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/js/util.js")) {
    send_response(fd, 200, "OK", "application/javascript", kJsUtil,
                  kJsUtilLen);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/js/app.js")) {
    send_response(fd, 200, "OK", "application/javascript", kJsApp,
                  kJsAppLen);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/js/remote.js")) {
    send_response(fd, 200, "OK", "application/javascript", kJsRemote,
                  kJsRemoteLen);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/js/signin.js")) {
    send_response(fd, 200, "OK", "application/javascript", kJsSignin,
                  kJsSigninLen);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/api/status")) {
    handle_status(fd);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/api/presence")) {
    handle_presence(fd);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/api/icon")) {
    handle_icon(fd);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/api/frame")) {
    handle_frame(fd);
    return;
  }
  if(!strcmp(method, "POST") && !strcmp(path, "/api/config")) {
    handle_config(fd, body);
    return;
  }
  if(!strcmp(method, "POST") && !strcmp(path, "/api/token")) {
    handle_token(fd, body);
    return;
  }
  if(!strcmp(method, "POST") && !strcmp(path, "/api/token/delete")) {
    unlink(TOKEN_PATH);
    send_json(fd, 200, "{\"ok\":1}");
    return;
  }
  if(!strcmp(method, "POST") && !strcmp(path, "/api/discord")) {
    handle_discord(fd, body);
    return;
  }
  if(!strcmp(method, "POST") && !strcmp(path, "/api/ra/start")) {
    handle_ra_start(fd, body);
    return;
  }
  if(!strcmp(method, "GET") && !strcmp(path, "/api/ra/poll")) {
    handle_ra_poll(fd, query);
    return;
  }
  if(!strcmp(method, "POST") && !strcmp(path, "/api/ra/send")) {
    handle_ra_send(fd, body);
    return;
  }
  if(!strcmp(method, "POST") && !strcmp(path, "/api/ra/close")) {
    handle_ra_close(fd, body);
    return;
  }
  send_json(fd, 404, "{\"error\":\"not found\"}");
}
