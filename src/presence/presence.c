#define _GNU_SOURCE
#include "core/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <unistd.h>

#include "presence/presence.h"

typedef struct app_info {
  uint32_t app_id;       /* 0x00 */
  uint32_t unknown1;     /* 0x04 */
  uint64_t unknown2;     /* 0x08 */
  char     title_id[10]; /* 0x10 */
  char     pad[110];
} app_info_t;

int sceKernelGetAppInfo(int pid, app_info_t *info);

#ifndef APPMETA_ROOT_A
#define APPMETA_ROOT_A "/user/appmeta/%s"
#endif
#ifndef APPMETA_ROOT_B
#define APPMETA_ROOT_B "/system_data/priv/appmeta/%s"
#endif

static const char *kAppmetaRoots[] = {
  APPMETA_ROOT_A,
  APPMETA_ROOT_B
};

static void
copy_str(char *dst, size_t cap, const char *src) {
  size_t n = strlen(src);
  if(n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

static int
file_exists(const char *path) {
  return access(path, R_OK) == 0;
}

static int
read_file(const char *path, char *buf, size_t cap, size_t *out_len) {
  return read_file_all(path, buf, cap, out_len) > 0 ? 0 : -1;
}

static int
json_str(const char *json, size_t len, const char *key, char *out, size_t cap) {
  char needle[64];
  const char *found = NULL;
  const char *end;
  size_t need;
  size_t i;
  int nl;

  if(cap == 0) return -1;
  out[0] = 0;
  nl = snprintf(needle, sizeof(needle), "\"%s\"", key);
  if(nl <= 0 || (size_t)nl >= sizeof(needle)) return -1;
  need = (size_t)nl;
  end = json + len;

  for(i = 0; i + need <= len; i++) {
    if(memcmp(json + i, needle, need) == 0) {
      found = json + i + need;
      break;
    }
  }
  if(!found) return -1;

  while(found < end &&
        (*found == ' ' || *found == ':' || *found == '\t' ||
         *found == '\n' || *found == '\r'))
    found++;
  if(found >= end || *found != '"') return -1;
  found++;

  i = 0;
  while(found < end) {
    if(*found == '\\' && found + 1 < end) {
      found++;
      if(i + 1 >= cap) break;
      out[i++] = *found++;
      continue;
    }
    if(*found == '"') break;
    if(i + 1 >= cap) break;
    out[i++] = *found++;
  }
  out[i] = 0;
  return 0;
}

static int
title_id_ok(const char *s) {
  size_t i;
  if(!s || strlen(s) < 5) return 0;
  for(i = 0; s[i]; i++) {
    char c = s[i];
    if(!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return 0;
  }
  return 1;
}

static void
fill_from_param(const char *json, size_t len, ps5_app_t *app) {
  if(!app->name[0])
    json_str(json, len, "titleName", app->name, sizeof(app->name));
  if(!app->version[0])
    json_str(json, len, "contentVersion", app->version, sizeof(app->version));
  if(!app->version[0])
    json_str(json, len, "originContentVersion", app->version,
             sizeof(app->version));
  if(!app->version[0])
    json_str(json, len, "masterVersion", app->version, sizeof(app->version));
  if(!app->content_id[0])
    json_str(json, len, "contentId", app->content_id, sizeof(app->content_id));
  if(!app->concept_id[0])
    json_str(json, len, "conceptId", app->concept_id, sizeof(app->concept_id));
}

static void
resolve_meta(ps5_app_t *app) {
  char dir[160];
  char path[256];
  char buf[16384];
  size_t len;
  size_t i;

  if(!app->title_id[0]) return;

  for(i = 0; i < sizeof(kAppmetaRoots) / sizeof(kAppmetaRoots[0]); i++) {
    snprintf(dir, sizeof(dir), kAppmetaRoots[i], app->title_id);

    if(!app->icon_path[0]) {
      snprintf(path, sizeof(path), "%s/icon0.png", dir);
      if(file_exists(path)) copy_str(app->icon_path, sizeof(app->icon_path),
                                     path);
    }

    snprintf(path, sizeof(path), "%s/sce_sys/param.json", dir);
    if(read_file(path, buf, sizeof(buf), &len) == 0)
      fill_from_param(buf, len, app);

    snprintf(path, sizeof(path), "%s/param.json", dir);
    if(read_file(path, buf, sizeof(buf), &len) == 0)
      fill_from_param(buf, len, app);
  }

  if(!app->name[0]) copy_str(app->name, sizeof(app->name), app->title_id);
}

int
presence_foreground(ps5_app_t *out) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  struct kinfo_proc *ki;
  char *ptr;
  char *end;
  void *buf;
  size_t buf_size = 0;
  long best_start = -1;
  int best_running = -1;
  int found = 0;

  if(!out) return -1;
  memset(out, 0, sizeof(*out));

  if(sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0 || buf_size == 0) {
    dlogf("drpc5: sysctl size errno=%d\n", errno);
    return -1;
  }
  buf = malloc(buf_size);
  if(!buf) return -1;
  if(sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
    dlogf("drpc5: sysctl read errno=%d\n", errno);
    free(buf);
    return -1;
  }

  ptr = (char *)buf;
  end = ptr + buf_size;
  while(ptr < end) {
    app_info_t ai;
    int running;
    long started;

    ki = (struct kinfo_proc *)ptr;
    if(ki->ki_structsize <= 0) break;
    ptr += ki->ki_structsize;

    if(strncmp(ki->ki_comm, "eboot.bin", 9) != 0) continue;

    memset(&ai, 0, sizeof(ai));
    if(sceKernelGetAppInfo(ki->ki_pid, &ai) != 0) continue;
    ai.title_id[sizeof(ai.title_id) - 1] = 0;
    if(!title_id_ok(ai.title_id)) continue;
    if(!strcmp(ai.title_id, "00000000")) continue;

    running = ki->ki_stat == SRUN;
    started = (long)ki->ki_start.tv_sec;
    if(best_running >= 0 &&
       !(running > best_running ||
         (running == best_running && started > best_start)))
      continue;

    best_running = running;
    best_start = started;
    out->pid = (int)ki->ki_pid;
    out->app_id = ai.app_id;
    out->running = running;
    out->start_epoch = (int64_t)started;
    copy_str(out->title_id, sizeof(out->title_id), ai.title_id);
    found = 1;
  }

  free(buf);
  if(!found) return -1;
  resolve_meta(out);
  return 0;
}