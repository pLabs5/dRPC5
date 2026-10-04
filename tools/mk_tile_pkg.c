#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LPP_INNER_NONE 0
#define LPP_INNER_NWONLY_DATA_FIRST 3
#define EXPECTED_ABI_VERSION 7

typedef struct lpp_launch_readiness {
  int32_t struct_size;
  int32_t has_eboot;
  int32_t has_param_json;
  int32_t has_param_sfo;
  int32_t requires_debug_console;
  int32_t is_launch_ready;
  int32_t module_count;
  int32_t issue_count;
} lpp_launch_readiness;

typedef const char *(*fn_version)(void);
typedef int (*fn_abi_version)(void);
typedef int (*fn_keys_available)(void);
typedef int (*fn_last_error)(char *, int);
typedef int (*fn_is_valid_content_id)(const char *);
typedef int (*fn_is_valid_title_id)(const char *);
typedef int (*fn_package_homebrew)(const char *, const char *, const char *, const char *, const char *,
                                   const char *, const char *, int, lpp_launch_readiness *, char *, int);
typedef int (*fn_launch_readiness_issues)(const char *, char *, int);

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("error: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

static void mkdirs(const char *path) {
  char tmp[PATH_MAX];
  char *p;
  size_t len;

  if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) die("path too long: %s", path);

  len = strlen(tmp);
  while (len > 1 && tmp[len - 1] == '/') tmp[--len] = 0;

  for (p = tmp + 1; *p != 0; p++) {
    if (*p != '/') continue;
    *p = 0;
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) die("mkdir %s: %s", tmp, strerror(errno));
    *p = '/';
  }
  if (mkdir(tmp, 0777) != 0 && errno != EEXIST) die("mkdir %s: %s", tmp, strerror(errno));
}

static char *dupstr(const char *s) {
  char *r = strdup(s);
  if (r == NULL) die("out of memory");
  return r;
}

static char *dirname_of(const char *path) {
  const char *slash = strrchr(path, '/');
  char *dir;
  size_t n;

  if (slash == NULL) return dupstr(".");
  n = (size_t)(slash - path);
  if (n == 0) n = 1;
  if ((dir = malloc(n + 1)) == NULL) die("out of memory");
  memcpy(dir, path, n);
  dir[n] = 0;
  return dir;
}

static char *resolve_existing(const char *path) {
  char buf[PATH_MAX];

  if (realpath(path, buf) == NULL) die("cannot resolve %s: %s", path, strerror(errno));
  return dupstr(buf);
}

static void copy_file(const char *from, const char *to) {
  FILE *in, *out;
  char buf[65536];
  size_t n;

  if ((in = fopen(from, "rb")) == NULL) die("cannot read %s: %s", from, strerror(errno));
  if ((out = fopen(to, "wb")) == NULL) die("cannot write %s: %s", to, strerror(errno));

  while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) die("short write on %s", to);
  }
  if (ferror(in)) die("read error on %s", from);
  fclose(in);
  if (fclose(out) != 0) die("write failed on %s", to);
}

static void *must_sym(void *handle, const char *name) {
  void *p;
  dlerror();
  p = dlsym(handle, name);
  if (p == NULL) die("symbol %s not found in library", name);
  return p;
}

