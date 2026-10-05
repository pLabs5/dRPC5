#pragma once

/* Request handlers, one pair per file in this directory. Internal to the HTTP
   server: route() in route.c is the only caller. Each handler owns its own
   response and returns, so route.c stays a flat dispatch table.

   Handlers that need a request body take the buffered body; handlers that read
   query parameters take the raw query string. */

void handle_status(int fd);
void handle_config(int fd, const char *body);
void handle_token(int fd, const char *body);
void handle_token_delete(int fd);
void handle_discord(int fd, const char *body);
void handle_ra_start(int fd, const char *body);
void handle_ra_poll(int fd, const char *query);
void handle_ra_send(int fd, const char *body);
void handle_ra_close(int fd, const char *body);
void handle_presence(int fd);
void handle_icon(int fd);
void handle_frame(int fd);
