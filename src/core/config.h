#pragma once

#include <stddef.h>

extern const char *kConfigKeys[];
extern const char *kConfigDefaults[];
extern const int kConfigDefaultsN;

int cfg_get(const char *key, char *out, size_t cap);
int cfg_bool(const char *key, int dflt);
long long cfg_int64(const char *key, long long dflt);
int read_token(char *out, size_t cap);
int cfg_lookup(const char *cfg, const char *key, char *out, size_t cap);
void write_default_config(void);
void write_ca(void);
void sanitize_value(char *v, size_t cap);
