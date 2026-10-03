#pragma once

#include <stddef.h>

typedef struct {
  int has_game;
  const char *name;
  const char *details;
  const char *state;
  long long start_epoch;
  long long end_epoch;
  const char *large_key;
  const char *large_text;
  const char *small_key;
  const char *small_text;
  int type;
} gw_activity;

int gateway_start(void);
void gateway_stop(void);
int gateway_build_presence(char *out, size_t cap, const gw_activity *act,
                           const char *status, char *published,
                           size_t pubcap);
void gateway_status(int *connected, int *ready, int *auth_failed,
                    char *activity, size_t cap);
const char *gateway_last_activity(void);