#pragma once

/* ===========================================================================
   ALL on-screen notification text for dRPC5 lives in this file.
   ---------------------------------------------------------------------------
   This is the only file in the project that holds wording shown to the user on
   the console. Reword, re-prefix, shorten or silence any message by editing the
   define below and rebuilding; no .c file contains notification text.

   Rules for editing:
     - These are printf format strings. The arguments are supplied by
       src/notify/notify.c, so keep the placeholders that are present. You may
       add placeholders only if you also add the matching argument in notify.c.
         %d  signed int      errno-style codes, or a port number
         %u  unsigned int    byte counts
         %x  unsigned int    hex error codes from the system libraries
     - Text is truncated to the system limit by the notification transport, not
       here. Keep it to roughly one line; the console shows a short toast.
     - To silence a message without deleting it, call the named helper less
       often, or make the helper in notify.c a no-op. Do not edit the callers.
     - Log lines are NOT here. Those go to drpc5.log via dlogf() in
       src/core/util.c and are a separate concern.
   =========================================================================== */

/* Shown at the front of most messages. Change this once to re-brand them all. */
#define MSG_PREFIX "dRPC5"

/* ===========================================================================
   WELCOME MESSAGES

   The two things you see when dRPC5 starts, in this order:

     1. MSG_WELCOME_HEADING   - the greeting, from src/main.c
     2. MSG_HTTP_SERVER       - the address it is listening on, from
                                src/http/server.c once the socket is bound

   THE APP NAME AND VERSION ARE NOT PLACEHOLDERS. Do not type %s for them.
   notify.c builds "<app name> <version>" from src/manifest.c and substitutes
   that as the single %s, so:

       Welcome to dRPC5 1.00
                  ^^^^^^^^^ supplied for you

   To change the name or the number, edit manifest.c. To change the sentence,
   edit the string below. Neither edit disturbs the other.

   Each string below contains AT MOST ONE placeholder, and that placeholder is
   named in its own comment. There is no counting to do.

   Want fewer toasts? Comment out a line here and rebuild - see "To silence a
   message" in the rules at the top of this file.
   =========================================================================== */

/* 1. The greeting. The %s is replaced with the app name and version together,
   e.g. "dRPC5 1.00". Nothing else in this string is substituted. */
#define MSG_WELCOME_HEADING "Welcome to %s"

/* 2. The listening address, sent once the socket is actually bound so the
   address shown is one that works.
   ONE placeholder: %s, replaced with the whole URL including the port, for
   example http://192.168.1.159:8642. The IP falls back to 127.0.0.1 when no LAN
   route can be found. */
#define MSG_HTTP_SERVER "Starting HTTP server at %s"

/* --- tile install (src/install.c) ------------------------------------------
   The tile is the on-screen tile that launches the payload. */

/* The tile is already registered, so nothing was installed this boot. */
#define MSG_TILE_ALREADY MSG_PREFIX ": tile already installed"

/* The data directory could not be created. Placeholder: errno. */
#define MSG_MKDIR_FAILED MSG_PREFIX ": cannot create " "/data/pLabs5/dRPC5" " (errno %d)"

/* The embedded package could not be written to disk. Placeholder: errno. */
#define MSG_PKG_WRITE_FAILED MSG_PREFIX ": cannot write " "DRPC00001_00-DRPC5AAAAAAAAAAA.pkg" " (errno %d)"

/* The package was staged and is about to be handed to the installer.
   Placeholder: size of the package in bytes. */
#define MSG_PKG_WRITTEN MSG_PREFIX ": installing package (%u bytes)"

/* The installer failed to initialise. Placeholder: error code, hex. */
#define MSG_INSTALL_INIT_FAILED MSG_PREFIX ": installer init failed (0x%x)"

/* The installer initialised; the real install is about to be attempted. */
#define MSG_INSTALL_INIT_OK MSG_PREFIX ": installer ready, installing..."

/* The tile was installed successfully. */
#define MSG_TILE_INSTALLED MSG_PREFIX ": tile installed"

/* First of the two install calls failed; the fallback is now being tried.
   Placeholder: error code, hex. */
#define MSG_INSTALL_PKG_FAILED MSG_PREFIX ": fast install failed (0x%x), retrying"

/* Both install calls failed, so no tile was registered. Placeholder: error
   code, hex. */
#define MSG_INSTALL_FAILED MSG_PREFIX ": tile install failed (0x%x)"
