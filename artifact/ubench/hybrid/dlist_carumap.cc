#include "../../ds/hybrid/dlist_carumap.h"
#include "../include/experiment.h"

using descriptor = HYBRID_ALG<HYBRID_OREC>; // defined by Makefile
using map = dlist_carumap<int, int, descriptor>;
using K2VAL = I2I;

#include "../include/launch.h"

HYBRID_GLOBALS_INITIALIZER;
