#pragma once

#include <stddef.h>

#define HDR_MAX   16384
#define BODY_MAX  49152
#define JSON_MAX  65536

void send_response(int fd, int code, const char *reason, const char *ctype,
                   const void *body, size_t len);
void send_json(int fd, int code, const char *json);
void send_redirect(int fd, const char *loc);
int peer_is_local(int fd);
void lan_ip(char *out, size_t cap);
void load_prev_pid(void);
void *handle_conn(void *arg);
int httpd_run(void);
