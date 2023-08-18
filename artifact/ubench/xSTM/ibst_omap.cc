#include "../../ds/xSTM/ibst_omap.h"
#include "../../policies/baseline/thread.h"
#include "../include/experiment.h"

using descriptor = thread_t;
using map = ibst_omap<int, int, descriptor>;
using K2VAL = I2I;

#include "../include/launch.h"

THREAD_T_GLOBALS_INITIALIZER;
