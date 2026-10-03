#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bundled_tile_pkg.h"
#include "discord.h"
#include "gateway.h"
#include "paths.h"
#include "presence.h"
#include "web.h"

#define HDR_MAX   16384
#define BODY_MAX  49152
#define JSON_MAX  65536

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallPkg(const char*, void*);
int sceAppInstUtilInstallByPackage(void*, void*, void*);

struct pkg_info_abi {
  char content_id[0x30];
  int content_type;
  int content_platform;
};

struct pkg_meta_abi {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
};

struct pkg_playgo_abi {
  char languages[30][8];
  char scenario_ids[64][3];
  char content_ids[64][0x30];
  long unknown[810];
};

static const char *kConfigKeys[] = {
    "enabled", "app_id", "mode", "status",
    "src_game", "src_media", "src_app", "src_idle",
    "media_line", "show_artwork", "show_platform",
    "name", "state", "details", "type",
    "asset_key", "asset_text", "asset_small_key", "asset_small_text",
    "start_timestamp", "end_timestamp",
    "start_timestamp", "end_timestamp", NULL};

static int
write_pkg(void) {
  FILE *f;
  size_t written;

  if((f=fopen(PKG_PATH, "wb"))==NULL) return -1;
  written=fwrite(kTilePkg, 1, kTilePkgSize, f);
  fclose(f);
  return written==kTilePkgSize ? 0 : -1;
}

static void
install_tile(void) {
  struct stat st;
  struct pkg_info_abi info;
  struct pkg_info_abi info2;
  struct pkg_meta_abi meta;
  struct pkg_playgo_abi playgo;
  char uri[96];
  int err;

  if(stat(APPMETA_PATH, &st)==0) return;

  mkdir(STORE_DIR, 0777);
  if(write_pkg()) {
    printf("drpc5: could not write " PKG_PATH "\n");
    return;
  }

  if((err=sceAppInstUtilInitialize())) {
    printf("sceAppInstUtilInitialize: %x\n", err);
    return;
  }

  memset(&info, 0, sizeof(info));
  if((err=sceAppInstUtilAppInstallPkg(PKG_VISIBLE, &info))==0) {
    printf("drpc5: tile installed\n");
    return;
  }
  printf("sceAppInstUtilAppInstallPkg: %x\n", err);

  memset(&info2, 0, sizeof(info2));
  memset(&meta, 0, sizeof(meta));
  memset(&playgo, 0, sizeof(playgo));
  snprintf(uri, sizeof(uri), "file://%s", PKG_VISIBLE);
  meta.uri=uri;
  meta.content_name="dRPC5";

  if((err=sceAppInstUtilInstallByPackage(&meta, &info2, &playgo))==0) {
    printf("drpc5: tile installed\n");
    return;
  }
  printf("sceAppInstUtilInstallByPackage: %x\n", err);
}

static int
read_whole_file(const char *path, char *buf, size_t cap) {
  FILE *f;
  size_t n;

  f = fopen(path, "rb");
  if(!f) return -1;
  n = fread(buf, 1, cap, f);
  fclose(f);
  return (int)n;
}

