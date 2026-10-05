#pragma once

/* Our own resolver, used for every outbound connection.

   The console has a resolver of its own, but it is not ours to choose: it is
   whatever the network the console joined hands out. Everything here goes
   through libcurl, and by default libcurl would use that resolver. So each
   request resolves its own host through dns_resolve() and hands libcurl the
   address as a CURLOPT_RESOLVE pin.

   Pinning the address is not the same as trusting the address: the request
   still goes out with the real hostname, so TLS SNI and certificate
   verification are unchanged. We only decide where the name is looked up.

   If our resolver fails we say so and let the request fall back to libcurl's
   resolver rather than failing outright - a DNS outage should not look
   different from a Discord outage in the logs. */

#include <curl/curl.h>

#include <stddef.h>

struct curl_slist;

/* Resolve one hostname to an IPv4 address using the configured resolvers.
   Results, including failures, are cached briefly so a reconnect loop does not
   turn into a query loop. Returns 0 on success, -1 on failure. */
int dns_resolve(const char *host, char *ip, size_t cap);

/* Resolve host and return a CURLOPT_RESOLVE list holding "host:port:ip", or
   NULL if it could not be resolved (in which case the caller should carry on
   unpinned). The caller owns the list and frees it with
   curl_slist_free_all() once the transfer is done. */
struct curl_slist *dns_pin(const char *host, int port);

/* As dns_pin(), but takes the URL and works the host and port out of it.
   Returns NULL if the URL has no host we can resolve. */
struct curl_slist *dns_pin_url(const char *url);
