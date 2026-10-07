/* Per-title artwork overrides.

   Store art only exists for titles that have a PlayStation store listing, so
   anything else - homebrew, betas, region-locked titles - falls back to no
   icon at all. A user can still give those an icon: one line in art.ini per
   title, "TITLE_ID=url", checked here before the store pipeline is consulted.

   The value may be a plain https image URL, which the worker proxies to a
   Discord external asset the same way it proxies store art, or an mp: path the
   user already obtained from the external-assets endpoint. Once a plain URL
   has been proxied, this prefers the cached mp: so the raw URL is neither
   shown to Discord nor re-proxied on every refresh. */
#define _GNU_SOURCE
#include "psn/psn.h"
#include "psn/internal.h"

#include "paths.h"

#include <stdio.h>
#include <string.h>

/* Built-in defaults. art.ini overrides these; nothing else overrides either.
   Keyed on title_id, the same key the icons cache and detect_activity use. */
static const struct {
  const char *title_id;
  const char *url;
} kDefaultArt[] = {
    {"PPSA50001",
     "https://raw.githubusercontent.com/pLabs5/yFree5/refs/heads/main/"
     "tile/sce_sys/icon0.png"},
};

static int art_file_value(const char *title_id, char *out, size_t cap) {
  char line[512];
  size_t n = strlen(title_id);
  FILE *f = fopen(ART_PATH, "rb");

  if (!f) return -1;
  while (fgets(line, sizeof(line), f)) {
    char *v, *e;

    if (strncmp(line, title_id, n) != 0 || line[n] != '=') continue;
    v = line + n + 1;
    e = strchr(v, '\n');
    if (e) *e = 0;
    e = strchr(v, '\r');
    if (e) *e = 0;
    if (*v) {
      snprintf(out, cap, "%s", v);
      fclose(f);
      return 0;
    }
  }
  fclose(f);
  return -1;
}

int custom_art(const char *title_id, char *out, size_t cap) {
  char cached[320];
  unsigned i;

  if (!title_id || !title_id[0] || !out || cap == 0) return -1;
  out[0] = 0;

  if (art_file_value(title_id, out, cap) != 0) {
    for (i = 0; i < (unsigned)(sizeof(kDefaultArt) / sizeof(kDefaultArt[0]));
         i++) {
      if (strcmp(kDefaultArt[i].title_id, title_id) == 0) {
        snprintf(out, cap, "%s", kDefaultArt[i].url);
        break;
      }
    }
    if (!out[0]) return -1;
  }

  /* mp: already is a Discord asset; use it as-is. A plain URL is only
     presentable once proxied, so if that proxy already landed use it instead
     of showing raw bytes Discord will not treat as an asset key. */
  if (strncmp(out, "mp:", 3) == 0) return 0;
  if (cache_get(title_id, cached, sizeof(cached)) == 0 &&
      strncmp(cached, "mp:", 3) == 0) {
    snprintf(out, cap, "%s", cached);
  }
  return 0;
}