static int
send_all(int fd, const void *buf, size_t len) {
  const char *p = buf;
  while(len) {
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if(n <= 0) {
      if(n < 0 && errno == EINTR) continue;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static void
send_response(int fd, int code, const char *reason, const char *ctype,
              const void *body, size_t len) {
  char hdr[256];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Connection: close\r\n"
                   "Cache-Control: no-store\r\n\r\n",
                   code, reason, ctype, len);
  if(n <= 0) return;
  if(send_all(fd, hdr, (size_t)n)) return;
  if(len) send_all(fd, body, len);
}

static void
send_json(int fd, int code, const char *json) {
  const char *reason = code < 400 ? "OK" : "Error";
  send_response(fd, code, reason, "application/json", json, strlen(json));
}

static size_t
json_escape(char *dst, size_t cap, const char *src) {
  size_t o = 0;
  if(!cap) return 0;
  while(*src && o + 7 < cap) {
    unsigned char c = (unsigned char)*src++;
    if(c == '"' || c == '\\') {
      dst[o++] = '\\';
      dst[o++] = (char)c;
    } else if(c == '\n') {
      dst[o++] = '\\';
      dst[o++] = 'n';
    } else if(c == '\r') {
      dst[o++] = '\\';
      dst[o++] = 'r';
    } else if(c == '\t') {
      dst[o++] = '\\';
      dst[o++] = 't';
    } else if(c < 0x20) {
      o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
    } else {
      dst[o++] = (char)c;
    }
  }
  dst[o] = 0;
  return o;
}

static char *
find_header(char *hdrs, const char *key) {
  char *p = hdrs;
  size_t klen = strlen(key);
  while(p && *p) {
    if(!strncasecmp(p, key, klen) && p[klen] == ':') {
      char *v = p + klen + 1;
      while(*v == ' ' || *v == '\t') v++;
      return v;
    }
    p = strstr(p, "\r\n");
    if(!p) break;
    p += 2;
  }
  return NULL;
}

static int
json_get(const char *body, const char *key, char *out, size_t cap) {
  char pat[80];
  const char *p;
  const char *end;
  size_t o = 0;

  if(cap == 0) return -1;
  out[0] = 0;
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  p = strstr(body, pat);
  if(!p) return -1;
  p += strlen(pat);
  while(*p == ' ' || *p == '\t') p++;
  if(*p != ':') return -1;
  p++;
  while(*p == ' ' || *p == '\t') p++;

  if(*p == '"') {
    p++;
    while(*p && *p != '"') {
      if(*p == '\\' && p[1]) {
        p++;
        if(o + 1 < cap) out[o++] = *p;
        p++;
        continue;
      }
      if(o + 1 < cap) out[o++] = *p;
      p++;
    }
    out[o] = 0;
    return 0;
  }

  end = p;
  while(*end && *end != ',' && *end != '}' && *end != '\r' && *end != '\n')
    end++;
  while(end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
  o = (size_t)(end - p);
  if(o >= cap) o = cap - 1;
  memcpy(out, p, o);
  out[o] = 0;
  return 0;
}

static int
qs_get(const char *query, const char *key, char *out, size_t cap) {
  char pat[64];
  const char *p;
  const char *end;
  size_t o = 0;

  out[0] = 0;
  if(!query) return -1;
  snprintf(pat, sizeof(pat), "%s=", key);
  p = strstr(query, pat);
  if(!p) return -1;
  p += strlen(pat);
  end = p;
  while(*end && *end != '&') end++;
  while(p < end && o + 1 < cap) out[o++] = *p++;
  out[o] = 0;
  return 0;
}

static void
sanitize_value(char *v, size_t cap) {
  size_t o = 0;
  size_t i;
  for(i = 0; v[i] && o + 1 < cap; i++) {
    unsigned char c = (unsigned char)v[i];
    if(c == '\r' || c == '\n' || c == '\t') continue;
    if(c < 0x20) continue;
    v[o++] = (char)c;
  }
  v[o] = 0;
}

static void
handle_status(int fd) {
  struct stat st;
  char cfg[4096];
  char esc[300];
  char json[8192];
  char activity[192];
  size_t off = 0;
  int has_token = stat(TOKEN_PATH, &st) == 0;
  int installed = stat(APPMETA_PATH, &st) == 0;
  int gw_connected = 0;
  int gw_ready = 0;
  int gw_auth_failed = 0;
  FILE *f;

  gateway_status(&gw_connected, &gw_ready, &gw_auth_failed, activity, sizeof(activity));

  off += (size_t)snprintf(json + off, sizeof(json) - off,
                          "{\"installed\":%d,\"port\":%d,\"has_token\":%d,"
                          "\"gateway\":{\"connected\":%d,\"ready\":%d,"
                          "\"auth_failed\":%d,\"activity\":\"%s\"},"
                          "\"config\":{",
                          installed, DRPC_PORT, has_token, gw_connected,
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
      const char *kp = strstr(cfg, kConfigKeys[i]);
      char buf[256];
      const char *val = "";
      size_t klen = strlen(kConfigKeys[i]);
      buf[0] = 0;
      if(kp && (kp == cfg || kp[-1] == '\n') && kp[klen] == '=') {
        const char *e = strchr(kp + klen + 1, '\n');
        size_t vl = e ? (size_t)(e - (kp + klen + 1)) : strlen(kp + klen + 1);
        if(vl >= sizeof(buf)) vl = sizeof(buf) - 1;
        memcpy(buf, kp + klen + 1, vl);
        buf[vl] = 0;
        val = buf;
      }
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
  char val[256];
  size_t off = 0;
  FILE *f;
  int i;

  mkdir(STORE_DIR, 0777);
  if((f = fopen(CONFIG_PATH, "w")) == NULL) {
    send_json(fd, 500, "{\"error\":\"cannot write config\"}");
    return;
  }
  for(i = 0; kConfigKeys[i]; i++) {
    if(json_get(body, kConfigKeys[i], val, sizeof(val)) != 0)
      val[0] = 0;
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
  char method[16];
  char path[256];
  char inner[BODY_MAX];
  char *resp;
  char *out;
  int status = 0;

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

  if(discord_http(method, path, inner[0] ? inner : NULL, &status, resp,
                  JSON_MAX - 512) != 0) {
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
                          "\"app_type\":%u,\"running\":%d,"
                          "\"start_timestamp\":%lld,\"title_id\":\"%s\",",
                          app.pid, (unsigned)app.app_id,
                          (unsigned)app.app_type, app.running,
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

static void
route(int fd, char *method, char *path, char *query, char *body) {
  if(!strcmp(method, "GET") && (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
    send_response(fd, 200, "OK", "text/html; charset=utf-8", kIndexHtml,
                  kIndexHtmlLen);
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

static int
read_line_body(char *hdrs, size_t *hlen, int fd, char *body) {
  long content_length = 0;
  char *cl;
  size_t body_len = 0;

  hdrs[*hlen] = 0;
  cl = find_header(hdrs, "Content-Length");
  if(cl) content_length = strtol(cl, NULL, 10);
  if(content_length < 0) content_length = 0;
  if(content_length > BODY_MAX) content_length = BODY_MAX;

  {
    char *end = strstr(hdrs, "\r\n\r\n");
    if(end) {
      body_len = *hlen - (size_t)(end + 4 - hdrs);
      if((long)body_len > content_length) body_len = (size_t)content_length;
      if(body_len) memcpy(body, end + 4, body_len);
    } else {
      body_len = 0;
    }
  }

  while((long)body_len < content_length) {
    ssize_t n = recv(fd, body + body_len, (size_t)content_length - body_len,
                     0);
    if(n <= 0) break;
    body_len += (size_t)n;
  }
  body[body_len] = 0;
  return (int)body_len;
}

static void *
handle_conn(void *arg) {
  int fd = (int)(intptr_t)arg;
  char *hdrs;
  char *body;
  char method[16];
  char path[1024];
  char *query;
  size_t hlen = 0;
  ssize_t n;

  hdrs = malloc(HDR_MAX + 1);
  body = malloc(BODY_MAX + 1);
  if(!hdrs || !body) {
    free(hdrs);
    free(body);
    close(fd);
    return NULL;
  }

  for(;;) {
    if(hlen >= HDR_MAX) break;
    n = recv(fd, hdrs + hlen, HDR_MAX - hlen, 0);
    if(n <= 0) break;
    hlen += (size_t)n;
    hdrs[hlen] = 0;
    if(strstr(hdrs, "\r\n\r\n")) break;
  }
  if(!strstr(hdrs, "\r\n\r\n")) {
    free(hdrs);
    free(body);
    close(fd);
    return NULL;
  }

  if(sscanf(hdrs, "%15s %1023s", method, path) != 2) {
    free(hdrs);
    free(body);
    close(fd);
    return NULL;
  }

  read_line_body(hdrs, &hlen, fd, body);

  query = strchr(path, '?');
  if(query) {
    *query = 0;
    query++;
  }

  route(fd, method, path, query, body);

  free(hdrs);
  free(body);
  close(fd);
  return NULL;
}

static int
httpd_run(void) {
  int lfd;
  struct sockaddr_in addr;
  int one = 1;

  lfd = socket(AF_INET, SOCK_STREAM, 0);
  if(lfd < 0) return -1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(DRPC_PORT);
  if(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    printf("drpc5: bind failed: %d\n", errno);
    close(lfd);
    return -1;
  }
  if(listen(lfd, 8) < 0) {
    close(lfd);
    return -1;
  }
  printf("drpc5: config server on http://127.0.0.1:%d/\n", DRPC_PORT);

  for(;;) {
    int cfd = accept(lfd, NULL, NULL);
    pthread_t th;
    if(cfd < 0) {
      if(errno == EINTR) continue;
      break;
    }
    if(pthread_create(&th, NULL, handle_conn, (void *)(intptr_t)cfd) == 0)
      pthread_detach(th);
    else
      close(cfd);
  }
  close(lfd);
  return -1;
}

int
main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  discord_init();
  install_tile();
  httpd_run();
  gateway_start();
  for(;;) pause();
  return 0;
}
