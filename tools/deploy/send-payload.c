#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_PORT "9021"

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("send-payload: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(1);
}

static void show_help(void) {
  fputs("usage: send-payload [-h HOST] [-p PORT] [-i] PAYLOAD\n"
        "\n"
        "  -h HOST    console address (default $PS5_HOST)\n"
        "  -p PORT    console port (default $PS5_PORT, else " DEFAULT_PORT ")\n"
        "  -i         append stdin to the payload (interactive mode)\n"
        "  -H         show this help\n"
        "\n"
        "PAYLOAD is a file to send, or a string to send verbatim.\n",
        stdout);
}

static int connect_to(const char *host, const char *port) {
  struct addrinfo hints, *res, *ai;
  int fd = -1;
  int rc;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if ((rc = getaddrinfo(host, port, &hints, &res)) != 0) {
    die("cannot resolve %s:%s: %s", host, port, gai_strerror(rc));
  }

  for (ai = res; ai != NULL; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }

  freeaddrinfo(res);
  if (fd < 0) die("cannot connect to %s:%s: %s", host, port, strerror(errno));
  return fd;
}

static void write_all(int fd, const unsigned char *buf, size_t len, const char *what) {
  size_t off = 0;

  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      die("sending %s: %s", what, strerror(errno));
    }
    if (n == 0) die("sending %s: connection closed", what);
    off += (size_t)n;
  }
}

static void send_file(int fd, const char *path) {
  unsigned char buf[65536];
  FILE *f;
  size_t total = 0;

  if ((f = fopen(path, "rb")) == NULL) die("cannot open %s: %s", path, strerror(errno));

  for (;;) {
    size_t n = fread(buf, 1, sizeof buf, f);
    if (n == 0) break;
    write_all(fd, buf, n, path);
    total += n;
  }

  if (ferror(f)) die("error reading %s", path);
  fclose(f);
  fprintf(stderr, "send-payload: sent %zu bytes from %s\n", total, path);
}

static void send_stdin(int fd) {
  unsigned char buf[65536];
  size_t total = 0;
  ssize_t n;

  while ((n = read(STDIN_FILENO, buf, sizeof buf)) > 0) {
    write_all(fd, buf, (size_t)n, "stdin");
    total += (size_t)n;
  }
  if (n < 0 && errno != EINTR) die("reading stdin: %s", strerror(errno));
  fprintf(stderr, "send-payload: appended %zu bytes from stdin\n", total);
}

static void drain(int fd) {
  unsigned char buf[4096];
  ssize_t n;

  for (;;) {
    n = read(fd, buf, sizeof buf);
    if (n > 0) {
      fwrite(buf, 1, (size_t)n, stdout);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  fflush(stdout);
}

int main(int argc, char **argv) {
  const char *host = getenv("PS5_HOST");
  const char *port = getenv("PS5_PORT");
  const char *payload = NULL;
  int interactive = 0;
  int opt;
  int fd;
  struct timeval tv;

  if (port == NULL || *port == 0) port = DEFAULT_PORT;

  opterr = 0;
  while ((opt = getopt(argc, argv, "h:p:iH")) != -1) {
    switch (opt) {
      case 'h':
        host = optarg;
        break;
      case 'p':
        port = optarg;
        break;
      case 'i':
        interactive = 1;
        break;
      case 'H':
        show_help();
        return 0;
      default:
        show_help();
        return 1;
    }
  }

  if (optind < argc) payload = argv[optind];

  if (host == NULL || *host == 0) die("no host given (use -h or set PS5_HOST)");
  if (port == NULL || *port == 0) die("no port given (use -p or set PS5_PORT)");
  if (payload == NULL) die("no payload given");

  signal(SIGPIPE, SIG_IGN);

  fd = connect_to(host, port);

  if (access(payload, R_OK) == 0) {
    send_file(fd, payload);
  } else {
    size_t n = strlen(payload);
    char *line = malloc(n + 2);
    if (line == NULL) die("out of memory");
    memcpy(line, payload, n);
    line[n] = '\n';
    line[n + 1] = 0;
    write_all(fd, (const unsigned char *)line, n + 1, "payload string");
    fprintf(stderr, "send-payload: sent %zu bytes as a string\n", n + 1);
    free(line);
  }

  if (interactive) send_stdin(fd);

  shutdown(fd, SHUT_WR);

  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  drain(fd);

  close(fd);
  return 0;
}