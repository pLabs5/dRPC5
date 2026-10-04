#define _GNU_SOURCE
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void json_copy(char *dst, size_t cap, const char *src) {
  size_t o = 0;
  if (!cap) return;
  if (!src) src = "";
  while (*src && o + 7 < cap) {
    unsigned char c = (unsigned char)*src++;
    if (c == '"' || c == '\\') {
      dst[o++] = '\\';
      dst[o++] = (char)c;
    } else if (c == '\n') {
      dst[o++] = '\\';
      dst[o++] = 'n';
    } else if (c == '\r') {
      dst[o++] = '\\';
      dst[o++] = 'r';
    } else if (c == '\t') {
      dst[o++] = '\\';
      dst[o++] = 't';
    } else if (c < 0x20) {
      o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
    } else {
      dst[o++] = (char)c;
    }
  }
  dst[o] = 0;
}

int frame_op(const char *frame, long *op) {
  const char *p = strstr(frame, "\"op\":");
  if (!p) return -1;
  p += 5;
  while (*p == ' ') p++;
  *op = strtol(p, NULL, 10);
  return 0;
}

int frame_has(const char *frame, const char *needle) {
  return strstr(frame, needle) != NULL;
}

long frame_long(const char *frame, const char *key, long dflt) {
  const char *p = strstr(frame, key);
  char *end;
  long v;
  if (!p) return dflt;
  p += strlen(key);
  while (*p == ' ') p++;
  if (*p < '0' || *p > '9') return dflt;
  v = strtol(p, &end, 10);
  return end == p ? dflt : v;
}

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}

int json_read_str(const char *v, char *out, size_t cap) {
  size_t i = 0;

  if (cap == 0 || !v) return -1;
  out[0] = 0;
  v = skip_ws(v);
  if (*v == '"') {
    v++;
    while (*v && *v != '"') {
      if (*v == '\\' && v[1]) {
        v++;
        if (i + 1 < cap) out[i++] = *v;
        v++;
        continue;
      }
      if (i + 1 < cap) out[i++] = *v;
      v++;
    }
  } else {
    while (*v && *v != ',' && *v != '}' && *v != ']' && i + 1 < cap)
      out[i++] = *v++;
  }
  out[i] = 0;
  return i ? 0 : -1;
}

const char * obj_find(const char *obj, const char *key) {
  int depth = 0;
  int instr = 0;
  size_t klen = strlen(key);
  const char *p;

  if (!obj) return NULL;
  obj = skip_ws(obj);
  if (*obj != '{') return NULL;
  for (p = obj; *p; p++) {
    char c = *p;

    if (instr) {
      if (c == '\\') { p++; continue; }
      if (c == '"') instr = 0;
      continue;
    }
    if (c == '"') {
      if (depth == 1 && (p > obj && (p[-1] == '{' || p[-1] == ','))) {
        const char *k = p + 1;
        const char *e = strchr(k, '"');
        if (e && (size_t)(e - k) == klen && !strncmp(k, key, klen)) {
          const char *q = skip_ws(e + 1);
          if (*q == ':') return skip_ws(q + 1);
        }
      }
      instr = 1;
      continue;
    }
    if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') {
      depth--;
      if (depth <= 0) return NULL;
    }
  }
  return NULL;
}

const char * json_obj(const char *frame, const char *key) {
  const char *d = strstr(frame, "\"d\":{");
  if (!d) d = strstr(frame, "\"d\": {");
  if (!d) return NULL;
  d = strchr(d, '{');
  if (!d) return NULL;
  if (!key) return d;
  {
    const char *v = obj_find(d, key);
    if (!v) return NULL;
    return skip_ws(v);
  }
}

size_t json_escape(char *dst, size_t cap, const char *src) {
  size_t o = 0;
  if(!cap) return 0;
  while(*src && o + 7 < cap) {
    unsigned char c = (unsigned char)*src++;
    if(c == '"' || c == '\\') {
      dst[o++] = '\\';
      dst[o++] = (char)c;
    } else if(c == '\n') {
      dst[o++] = '\\';
      dst[o++] = 'n';
    } else if(c == '\r') {
      dst[o++] = '\\';
      dst[o++] = 'r';
    } else if(c == '\t') {
      dst[o++] = '\\';
      dst[o++] = 't';
    } else if(c < 0x20) {
      o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
    } else {
      dst[o++] = (char)c;
    }
  }
  dst[o] = 0;
  return o;
}

int json_get(const char *body, const char *key, char *out, size_t cap) {
  char pat[80];
  const char *p;
  const char *end;
  size_t o = 0;

  if(cap == 0) return -1;
  out[0] = 0;
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  p = strstr(body, pat);
  if(!p) return -1;
  p += strlen(pat);
  while(*p == ' ' || *p == '\t') p++;
  if(*p != ':') return -1;
  p++;
  while(*p == ' ' || *p == '\t') p++;

  if(*p == '"') {
    p++;
    while(*p && *p != '"') {
      if(*p == '\\' && p[1]) {
        p++;
        if(o + 1 < cap) out[o++] = *p;
        p++;
        continue;
      }
      if(o + 1 < cap) out[o++] = *p;
      p++;
    }
    out[o] = 0;
    return 0;
  }

  end = p;
  while(*end && *end != ',' && *end != '}' && *end != '\r' && *end != '\n')
    end++;
  while(end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
  o = (size_t)(end - p);
  if(o >= cap) o = cap - 1;
  memcpy(out, p, o);
  out[o] = 0;
  return 0;
}

int qs_get(const char *query, const char *key, char *out, size_t cap) {
  char pat[64];
  const char *p;
  const char *end;
  size_t o = 0;

  out[0] = 0;
  if(!query) return -1;
  snprintf(pat, sizeof(pat), "%s=", key);
  p = strstr(query, pat);
  if(!p) return -1;
  p += strlen(pat);
  end = p;
  while(*end && *end != '&') end++;
  while(p < end && o + 1 < cap) out[o++] = *p++;
  out[o] = 0;
  return 0;
}