#include "../../ds/baseline/lazylist_omap.h"
#include "../../policies/baseline/thread.h"
#include "../include/experiment.h"

using descriptor = thread_t;
using map = lazylist_omap<int, int, descriptor, -1, -1>;
using K2VAL = I2I;

#include "../include/launch.h"

THREAD_T_GLOBALS_INITIALIZER;
