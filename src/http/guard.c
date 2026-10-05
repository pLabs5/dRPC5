#include "http/guard.h"

#include "http/http.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "core/util.h"

/* Reachable from the rest of the LAN: the PC page, and the one thing it is for
   - handing us a token. It also reads status to render the pills next to the
   token box. Everything else (config, presence, token deletion, remote auth,
   the console UI itself) is loopback-only. */
int lan_allowed(const char *method, const char *path) {
  if(!strcmp(method, "POST")) return !strcmp(path, "/api/token");
  if(strcmp(method, "GET")) return 0;
  return !strcmp(path, "/pc.html") || !strcmp(path, "/token") ||
         !strcmp(path, "/qrcode.js") || !strcmp(path, "/api/status");
}

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
int cross_site(char *hdrs) {
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
