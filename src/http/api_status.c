/* GET /api/status - everything the console UI polls: install state,
   gateway health, the LAN address, and the whole config. */
#define _GNU_SOURCE
#include "http/handlers.h"
#include "http/http.h"

#include "core/config.h"
#include "core/json.h"
#include "core/util.h"
#include "gw/gateway.h"
#include "manifest/manifest.h"
#include "paths.h"

#include <stdio.h>
#include <sys/stat.h>

void
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
              "{\"name\":\"%s\",\"version\":\"%s\","
              "\"installed\":%d,\"port\":%d,\"has_token\":%d,"
              "\"ip\":\"%s\","
              "\"gateway\":{\"connected\":%d,\"ready\":%d,"
              "\"auth_failed\":%d,\"activity\":\"%s\"},"
              "\"config\":{",
              kManifest.app_name, kManifest.version, installed, DRPC_PORT,
              has_token, ip, gw_connected, gw_ready, gw_auth_failed, activity);
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