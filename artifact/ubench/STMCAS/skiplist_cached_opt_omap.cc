#include "../../ds/STMCAS/skiplist_cached_opt_omap.h"
#include "../include/experiment.h"

using descriptor = STMCAS_ALG<STMCAS_OREC>; // defined by Makefile
using map = skiplist_cached_opt_omap<int, int, descriptor, -1, -1>;
using K2VAL = I2I;

#include "../include/launch.h"

STMCAS_GLOBALS_INITIALIZER;
