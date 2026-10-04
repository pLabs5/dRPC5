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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
  int has_token = stat(TOKEN_PATH, &st) == 0;
  int installed = stat(APPMETA_PATH, &st) == 0;
  int gw_connected = 0;
  int gw_ready = 0;
  int gw_auth_failed = 0;
  FILE *f;

  gateway_status(&gw_connected, &gw_ready, &gw_auth_failed, activity, sizeof(activity));
  lan_ip(ip, sizeof(ip));

  off += (size_t)snprintf(json + off, sizeof(json) - off,
                          "{\"installed\":%d,\"port\":%d,\"has_token\":%d,"
                          "\"ip\":\"%s\","
                          "\"gateway\":{\"connected\":%d,\"ready\":%d,"
                          "\"auth_failed\":%d,\"activity\":\"%s\"},"
                          "\"config\":{",
                          installed, DRPC_PORT, has_token, ip, gw_connected,
                          gw_ready, gw_auth_failed, activity);
  cfg[0] = 0;
  if((f = fopen(CONFIG_PATH, "r")) != NULL) {
    size_t n = fread(cfg, 1, sizeof(cfg) - 1, f);
    cfg[n] = 0;
    fclose(f);
  }
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
      off += (size_t)snprintf(json + off, sizeof(json) - off,
                              "%s\"%s\":\"%s\"", first ? "" : ",",
                              kConfigKeys[i], esc);
      first = 0;
    }
  }
  off += (size_t)snprintf(json + off, sizeof(json) - off, "}}");
  send_json(fd, 200, json);
}

static void
handle_config(int fd, const char *body) {
  char json[8192];
  char cfg[4096];
  char val[256];
  size_t off = 0;
  FILE *f;
  int i;

  cfg[0] = 0;
  if((f = fopen(CONFIG_PATH, "r")) != NULL) {
    size_t n = fread(cfg, 1, sizeof(cfg) - 1, f);
    cfg[n] = 0;
    fclose(f);
  }
  mkdir(STORE_DIR, 0777);
  if((f = fopen(CONFIG_PATH, "w")) == NULL) {
    send_json(fd, 500, "{\"error\":\"cannot write config\"}");
    return;
  }
  for(i = 0; kConfigKeys[i]; i++) {
    if(json_get(body, kConfigKeys[i], val, sizeof(val)) != 0 &&
       cfg_lookup(cfg, kConfigKeys[i], val, sizeof(val)) != 0)
      snprintf(val, sizeof(val), "%s",
               i < kConfigDefaultsN && kConfigDefaults[i] ? kConfigDefaults[i]
                                                       : "");
    sanitize_value(val, sizeof(val));
    fprintf(f, "%s=%s\n", kConfigKeys[i], val);
  }
  fclose(f);
  off += (size_t)snprintf(json + off, sizeof(json) - off, "{\"ok\":1}");
  send_json(fd, 200, json);
}

static void
handle_token(int fd, const char *body) {
  char token[600];
  FILE *f;

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
  if((f = fopen(TOKEN_PATH, "w")) == NULL) {
    send_json(fd, 500, "{\"error\":\"cannot write token\"}");
    return;
  }
  fwrite(token, 1, strlen(token), f);
  fclose(f);
  chmod(TOKEN_PATH, 0600);
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
    size_t off = (size_t)snprintf(out, JSON_MAX, "{\"status\":%d,\"body\":\"",
                                  status);
    off += json_escape(out + off, JSON_MAX - off, resp);
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
    off += (size_t)snprintf(json + off, sizeof(json) - off, "{\"error\":\"");
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

  if(presence_foreground(&app) != 0) {
    send_json(fd, 200, "{\"active\":0}");
    return;
  }

  json_escape(esc, sizeof(esc), app.title_id);
  off += (size_t)snprintf(json + off, sizeof(json) - off,
                          "{\"active\":1,\"pid\":%d,\"app_id\":%u,"
                          "\"running\":%d,"
                          "\"start_timestamp\":%lld,\"title_id\":\"%s\",",
                          app.pid, (unsigned)app.app_id,
                          app.running,
                          (long long)app.start_epoch, esc);

  json_escape(esc, sizeof(esc), app.name);
  off += (size_t)snprintf(json + off, sizeof(json) - off, "\"name\":\"%s\",",
                          esc);

  json_escape(esc, sizeof(esc), app.version);
  off += (size_t)snprintf(json + off, sizeof(json) - off, "\"version\":\"%s\",",
                          esc);

  json_escape(esc, sizeof(esc), app.icon_path);
  snprintf(json + off, sizeof(json) - off, "\"icon\":\"%s\"}", esc);
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

void route(int fd, char *method, char *path, char *query, char *body) {
  if(!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html") ||
                                !strcmp(path, "/callback"))) {
    if(strcmp(path, "/callback") && !peer_is_local(fd)) {
      send_redirect(fd, "/pc.html");
      return;
    }
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
