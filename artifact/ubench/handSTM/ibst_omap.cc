#include "../../ds/handSTM/ibst_omap.h"
#include "../include/experiment.h"

using descriptor = HANDSTM_ALG<HANDSTM_OREC>; // defined by Makefile
using map = ibst_omap<int, int, descriptor>;
using K2VAL = I2I;

#include "../include/launch.h"

HANDSTM_GLOBALS_INITIALIZER;
