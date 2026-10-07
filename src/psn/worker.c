/* Stage 4: fetching art off the request path.

   The HTTP handler must answer immediately, so it only records that a title
   wants refreshing and returns. One background thread does the actual work,
   with a per-title cooldown (PSN_RETRY_MS) so a store outage cannot turn
   into a request loop. */
#define _GNU_SOURCE
#include "psn/psn.h"
#include "psn/internal.h"

#include "core/config.h"
#include "core/util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_pending[16];
static char g_last_try[16];
static long long g_last_ms;
struct psn_job {
  char title_id[16];
  char concept_id[16];
  char content_id[48];
};

static void *psn_worker(void *arg) {
  struct psn_job job = *(struct psn_job *)arg;
  char url[320], cached[320], proxied[400];

  free(arg);
  url[0] = 0;
  cached[0] = 0;

  if (custom_art(job.title_id, url, sizeof(url)) == 0) {
    if (strncmp(url, "mp:", 3) == 0) {
      dlogf("drpc5: art %s override\n", job.title_id);
      goto done;
    }
  } else if (cache_get(job.title_id, cached, sizeof(cached)) == 0 &&
             strncmp(cached, "mp:", 3) != 0) {
    snprintf(url, sizeof(url), "%s", cached);
  } else if (fetch_art(job.concept_id, job.content_id, url, sizeof(url)) != 0) {
    dlogf("drpc5: art %s lookup failed\n", job.title_id);
    goto done;
  }

  if (g_proxy && g_proxy(url, proxied, sizeof(proxied)) == 0) {
    dlogf("drpc5: art %s %s\n", job.title_id, proxied);
    cache_put(job.title_id, proxied);
  } else if (strcmp(url, cached) != 0) {
    dlogf("drpc5: art %s raw %s\n", job.title_id, url);
    cache_put(job.title_id, url);
  } else {
    dlogf("drpc5: art %s proxy retry\n", job.title_id);
  }

done:
  pthread_mutex_lock(&g_mu);
  g_pending[0] = 0;
  pthread_mutex_unlock(&g_mu);
  return NULL;
}

int psn_art_cached(const char *title_id, char *out, size_t cap) {
  if (!title_id || !title_id[0] || !out || cap == 0) return -1;
  out[0] = 0;
  return cache_get(title_id, out, cap);
}

void psn_art_refresh_async(const char *title_id, const char *concept_id,
                           const char *content_id) {
  struct psn_job *job;
  pthread_t th;
  long long now;

  if (!title_id || !title_id[0]) return;

  now = mono_ms();
  pthread_mutex_lock(&g_mu);
  if (g_pending[0] || (g_last_try[0] && !strcmp(g_last_try, title_id) &&
                       now - g_last_ms < cfg_clamped("psn_retry_ms", 30000, 1000, 3600000))) {
    pthread_mutex_unlock(&g_mu);
    return;
  }
  snprintf(g_pending, sizeof(g_pending), "%s", title_id);
  snprintf(g_last_try, sizeof(g_last_try), "%s", title_id);
  g_last_ms = now;
  pthread_mutex_unlock(&g_mu);

  job = calloc(1, sizeof(*job));
  if (!job) {
    pthread_mutex_lock(&g_mu);
    g_pending[0] = 0;
    pthread_mutex_unlock(&g_mu);
    return;
  }
  snprintf(job->title_id, sizeof(job->title_id), "%s", title_id);
  if (concept_id)
    snprintf(job->concept_id, sizeof(job->concept_id), "%s", concept_id);
  if (content_id)
    snprintf(job->content_id, sizeof(job->content_id), "%s", content_id);

  if (pthread_create(&th, NULL, psn_worker, job) != 0) {
    free(job);
    pthread_mutex_lock(&g_mu);
    g_pending[0] = 0;
    pthread_mutex_unlock(&g_mu);
    return;
  }
  pthread_detach(th);
}
