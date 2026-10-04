#define _GNU_SOURCE
#include "install.h"

#include "bundled_tile_pkg.h"
#include "core/util.h"
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
    notifyf("dRPC5: tile already installed");
    return;
  }

  if(mkdir(STORE_DIR, 0777)!=0 && errno!=EEXIST) {
    printf("drpc5: mkdir " STORE_DIR " errno=%d\n", errno);
    notifyf("dRPC5: mkdir failed (%d)", errno);
    return;
  }
  if(write_pkg()) {
    printf("drpc5: could not write " PKG_PATH " errno=%d\n", errno);
    notifyf("dRPC5: pkg write failed (%d)", errno);
    return;
  }
  printf("dRPC5: pkg written (%u bytes)\n", kTilePkgSize);
  notifyf("dRPC5: pkg written (%u bytes)", kTilePkgSize);

  if((err=sceAppInstUtilInitialize())) {
    printf("sceAppInstUtilInitialize: %x\n", err);
    notifyf("dRPC5: install init 0x%x", err);
    return;
  }
  notifyf("dRPC5: install init ok");

  memset(&info, 0, sizeof(info));
  if((err=sceAppInstUtilAppInstallPkg(PKG_VISIBLE, &info))==0) {
    printf("drpc5: tile installed\n");
    notifyf("dRPC5: tile installed");
    return;
  }
  printf("sceAppInstUtilAppInstallPkg: %x\n", err);
  notifyf("dRPC5: AppInstallPkg 0x%x", err);

  memset(&info2, 0, sizeof(info2));
  memset(&meta, 0, sizeof(meta));
  memset(&playgo, 0, sizeof(playgo));
  snprintf(uri, sizeof(uri), "file://%s", PKG_VISIBLE);
  meta.uri=uri;
  meta.content_name="dRPC5";

  if((err=sceAppInstUtilInstallByPackage(&meta, &info2, &playgo))==0) {
    printf("drpc5: tile installed\n");
    notifyf("dRPC5: tile installed");
    return;
  }
  printf("sceAppInstUtilInstallByPackage: %x\n", err);
  notifyf("dRPC5: InstallByPackage 0x%x", err);
}