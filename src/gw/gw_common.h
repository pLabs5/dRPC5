#pragma once

#define _GNU_SOURCE
#include "gw/gw_internal.h"

#include "core/config.h"
#include "core/dns.h"
#include "core/json.h"
#include "core/util.h"
#include "manifest/manifest.h"
#include "paths.h"
#include "presence/presence.h"
#include "psn/psn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
