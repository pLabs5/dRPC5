#define _GNU_SOURCE
#include "install/install.h"

#include "generated/bundled_tile_pkg.h"
#include "core/util.h"
#include "manifest/manifest.h"
#include "notify/notify.h"
#include "paths.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallPkg(const char*, void*);
int sceAppInstUtilInstallByPackage(void*, void*, void*);

struct pkg_info_abi {
  char content_id[0x30];
  int content_type;
  int content_platform;
};

struct pkg_meta_abi {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
};

struct pkg_playgo_abi {
  char languages[30][8];
  char scenario_ids[64][3];
  char content_ids[64][0x30];
  long unknown[810];
};

int write_pkg(void) {
  FILE *f;
  size_t written;

  if((f=fopen(PKG_PATH, "wb"))==NULL) return -1;
  written=fwrite(kTilePkg, 1, kTilePkgSize, f);
  fclose(f);
  return written==kTilePkgSize ? 0 : -1;
}

void install_tile(void) {
  struct stat st;
  struct pkg_info_abi info;
  struct pkg_info_abi info2;
  struct pkg_meta_abi meta;
  struct pkg_playgo_abi playgo;
  char uri[96];
  int err;

  if(stat(APPMETA_PATH, &st)==0) {
    /* Log only. As a toast this fired on every single launch and said nothing
       the user needed to act on. If you want it back on screen, call
       notify_tile_already() here instead. */
    dlogf("drpc5: tile already installed\n");
    return;
  }

  if(mkdir(STORE_DIR, 0777)!=0 && errno!=EEXIST) {
    dlogf("drpc5: mkdir " STORE_DIR " errno=%d\n", errno);
    notify_mkdir_failed(errno);
    return;
  }
  if(write_pkg()) {
    dlogf("drpc5: could not write " PKG_PATH " errno=%d\n", errno);
    notify_pkg_write_failed(errno);
    return;
  }
  dlogf("dRPC5: pkg written (%u bytes)\n", kTilePkgSize);
  notify_pkg_written(kTilePkgSize);

  if((err=sceAppInstUtilInitialize())) {
    dlogf("sceAppInstUtilInitialize: %x\n", err);
    notify_install_init_failed((unsigned)err);
    return;
  }
  notify_install_init_ok();

  memset(&info, 0, sizeof(info));
  if((err=sceAppInstUtilAppInstallPkg(PKG_VISIBLE, &info))==0) {
    dlogf("drpc5: tile installed\n");
    notify_tile_installed();
    return;
  }
  dlogf("sceAppInstUtilAppInstallPkg: %x\n", err);
  notify_install_pkg_failed((unsigned)err);

  memset(&info2, 0, sizeof(info2));
  memset(&meta, 0, sizeof(meta));
  memset(&playgo, 0, sizeof(playgo));
  snprintf(uri, sizeof(uri), "file://%s", PKG_VISIBLE);
  meta.uri=uri;
  meta.content_name=kManifest.app_name;

  if((err=sceAppInstUtilInstallByPackage(&meta, &info2, &playgo))==0) {
    dlogf("drpc5: tile installed\n");
    notify_tile_installed();
    return;
  }
  dlogf("sceAppInstUtilInstallByPackage: %x\n", err);
  notify_install_failed((unsigned)err);
}