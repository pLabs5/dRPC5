#pragma once

/* Request admission policy for the config server. Kept apart from routing so
   the rules about who may talk to this server are readable in one place.

   Both functions live in guard.c. */

/* True when `path` may be served to a peer that is not on loopback. Only the PC
   page and the token hand-off are reachable from the LAN; everything else is
   console-local. */
int lan_allowed(const char *method, const char *path);

/* True when the request must be rejected as browser-mediated cross-site
   traffic. Takes the raw header block because find_header() works on the
   in-place buffer the connection handler built. */
int cross_site(char *hdrs);
