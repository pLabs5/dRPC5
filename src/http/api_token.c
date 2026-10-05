/* POST /api/token and POST /api/token/delete.

   The token is the Discord user token the gateway authenticates with. It is
   written 0600 and atomically: a torn token would still satisfy read_token's
   length check and permanently break gateway auth until it was re-pasted. */
#define _GNU_SOURCE
#include "http/handlers.h"
#include "http/http.h"

#include "core/config.h"
#include "core/json.h"
#include "core/util.h"
#include "paths.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void
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