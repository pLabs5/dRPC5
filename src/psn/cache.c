/* Stage 3: the on-disk icon map.

   A flat "TITLE_ID=url" text file, rewritten through a temp file and one
   rename so a reader never sees a partial map, and so a PSN outage cannot
   take out art that was already fetched. */
#define _GNU_SOURCE
#include "psn/psn.h"
#include "psn/internal.h"

#include "paths.h"

#include <stdio.h>
#include <string.h>

int cache_get(const char *title_id, char *out, size_t cap) {
  char line[512];
  size_t n = strlen(title_id);
  FILE *f = fopen(ICONS_PATH, "rb");

  if (!f) return -1;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, title_id, n) != 0 || line[n] != '=') continue;
    {
      char *v = line + n + 1;
      char *e = strchr(v, '\n');

      if (e) *e = 0;
      e = strchr(v, '\r');
      if (e) *e = 0;
      if (*v) {
        snprintf(out, cap, "%s", v);
        fclose(f);
        return 0;
      }
    }
  }
  fclose(f);
  return -1;
}

void cache_put(const char *title_id, const char *url) {
  char line[512];
  size_t n = strlen(title_id);
  FILE *f;

  pthread_mutex_lock(&g_mu);
  f = fopen(ICONS_PATH, "rb");
  if (f) {
    FILE *t = fopen(ICONS_PATH ".tmp", "wb");

    if (t) {
      while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, title_id, n) == 0 && line[n] == '=') continue;
        fputs(line, t);
      }
      fprintf(t, "%s=%s\n", title_id, url);
      fclose(t);
      fclose(f);
      rename(ICONS_PATH ".tmp", ICONS_PATH);
    } else {
      fclose(f);
    }
  } else {
    f = fopen(ICONS_PATH, "ab");
    if (f) {
      fprintf(f, "%s=%s\n", title_id, url);
      fclose(f);
    }
  }
  pthread_mutex_unlock(&g_mu);
}
