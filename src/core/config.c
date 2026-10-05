#define _GNU_SOURCE
#include "core/config.h"

#include "generated/bundled_ca.h"
#include "core/util.h"
#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

const char * kConfigKeys[] = {
    "enabled", "app_id", "mode", "status",
    "src_game", "src_media", "src_app", "src_idle",
    "media_line", "show_artwork", "show_platform",
    "name", "state", "details", "type",
    "asset_key", "asset_text", "asset_small_key", "asset_small_text",
    "start_timestamp", "end_timestamp", "platform",
    "gw_os", "gw_browser", "gw_version", "gw_device",
    "poll_ms", "hb_min_ms", "psn_retry_ms", "resync_ms", "reidentify_ms",
    "dns_servers", NULL};

void write_ca(void) {
  FILE *f;
  if ((f = fopen(CA_PATH, "wb"))) {
    if (fwrite(kCaPem, 1, kCaPemSize, f) != kCaPemSize) dlogf("drpc5: short write " CA_PATH "\n");
    fclose(f);
  }
}

const char *kConfigDefaults[] = {
    "1", "", "auto", "online",
    "1", "1", "0", "1", "", "1", "1",
    "", "", "", "0", "", "", "", "", "", "", "ps5",
    "Playstation", "PS5 GameBase", "1.00", "PS5",
    "10000", "5000", "30000", "600000", "150",
    "1.1.1.1,8.8.8.8,9.9.9.9", NULL};

#define NCFG_DEFAULTS ((int)(sizeof(kConfigDefaults) / sizeof(kConfigDefaults[0])) - 1)
const int kConfigDefaultsN = NCFG_DEFAULTS;

int cfg_lookup(const char *cfg, const char *key, char *out, size_t cap) {
  char pat[64];
  const char *s;
  const char *hit = NULL;
  size_t klen;

  if(cap == 0) return -1;
  out[0] = 0;
  if(!cfg || !cfg[0]) return -1;
  snprintf(pat, sizeof(pat), "%s=", key);
  klen = strlen(pat);
  s = cfg;
  while((s = strstr(s, pat)) != NULL) {
    if(s == cfg || s[-1] == '\n') { hit = s; break; }
    s += klen;
  }
  if(!hit) return -1;
  {
    const char *e = strchr(hit + klen, '\n');
    size_t vl = e ? (size_t)(e - (hit + klen)) : strlen(hit + klen);
    if(vl >= cap) vl = cap - 1;
    memcpy(out, hit + klen, vl);
    out[vl] = 0;
  }
  return 0;
}

void write_default_config(void) {
  char cfg[4096];
  char val[256];
  char out[12288];
  size_t n = 0;
  size_t off = 0;
  struct stat st;
  FILE *f;
  int i;

  cfg[0] = 0;
  if(stat(CONFIG_PATH, &st) == 0 && (f = fopen(CONFIG_PATH, "r")) != NULL) {
    n = fread(cfg, 1, sizeof(cfg) - 1, f);
    cfg[n] = 0;
    fclose(f);
  }
  out[0] = 0;
  for(i = 0; kConfigKeys[i] && kConfigDefaults[i]; i++) {
    int w;
    if(cfg_lookup(cfg, kConfigKeys[i], val, sizeof(val)) != 0 || !val[0]) {
      if(kConfigDefaults[i][0])
        snprintf(val, sizeof(val), "%s", kConfigDefaults[i]);
    }
    w = snprintf(out + off, sizeof(out) - off, "%s=%s\n", kConfigKeys[i], val);
    /* On truncation, leaving off unchanged makes the next iteration overwrite
       from out[0], which drops earlier keys and can leave a config holding only
       the last key. Stop instead. */
    if(w < 0 || (size_t)w >= sizeof(out) - off) {
      dlogf("drpc5: config too large, not rewriting defaults\n");
      return;
    }
    off += (size_t)w;
  }
  if(off == n && n && !memcmp(cfg, out, off)) return;
  if(write_file_atomic(CONFIG_PATH, out, off, 0644) != 0) return;
  dlogf("drpc5: config filled with defaults\n");
}

void sanitize_value(char *v, size_t cap) {
  size_t o = 0;
  size_t i;
  for(i = 0; v[i] && o + 1 < cap; i++) {
    unsigned char c = (unsigned char)v[i];
    if(c == '\r' || c == '\n' || c == '\t') continue;
    if(c < 0x20) continue;
    v[o++] = (char)c;
  }
  v[o] = 0;
}

int cfg_get(const char *key, char *out, size_t cap) {
  char line[512];
  FILE *f;
  size_t klen = strlen(key);
  int found = 0;

  if (cap == 0) return -1;
  out[0] = 0;
  f = fopen(CONFIG_PATH, "r");
  if (!f) return -1;
  while (fgets(line, sizeof(line), f)) {
    char *nl = strpbrk(line, "\r\n");
    /* A line that filled the buffer without a newline continues; skip the rest
       of it, otherwise its tail is parsed as a fresh "key=value" pair. */
    if (!nl && !feof(f)) {
      int c;
      while ((c = fgetc(f)) != EOF && c != '\n') { }
      continue;
    }
    if (nl) *nl = 0;
    if (strncmp(line, key, klen) != 0 || line[klen] != '=') continue;
    snprintf(out, cap, "%s", line + klen + 1);
    found = 1;
    break;
  }
  fclose(f);
  return found ? 0 : -1;
}

int cfg_bool(const char *key, int dflt) {
  char buf[64];
  if (cfg_get(key, buf, sizeof(buf)) != 0 || buf[0] == 0) return dflt;
  if (!strcmp(buf, "1") || !strcasecmp(buf, "true") || !strcasecmp(buf, "on") ||
      !strcasecmp(buf, "yes"))
    return 1;
  if (!strcmp(buf, "0") || !strcasecmp(buf, "false") || !strcasecmp(buf, "off") ||
      !strcasecmp(buf, "no"))
    return 0;
  return dflt;
}

long long cfg_int64(const char *key, long long dflt) {
  char buf[32];
  char *end;
  long long v;
  if (cfg_get(key, buf, sizeof(buf)) != 0 || buf[0] == 0) return dflt;
  v = strtoll(buf, &end, 10);
  /* No digits at all: treat a typo'd value as unset so the caller gets the
     documented default rather than whatever the clamp boundary happens to be. */
  if (end == buf) return dflt;
  return v;
}

long long cfg_clamped(const char *key, long long dflt, long long lo,
                      long long hi) {
  long long v = cfg_int64(key, dflt);
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

int read_token(char *out, size_t cap) {
  int n, overflow = 0;

  if (cap == 0) return -1;
  out[0] = 0;
  n = read_file_ex(TOKEN_PATH, out, cap, NULL, &overflow);
  if (n < 0) return -1;
  /* A token that did not fit is truncated and would be sent to Discord as a
     bad credential, which looks exactly like an expired token. Fail loudly
     instead: a token this long means TOKEN_PATH is not what we think it is. */
  if (overflow) {
    dlogf("drpc5: token exceeds %d bytes, refusing to use it", (int)cap);
    out[0] = 0;
    return -1;
  }
  while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
    out[--n] = 0;
  if (n < 20) {
    dlogf("drpc5: token is only %d bytes, too short to be valid", n);
    return -1;
  }
  return 0;
}