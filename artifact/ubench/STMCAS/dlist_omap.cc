#include "../../ds/STMCAS/dlist_omap.h"
#include "../include/experiment.h"

using descriptor = STMCAS_ALG<STMCAS_OREC>; // defined by Makefile
using map = dlist_omap<int, int, descriptor, false>;
using K2VAL = I2I;

#include "../include/launch.h"

STMCAS_GLOBALS_INITIALIZER;
