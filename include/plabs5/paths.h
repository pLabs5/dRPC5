#pragma once

/* Points the SDK's bootstrap modules at dRPC5's own path definitions. Reached
   via -Iinclude before the SDK's include/ directory. The relative path avoids
   this file including itself ("paths.h" would match its own directory). */
#include "../../src/paths.h"
