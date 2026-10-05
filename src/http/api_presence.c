/* GET /api/presence, /api/icon, /api/frame.

   The foreground app as the console reports it, that app's icon, and the
   last presence frame the gateway published (the UI shows it as a preview). */
#define _GNU_SOURCE
#include "http/handlers.h"
#include "http/http.h"

#include "core/json.h"
#include "core/util.h"
#include "gw/gateway.h"
#include "presence/presence.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void
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
void
handle_frame(int fd) {
  char buf[8192];

  if(gateway_frame(buf, sizeof(buf)) <= 0) {
    send_json(fd, 404, "{\"error\":\"no frame\"}");
    return;
  }
  send_response(fd, 200, "OK", "application/json; charset=utf-8", buf,
                strlen(buf));
}
void
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