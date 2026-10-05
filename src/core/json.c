#define _GNU_SOURCE
#include "core/json.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void json_copy(char *dst, size_t cap, const char *src) {
  json_escape(dst, cap, src);
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
  if(!src) src = "";
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
  const char *p, *end;
  size_t o = 0;

  if(cap == 0) return -1;
  out[0] = 0;
  /* A request with no body reaches the handlers as NULL. */
  if(!body) return -1;
  /* obj_find only matches a real member of the top-level object, so a key that
     merely appears inside another key's string value cannot be picked up. */
  p = obj_find(body, key);
  if(!p) return -1;

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

  /* Scan a bare value to its end, tracking nesting and strings so an object or
     array value stops at the matching close instead of running to the next
     comma at depth 0. */
  {
    int depth = 0, instr = 0;
    end = p;
    for(; *end; end++) {
      char c = *end;
      if(instr) {
        if(c == '\\') { end++; continue; }
        if(c == '"') instr = 0;
        continue;
      }
      if(c == '"') { instr = 1; continue; }
      if(c == '{' || c == '[') { depth++; continue; }
      if(c == '}' || c == ']') {
        if(depth == 0) break;
        depth--;
        continue;
      }
      if(c == ',' && depth == 0) break;
    }
  }
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
int json_append(char *buf, size_t cap, size_t *off, int *out_overflow,
                const char *fmt, ...) {
  va_list ap;
  int n;

  if(!buf || !off || cap == 0) return 1;
  if(*off > cap - 1) *off = cap - 1;
  va_start(ap, fmt);
  n = vsnprintf(buf + *off, cap - *off, fmt, ap);
  va_end(ap);
  if(n < 0) {
    if(out_overflow) *out_overflow = 1;
    return 1;
  }
  if((size_t)n >= cap - *off) {
    /* Truncated: clamp to what actually fit so the next call is still bounded. */
    *off = cap - 1;
    if(out_overflow) *out_overflow = 1;
    return 1;
  }
  *off += (size_t)n;
  return 0;
}
