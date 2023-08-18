#include "../../ds/hybrid/rbtree_omap_drop.h"
#include "../include/experiment.h"

using descriptor = HYBRID_ALG<HYBRID_OREC>; // defined by Makefile
using map = rbtree_omap_drop<int, int, descriptor, -1, -1>;
using K2VAL = I2I;

#include "../include/launch.h"

HYBRID_GLOBALS_INITIALIZER;
