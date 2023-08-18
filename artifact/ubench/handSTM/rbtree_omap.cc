#include "../../ds/handSTM/rbtree_omap.h"
#include "../include/experiment.h"

using descriptor = HANDSTM_ALG<HANDSTM_OREC>; // defined by Makefile
using map = rbtree_omap<int, int, descriptor, -1, -1>;
using K2VAL = I2I;

#include "../include/launch.h"

HANDSTM_GLOBALS_INITIALIZER;
