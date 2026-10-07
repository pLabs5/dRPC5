#include "manifest/manifest.h"

#include <stdio.h>

/* ===========================================================================
   EDIT THESE VALUES.

   This is the single definition of dRPC5's identity. On a release: bump
   `version`. That is the only number to touch - the package stamp is
   derived from it by the Makefile.

   One trap worth knowing about:

     - `version` and `client_version` look alike and mean different things.
       `version` is dRPC5. `client_version` is the PS5 version dRPC5 lies
       about to Discord, and changing it moves your account into a different
       bucket in Discord's client list. Leave it at 1.00.

   Write `version` however you like ("1.0", "0.5", "2.1"); the package stamp
   is zero-padded for you.
   =========================================================================== */
const manifest_t kManifest = {
    .app_name = "dRPC5",
.version = "0.5.1",
    .dev_name = "foxinwinter",
    .org_name = "pLabs5",
    .license = "AGPL-3.0-only",
    .title_id = "DRPC00001",
    .content_id = "UP9000-DRPC00001_00-DRPC5AAAAAAAAAAA",
    .repo_url = "https://github.com/pLabs5/dRPC5",
    .client_version = "1.00",
};

void manifest_identity(char *out, size_t cap) {
  if (!out || cap == 0) return;
  snprintf(out, cap, "%s %s", kManifest.app_name, kManifest.version);
}
