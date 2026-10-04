#define _GNU_SOURCE
#include "api.h"
#include "http.h"

#include "core/config.h"
#include "core/json.h"
#include "core/util.h"
#include "paths.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

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

void send_response(int fd, int code, const char *reason, const char *ctype,
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

void send_json(int fd, int code, const char *json) {
  const char *reason = code < 400 ? "OK" : "Error";
  send_response(fd, code, reason, "application/json", json, strlen(json));
}

void send_redirect(int fd, const char *loc) {
  char hdr[256];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 302 Found\r\n"
                   "Location: %s\r\n"
                   "Content-Length: 0\r\n"
                   "Connection: close\r\n"
                   "Cache-Control: no-store\r\n\r\n",
                   loc);
  if(n > 0) send_all(fd, hdr, (size_t)n);
}

int peer_is_local(int fd) {
  struct sockaddr_in sin;
  socklen_t len = sizeof(sin);

  if(getpeername(fd, (struct sockaddr *)&sin, &len) != 0) return 1;
  return (ntohl(sin.sin_addr.s_addr) >> 24) == 127;
}

char * find_header(char *hdrs, const char *key) {
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

int read_line_body(char *hdrs, size_t *hlen, int fd, char *body) {
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

void * handle_conn(void *arg) {
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


static int prev_pid = -1;

void load_prev_pid(void) {
  FILE *f = fopen(STORE_DIR "/pid", "r");

  if (f) {
    if (fscanf(f, "%d", &prev_pid) != 1) prev_pid = -1;
    fclose(f);
  }
  f = fopen(STORE_DIR "/pid", "w");
  if (f) {
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
  }
}

static void
kill_stale(void) {
  if (prev_pid <= 1 || prev_pid == (int)getpid()) {
    printf("dRPC5: no previous instance recorded\n");
    return;
  }
  if (kill(prev_pid, 0) < 0) {
    printf("dRPC5: pid %d not running (errno %d)\n", prev_pid, errno);
    return;
  }
  printf("dRPC5: killing stale instance pid %d\n", prev_pid);
  if (kill(prev_pid, SIGKILL) < 0)
    printf("dRPC5: kill(%d) failed: %d\n", prev_pid, errno);
}

void lan_ip(char *out, size_t cap) {
  struct sockaddr_in sin;
  socklen_t len = sizeof(sin);
  int fd;

  snprintf(out, cap, "?");
  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if(fd < 0) return;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons(9);
  sin.sin_addr.s_addr = inet_addr("1.1.1.1");
  if(connect(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0 &&
     getsockname(fd, (struct sockaddr *)&sin, &len) == 0)
    inet_ntop(AF_INET, &sin.sin_addr, out, (socklen_t)cap);
  close(fd);
}

int httpd_run(void) {
  int lfd;
  struct sockaddr_in addr;
  int one = 1;

  lfd = socket(AF_INET, SOCK_STREAM, 0);
  if(lfd < 0) return -1;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(DRPC_PORT);
  {
    int tries;
    for(tries = 0; tries < 15; tries++) {
      if(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0) break;
      printf("drpc5: bind failed: %d (try %d)\n", errno, tries + 1);
      if(errno != EADDRINUSE) { close(lfd); return -1; }
      if(tries == 0) kill_stale();
      sleep(1);
    }
    if(tries == 15) {
      printf("drpc5: port %d busy - another dRPC5 instance is running\n", DRPC_PORT);
      close(lfd);
      return -1;
    }
  }
  if(listen(lfd, 8) < 0) {
    close(lfd);
    return -1;
  }
  {
    char ip[64];
    lan_ip(ip, sizeof(ip));
    printf("drpc5: config server on http://%s:%d/ (and 127.0.0.1)\n", ip,
           DRPC_PORT);
    printf("drpc5: paste a token from any device: http://%s:%d/pc.html\n", ip,
           DRPC_PORT);
  }

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