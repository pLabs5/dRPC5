#pragma once

#include <stddef.h>

/* ---------------------------------------------------------------------------
   Notification transport.

   notifyf() is the low-level primitive: it formats a message into the system
   notification request and submits it. Prefer the named helpers below, which
   pair a call site with the text in src/notify/messages.h, so that all wording
   stays in one file.

   Every helper is safe to call from any thread and is a silent no-op if the
   system call fails. None of them touch the log; use dlogf() for diagnostics.
   --------------------------------------------------------------------------- */

/* Send arbitrary text as a console notification. This is the escape hatch for
   one-off text that does not warrant a named helper. Prefer notifyf(MSG_...)
   so the wording still lives in messages.h. */
void notifyf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* --- welcome (see src/notify/messages.h) --- */

/* 1. The greeting. Reads the app name and version from the manifest, so
   renaming or bumping either is a one-line edit in manifest.c. */
void notify_welcome(void);

/* 2. The listening address. Pass the whole URL including the port, e.g.
   "http://192.168.1.159:8642". Call this once the socket is bound, so the
   address shown is one that actually works. */
void notify_http_server(const char *url);

/* --- tile install (used by src/install.c) --- */

/* The tile was already registered; nothing to do this boot. */
void notify_tile_already(void);

/* The data directory could not be created. Pass errno. */
void notify_mkdir_failed(int err);

/* The embedded package could not be written to disk. Pass errno. */
void notify_pkg_write_failed(int err);

/* The package was staged successfully and is about to be installed. Pass the
   package size in bytes. */
void notify_pkg_written(unsigned bytes);

/* The installer failed to initialise. Pass the error code. */
void notify_install_init_failed(unsigned err);

/* The installer initialised; the install itself is about to be attempted. */
void notify_install_init_ok(void);

/* The tile was installed successfully. */
void notify_tile_installed(void);

/* The preferred install call failed and the fallback is being attempted. Pass
   the error code. */
void notify_install_pkg_failed(unsigned err);

/* Both install calls failed, so no tile was registered. Pass the error code. */
void notify_install_failed(unsigned err);
