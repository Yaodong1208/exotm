#include "../../ds/handSTM/skiplist_omap_bigtx.h"
#include "../include/experiment.h"

using descriptor = HANDSTM_ALG<HANDSTM_OREC>; // defined by Makefile
using map = skiplist_omap_bigtx<int, int, descriptor, -1, -1>;
using K2VAL = I2I;

#include "../include/launch.h"

HANDSTM_GLOBALS_INITIALIZER;
