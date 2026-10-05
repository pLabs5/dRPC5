/* Request dispatch: path, method and admission checks to a handler.

   Also serves the embedded web UI. Kept as one flat table on purpose - this is
   the map of what the server exposes, and it should be readable top to bottom. */
#define _GNU_SOURCE
#include "http/api.h"
#include "http/guard.h"
#include "http/handlers.h"
#include "http/http.h"
#include "http/web.h"
#include "paths.h"

#include <string.h>
#include <unistd.h>

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