#pragma once

#include "gw/gateway.h"
#include "core/curl_api.h"
#include "core/curlx.h"

#include <stddef.h>

#define GW_URL       "wss://gateway.discord.gg"
#define GW_QUERY     "/?v=10&encoding=json"
#define GW_QUERY_NOSLASH "?v=10&encoding=json"
#define GW_FRAME     8192
#define GW_MSG       (16u << 20)
#define GW_URL_MAX   256
#define GW_BACKOFF_BASE 1000
#define GW_BACKOFF_MAX  60000
#define GW_BACKOFF_MAX_SHIFT 6
#define GW_RESET_SESSION_AFTER 4
#define GW_CAPABILITIES 30717
#define GW_CLIENT_BUILD 267206
#define GW_CLIENT_VERSION "267.0"
#define GW_RELEASE_CHANNEL "googleRelease"
#define GW_USER_AGENT "Discord-Android/267206;RNA"
#define GW_ORIGIN     "https://discord.com"

extern long long g_time_off;
extern int g_time_known;
extern long long g_time_synced_ms;

int ws_send(CURL *easy, const char *frame);
int connect_ws(CURL *easy, const char *url);
int send_identify(CURL *easy, const char *token);
int send_resume(CURL *easy, const char *token, const char *session_id,
                long seq);
void gw_build_url(char *out, size_t cap, const char *base);
int send_heartbeat(CURL *easy);
int clear_presence(CURL *easy);
int non_resumable(int code);
void sync_time_offset(void);
int proxy_external_asset(const char *url, char *out, size_t cap);
void reload_config(char *status, size_t status_cap, char *mode,
                   size_t mode_cap, char *name, size_t name_cap,
                   char *details, size_t details_cap, char *state,
                   size_t state_cap, char *large_key, size_t large_key_cap,
                   char *large_text, size_t large_text_cap, char *small_key,
                   size_t small_key_cap, char *small_text,
                   size_t small_text_cap);
void detect_activity(gw_activity *act, int src_game, int show_platform,
                     int show_artwork, const char *manual_name,
                     const char *manual_details, const char *manual_state,
                     const char *large_key, const char *large_text,
                     const char *small_key, const char *small_text);
