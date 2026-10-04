#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("error: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

static unsigned char *slurp(const char *path, size_t *len) {
  FILE *f;
  long n;
  unsigned char *buf;

  if ((f = fopen(path, "rb")) == NULL) die("%s not found", path);
  if (fseek(f, 0, SEEK_END) != 0) die("seek %s", path);
  if ((n = ftell(f)) < 0) die("tell %s", path);
  rewind(f);

  if ((buf = malloc((size_t)n + 1)) == NULL) die("out of memory");
  if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) die("short read on %s", path);
  buf[n] = 0;
  fclose(f);

  *len = (size_t)n;
  return buf;
}

static void mkdirs(const char *path) {
  char tmp[4096];
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

static void ensure_parent(const char *path) {
  const char *slash = strrchr(path, '/');
  char dir[4096];

  if (slash == NULL) return;
  if ((size_t)(slash - path) >= sizeof dir) die("path too long: %s", path);
  memcpy(dir, path, (size_t)(slash - path));
  dir[slash - path] = 0;
  if (dir[0] != 0) mkdirs(dir);
}

static void split(char *spec, char **sym, char **input, char **size_sym, char **raw_sym) {
  char *colon;

  *sym = spec;
  if ((colon = strchr(spec, '=')) == NULL) die("bad spec %s, want SYMBOL=INPUT[:SIZE[:RAW]]", spec);
  *colon = 0;
  *input = colon + 1;

  if ((colon = strchr(*input, ':')) != NULL) {
    *colon = 0;
    *size_sym = colon + 1;
    if ((colon = strchr(*size_sym, ':')) != NULL) {
      *colon = 0;
      *raw_sym = colon + 1;
    } else {
      *raw_sym = NULL;
    }
  } else {
    *size_sym = NULL;
    *raw_sym = NULL;
  }

  if (**sym == 0) die("empty symbol in spec %s", spec);
  if (**input == 0) die("empty input in spec %s", spec);
}

static void emit(FILE *out, const char *sym, const unsigned char *data, size_t len, int hex) {
  const int per_line = hex ? 16 : 20;
  const char *sep = hex ? ", " : ",";
  size_t i;

  fprintf(out, "const unsigned char %s[] = {\n", sym);
  for (i = 0; i < len; i++) {
    if (i % (size_t)per_line == 0) fputs("  ", out);
    if (hex) {
      fprintf(out, "0x%02x", data[i]);
    } else {
      fprintf(out, "%u", (unsigned)data[i]);
    }
    if (i + 1 == len) {
      fputs(",\n", out);
    } else if ((i + 1) % (size_t)per_line == 0) {
      fputs(",\n", out);
    } else {
      fputs(sep, out);
    }
  }
  if (len == 0) fputs("\n", out);
  fputs("};\n", out);
}

static void emit_size(FILE *out, const char *sym, size_t len, int hex) {
  if (hex) {
    fprintf(out, "const unsigned int %s = %zu;\n\n", sym, len);
  } else {
    fprintf(out, "const unsigned int %s = %zuu;\n", sym, len);
  }
}

int main(int argc, char **argv) {
  const char *out_path = NULL;
  const char *include = NULL;
  int hex = 0;
  int check_nul = 0;
  int i;
  int specs = 0;
  FILE *out;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      out_path = argv[++i];
    } else if (strcmp(argv[i], "--include") == 0 && i + 1 < argc) {
      include = argv[++i];
    } else if (strcmp(argv[i], "--check-nul") == 0) {
      check_nul = 1;
    } else if (strcmp(argv[i], "--hex") == 0) {
      hex = 1;
    } else if (strcmp(argv[i], "--dec") == 0) {
      hex = 0;
    } else if (strcmp(argv[i], "--help") == 0) {
      fputs("usage: embed --out FILE [--dec|--hex] [--include NAME] [--check-nul]"
            " SYMBOL=INPUT[:SIZE[:RAW]]...\n", stdout);
      return 0;
    } else if (argv[i][0] == '-') {
      die("unknown option %s", argv[i]);
    } else {
      specs++;
    }
  }

  if (out_path == NULL) die("--out is required");
  if (specs == 0) die("no SYMBOL=INPUT specs given");

  ensure_parent(out_path);
  if ((out = fopen(out_path, "wb")) == NULL) die("cannot write %s: %s", out_path, strerror(errno));

  if (include != NULL) fprintf(out, "#include <%s>\n\n", include);

  for (i = 1; i < argc; i++) {
    char *sym, *input, *size_sym, *raw_sym;
    unsigned char *data;
    size_t len;
    size_t j;

    if (argv[i][0] == '-') {
      if (strcmp(argv[i], "--out") == 0 || strcmp(argv[i], "--include") == 0) i++;
      continue;
    }

    split(argv[i], &sym, &input, &size_sym, &raw_sym);
    data = slurp(input, &len);

    for (j = 0; check_nul && j < len; j++) {
      if (data[j] == 0) die("%s contains NUL bytes (symbol %s)", input, sym);
    }

    emit(out, sym, data, len, hex);
    if (size_sym != NULL) emit_size(out, size_sym, len, hex);
    if (raw_sym != NULL && !hex) emit_size(out, raw_sym, len, hex);

    free(data);
  }

  if (fclose(out) != 0) die("write failed on %s", out_path);
  return 0;
}