int main(int argc, char **argv) {
  const char *lib_path = getenv("LPP_LIB");
  const char *homebrew_arg = NULL;
  const char *out_pkg_arg = NULL;
  const char *content_id = NULL;
  const char *title = "";
  const char *version = "";
  const char *passcode = "";
  const char *module = "";
  int inner_compression = LPP_INNER_NWONLY_DATA_FIRST;
  int i;

  void *handle;
  fn_version p_version;
  fn_abi_version p_abi_version;
  fn_keys_available p_keys_available;
  fn_last_error p_last_error;
  fn_is_valid_content_id p_is_valid_content_id;
  fn_is_valid_title_id p_is_valid_title_id;
  fn_package_homebrew p_package_homebrew;
  fn_launch_readiness_issues p_readiness_issues;

  char title_id[16];
  char *homebrew, *out_pkg, *out_dir;
  char produced[4096];
  char issues[4096];
  char errbuf[1024];
  lpp_launch_readiness readiness;
  int rc, abi;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
      lib_path = argv[++i];
    } else if (strcmp(argv[i], "--homebrew") == 0 && i + 1 < argc) {
      homebrew_arg = argv[++i];
    } else if (strcmp(argv[i], "--out-pkg") == 0 && i + 1 < argc) {
      out_pkg_arg = argv[++i];
    } else if (strcmp(argv[i], "--content-id") == 0 && i + 1 < argc) {
      content_id = argv[++i];
    } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
      title = argv[++i];
    } else if (strcmp(argv[i], "--version") == 0 && i + 1 < argc) {
      version = argv[++i];
    } else if (strcmp(argv[i], "--passcode") == 0 && i + 1 < argc) {
      passcode = argv[++i];
    } else if (strcmp(argv[i], "--module") == 0 && i + 1 < argc) {
      module = argv[++i];
    } else if (strcmp(argv[i], "--inner-compression") == 0 && i + 1 < argc) {
      inner_compression = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--help") == 0) {
      fputs("usage: mk_tile_pkg --homebrew DIR --out-pkg FILE --content-id ID"
            " [--lib SO] [--title T] [--version V] [--passcode P] [--module M]"
            " [--inner-compression N]\n", stdout);
      return 0;
    } else {
      die("unknown argument %s", argv[i]);
    }
  }

  if (homebrew_arg == NULL) die("--homebrew is required");
  if (out_pkg_arg == NULL) die("--out-pkg is required");
  if (content_id == NULL) die("--content-id is required");

  if (lib_path == NULL || *lib_path == 0) lib_path = "tools/lib/libprosperopkg.so";
  if (access(lib_path, R_OK) != 0) die("%s not found (set --lib or LPP_LIB)", lib_path);

  handle = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) die("dlopen %s: %s", lib_path, dlerror());

  p_version = (fn_version)must_sym(handle, "lpp_version");
  p_abi_version = (fn_abi_version)must_sym(handle, "lpp_abi_version");
  p_keys_available = (fn_keys_available)must_sym(handle, "lpp_keys_available");
  p_last_error = (fn_last_error)must_sym(handle, "lpp_last_error");
  p_is_valid_content_id = (fn_is_valid_content_id)must_sym(handle, "lpp_is_valid_content_id");
  p_is_valid_title_id = (fn_is_valid_title_id)must_sym(handle, "lpp_is_valid_title_id");
  p_package_homebrew = (fn_package_homebrew)must_sym(handle, "lpp_package_homebrew");
  p_readiness_issues = (fn_launch_readiness_issues)must_sym(handle, "lpp_launch_readiness_issues");

  abi = p_abi_version();
  printf("libprosperopkg %s abi %d keys %d\n", p_version(), abi, p_keys_available());
  if (abi != EXPECTED_ABI_VERSION) {
    printf("warning: expected abi %d, library reports %d\n", EXPECTED_ABI_VERSION, abi);
  }

  if (!p_is_valid_content_id(content_id)) die("invalid content id %s", content_id);

  if (strlen(content_id) < 16) die("content id too short: %s", content_id);
  memcpy(title_id, content_id + 7, 9);
  title_id[9] = 0;
  if (!p_is_valid_title_id(title_id)) {
    printf("note: %s is not a PPSA id, param.json titleId must carry it\n", title_id);
  }

  homebrew = resolve_existing(homebrew_arg);

  out_dir = dirname_of(out_pkg_arg);
  mkdirs(out_dir);

  {
    char *abs_dir = resolve_existing(out_dir);
    const char *base = strrchr(out_pkg_arg, '/');
    size_t need;

    base = (base != NULL) ? base + 1 : out_pkg_arg;
    if (*base == 0) die("--out-pkg has no filename component");
    need = strlen(abs_dir) + 1 + strlen(base) + 1;
    if ((out_pkg = malloc(need)) == NULL) die("out of memory");
    snprintf(out_pkg, need, "%s/%s", abs_dir, base);
    free(abs_dir);
  }

  memset(&readiness, 0, sizeof readiness);
  readiness.struct_size = (int32_t)sizeof readiness;
  produced[0] = 0;

  rc = p_package_homebrew(homebrew, out_dir, content_id, passcode, title, version, module,
                          inner_compression, &readiness, produced, (int)sizeof produced);

  if (rc != 0) {
    errbuf[0] = 0;
    p_last_error(errbuf, (int)sizeof errbuf);
    die("lpp_package_homebrew failed: %s", errbuf);
  }

  if (produced[0] == 0) die("lpp_package_homebrew reported success but produced no path");

  if (strcmp(produced, out_pkg) != 0) {
    copy_file(produced, out_pkg);
    if (unlink(produced) != 0) printf("warning: could not remove %s: %s\n", produced, strerror(errno));
  }

  issues[0] = 0;
  p_readiness_issues(homebrew, issues, (int)sizeof issues);

  printf("eboot=%d param=%d sfo=%d modules=%d issues=%d debug_only=%d\n", readiness.has_eboot,
         readiness.has_param_json, readiness.has_param_sfo, readiness.module_count, readiness.issue_count,
         readiness.requires_debug_console);
  if (strspn(issues, " \t\r\n") != strlen(issues)) fputs(issues, stdout);
  printf("wrote %s\n", out_pkg);

  free(homebrew);
  free(out_pkg);
  free(out_dir);
  dlclose(handle);
  return 0;
}