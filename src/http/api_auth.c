/* Remote Auth: /api/ra/start, /poll, /send, /close.

   Backs the QR and remote-login sign-in paths. A session is opened against the
   public key the browser supplies, polled for frames on a long-poll timeout, and
   fed data from the page. */
#define _GNU_SOURCE
#include "http/handlers.h"
#include "http/http.h"

#include "core/json.h"
#include "discord/discord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
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

void
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

void
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

void
handle_ra_close(int fd, const char *body) {
  char idbuf[16];

  if(json_get(body, "id", idbuf, sizeof(idbuf)) != 0) {
    send_json(fd, 400, "{\"error\":\"missing id\"}");
    return;
  }
  ra_close(atoi(idbuf));
  send_json(fd, 200, "{\"ok\":1}");
}