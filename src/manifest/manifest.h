#pragma once

/* The manifest layout and manifest_identity() live in the pLabs5 SDK; only the
   kManifest table in manifest.c is dRPC5's. This shim keeps the include path. */
#include <plabs5/manifest.h>
