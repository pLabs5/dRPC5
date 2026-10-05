#pragma once

#include <stddef.h>

struct curl_slist;

int discord_init(void);
int discord_http(const char *method, const char *path, const char *body,
                 struct curl_slist *extra, int *status, char *out,
                 size_t outcap);
int ra_start(const char *public_key, char *err, size_t errcap);
int ra_poll(int id, long wait_ms, char *out, size_t cap, int *closed);
int ra_send(int id, const char *frame);
void ra_close(int id);
