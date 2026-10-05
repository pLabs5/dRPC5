/* The two pieces of PSN art state that cross stage boundaries.

   They live in their own file so the locking discipline is stated once: g_mu
   guards the icons rewrite and the in-flight job fields, and g_proxy is
   written once at startup by the gateway and only read afterwards. */
#define _GNU_SOURCE
#include "psn/internal.h"

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
psn_proxy_fn g_proxy;
