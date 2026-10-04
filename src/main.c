#define _GNU_SOURCE
#include "core/config.h"
#include "core/util.h"
#include "discord.h"
#include "gateway.h"
#include "http/http.h"
#include "install.h"
#include "paths.h"

#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void *
heartbeat(void *arg) {
  (void)arg;
  for(;;) {
    FILE *f = fopen(STORE_DIR "/.hb", "wb");
    if(f) {
      fputc('1', f);
      fclose(f);
    }
    sleep(1);
  }
  return NULL;
}

static void *
net_main(void *arg) {
  (void)arg;
  printf("dRPC5: stage=discord_init\n");
  printf("dRPC5: discord_init=%d\n", discord_init());
  printf("dRPC5: stage=gateway\n");
  printf("dRPC5: gateway_start=%d\n", gateway_start());
  return NULL;
}

int
main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  setvbuf(stdout, NULL, _IONBF, 0);
  mkdir(STORE_DIR, 0777);
  load_prev_pid();
  write_ca();
  write_default_config();
  {
    FILE *m=fopen(STORE_DIR "/.ran", "wb");
    if(m) {
      fputs("ran", m);
      fclose(m);
    }
  }
  notifyf("dRPC5 running - 127.0.0.1:%d", DRPC_PORT);

  {
    pthread_t hbt;
    if(pthread_create(&hbt, NULL, heartbeat, NULL) == 0)
      pthread_detach(hbt);
  }

  printf("dRPC5: stage=install\n");
  install_tile();

  {
    pthread_t nt;
    if (pthread_create(&nt, NULL, net_main, NULL) == 0)
      pthread_detach(nt);
    else
      net_main(NULL);
  }

  printf("dRPC5: stage=httpd\n");
  httpd_run();
  printf("dRPC5: httpd exited\n");
  for(;;) pause();
  return 0;
}