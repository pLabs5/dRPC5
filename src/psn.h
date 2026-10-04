#pragma once

#include <stddef.h>

struct curl_slist;

typedef int (*psn_proxy_fn)(const char *url, char *out, size_t cap);

void psn_set_proxy(psn_proxy_fn fn);
struct curl_slist *psn_pinned_hosts(void);

int psn_art_cached(const char *title_id, char *out, size_t cap);
void psn_art_refresh_async(const char *title_id, const char *concept_id,
                           const char *content_id);
