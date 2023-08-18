#include "../../ds/handSTM/dlist_carumap.h"
#include "../include/experiment.h"

using descriptor = HANDSTM_ALG<HANDSTM_OREC>; // defined by Makefile
using map = dlist_carumap<int, int, descriptor>;
using K2VAL = I2I;

#include "../include/launch.h"

HANDSTM_GLOBALS_INITIALIZER;
