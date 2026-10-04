#pragma once

#include <stddef.h>

void json_copy(char *dst, size_t cap, const char *src);
int frame_op(const char *frame, long *op);
int frame_has(const char *frame, const char *needle);
long frame_long(const char *frame, const char *key, long dflt);
int json_read_str(const char *v, char *out, size_t cap);
const char *obj_find(const char *obj, const char *key);
const char *json_obj(const char *frame, const char *key);
size_t json_escape(char *dst, size_t cap, const char *src);
int json_get(const char *body, const char *key, char *out, size_t cap);
int qs_get(const char *query, const char *key, char *out, size_t cap);
