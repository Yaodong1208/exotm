#include "../../ds/STMCAS/dlist_carumap.h"
#include "../include/experiment.h"

using descriptor = STMCAS_ALG<STMCAS_OREC>; // defined by Makefile
using map = dlist_carumap<int, int, descriptor>;
using K2VAL = I2I;

#include "../include/launch.h"

STMCAS_GLOBALS_INITIALIZER;
