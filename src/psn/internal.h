#pragma once

/* Internal to the PSN art pipeline: dns.c, art.c, cache.c and worker.c each
   own one stage, and the stages hand work to each other through here rather
   than through psn.h, which stays the public surface. */

#include "psn/psn.h"

#include <pthread.h>
#include <stddef.h>

/* Upper bound on a store response we will hold in memory. The store returns
   HTML for a concept page, so this is generous on purpose. */
#define PSN_PAGE (1 << 20)

/* The icons file is a shared flat "TITLE=url" map, and only one lookup or
   fetch may be in flight at a time. Both the API thread (reads) and the
   worker thread (writes) take this. */
extern pthread_mutex_t g_mu;

/* Optional host-mapping hook, set from the gateway via psn_set_proxy() and
   read by the worker when it decides what to cache. */
extern psn_proxy_fn g_proxy;

/* dns.c -> art.c */
int psn_get(const char *url, int head, long *code, char *ct, size_t ctc,
            char *body);

/* art.c -> worker.c */
int fetch_art(const char *concept_id, const char *content_id, char *url,
              size_t cap);

/* cache.c -> worker.c and psn_art_cached() */
int cache_get(const char *title_id, char *out, size_t cap);
void cache_put(const char *title_id, const char *url);
