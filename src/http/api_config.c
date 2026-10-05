/* POST /api/config - rewrite the config file from a form post.

   Every known key is written, whether or not the request mentioned it, so a
   partial post cannot silently drop settings. The whole file is built in
   memory and swapped in with one rename, because the gateway thread reads it
   concurrently and must never see a half-written config. */
#define _GNU_SOURCE
#include "http/handlers.h"
#include "http/http.h"

#include "core/config.h"
#include "core/json.h"
#include "core/util.h"
#include "paths.h"

#include <stdio.h>
#include <sys/stat.h>

void
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