#pragma once

#include <stddef.h>

/* ===========================================================================
   Build and identity metadata for the payload.

   The values live in src/manifest.c. That file is the only place they are
   defined; every other file reads them through kManifest. Nothing here is
   configurable at runtime and nothing is read from the config file.
   =========================================================================== */
typedef struct {
  /* Short name shown in the UI, the notification text and the install
     metadata. */
  const char *app_name;

  /* dRPC5's own version. Bump this on a release. It is cosmetic: it is
     reported by /api/status and used in log and notification text, and
     nothing gates behaviour on it. This is the ONLY version field - the
     package stamp is derived from it by the Makefile, so the installed tile
     and this value cannot drift apart. */
  const char *version;

  /* Who wrote it, and where it now lives. Attribution is a factual claim about
     authorship, so it is kept next to the licence rather than in user-facing
     notification text. */
  const char *dev_name;
  const char *org_name;

  /* SPDX identifier. dRPC5 is AGPL-3.0-only, not -or-later: forks must use
     exactly this version. Vendored third-party components keep their own
     licences; see README.md and third_party/LICENSES/. */
  const char *license;

  /* Package identity. title_id must match TITLE_ID in the Makefile, which is
     what the paths in src/paths.h are built from; the two are checked against
     each other by the build. */
  const char *title_id;
  const char *content_id;

  /* Canonical repository, used in install metadata and user-facing text. */
  const char *repo_url;

  /* The console version dRPC5 claims to be when it talks to Discord, sent as
     the private `gw_version` gateway property. This is NOT dRPC5's own
     version: it exists so the account lands in Discord's console bucket
     instead of the mobile one. It must stay "1.00" unless the values
     Sce.Vsh.DiscordAccessor.dll actually reports are known to have changed,
     because a wrong value makes the account look like a different client. */
  const char *client_version;
} manifest_t;

extern const manifest_t kManifest;

/* "dRPC5 1.00" - a single line naming the payload and its version, for log
   headers and notification text. Always NUL-terminates when cap > 0. */
void manifest_identity(char *out, size_t cap);
