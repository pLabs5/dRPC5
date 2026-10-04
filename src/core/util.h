#pragma once

#include <stddef.h>

void notifyf(const char *fmt, ...);
int read_whole_file(const char *path, char *buf, size_t cap);
long long now_ms(void);
void nap_ms(int ms);
void base64(const char *in, char *out, size_t cap);
