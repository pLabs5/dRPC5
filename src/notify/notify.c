#define _GNU_SOURCE
#include "notify/notify.h"

#include "notify/messages.h"
#include "manifest/manifest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Opaque system notification request. The leading bytes are a fixed-size header
   the system expects before the message; only the message field is ours. The
   layout is ABI, so neither field may be resized. */
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

void notifyf(const char *fmt, ...) {
  notify_request_t req;
  va_list ap;

  memset(&req, 0, sizeof req);
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* The name and version are supplied here rather than left as placeholders in
   messages.h, so the greeting can be reworded without anyone having to keep a
   %s count in step. Reads both from src/manifest.c. */
void notify_welcome(void) {
  char id[64];
  manifest_identity(id, sizeof(id));
  notifyf(MSG_WELCOME_HEADING, id);
}

void notify_http_server(const char *url) {
  if (!url || !url[0]) return;
  notifyf(MSG_HTTP_SERVER, url);
}

void notify_tile_already(void) {
  notifyf(MSG_TILE_ALREADY);
}

void notify_mkdir_failed(int err) {
  notifyf(MSG_MKDIR_FAILED, err);
}

void notify_pkg_write_failed(int err) {
  notifyf(MSG_PKG_WRITE_FAILED, err);
}

void notify_pkg_written(unsigned bytes) {
  notifyf(MSG_PKG_WRITTEN, bytes);
}

void notify_install_init_failed(unsigned err) {
  notifyf(MSG_INSTALL_INIT_FAILED, err);
}

void notify_install_init_ok(void) {
  notifyf(MSG_INSTALL_INIT_OK);
}

void notify_tile_installed(void) {
  notifyf(MSG_TILE_INSTALLED);
}

void notify_install_pkg_failed(unsigned err) {
  notifyf(MSG_INSTALL_PKG_FAILED, err);
}

void notify_install_failed(unsigned err) {
  notifyf(MSG_INSTALL_FAILED, err);
}
