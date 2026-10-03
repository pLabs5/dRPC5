#pragma once

#include <stdint.h>

typedef struct ps5_app {
  int      pid;
  uint32_t app_id;
  uint32_t app_type;
  char     title_id[16];
  char     name[128];
  char     version[16];
  char     icon_path[128];
  int64_t  start_epoch;
  int      running;
} ps5_app_t;

int presence_foreground(ps5_app_t *out);