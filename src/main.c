#define _GNU_SOURCE
#include "core/config.h"
#include "core/util.h"
#include "discord/discord.h"
#include "gw/gateway.h"
#include "http/http.h"
#include "install/install.h"
#include "notify/notify.h"
#include "paths.h"

#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static void *
net_main(void *arg) {
  (void)arg;
  dlogf("dRPC5: stage=discord_init\n");
  dlogf("dRPC5: discord_init=%d\n", discord_init());
  dlogf("dRPC5: stage=gateway\n");
  dlogf("dRPC5: gateway_start=%d\n", gateway_start());
  return NULL;
}

int
main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  setvbuf(stdout, NULL, _IONBF, 0);
  /* Open the log before anything that can fail, so a broken install still
     leaves a trace on disk rather than only in an unreachable stdout. */
  init_log();
  mkdir(STORE_DIR, 0777);
  dlogf("dRPC5: starting, pid %d", (int)getpid());
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
  /* Greeting first: the tile install below can emit several toasts of its own,
     and a welcome that arrives after them is not a welcome. */
  notify_welcome();

  dlogf("dRPC5: stage=install\n");
  install_tile();

  {
    pthread_t nt;
    if (pthread_create(&nt, NULL, net_main, NULL) == 0)
      pthread_detach(nt);
    else
      net_main(NULL);
  }

  dlogf("dRPC5: stage=httpd\n");
  httpd_run();
  dlogf("dRPC5: httpd exited\n");
  for(;;) pause();
  return 0;
}