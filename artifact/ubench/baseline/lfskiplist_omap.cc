#include "../../ds/baseline/lfskiplist_omap.h"
#include "../../policies/baseline/thread.h"
#include "../include/experiment.h"

using descriptor = thread_t;
using map = fraser_skiplist<unsigned long, void *, descriptor>;
using K2VAL = I2V;

#include "../include/launch.h"

THREAD_T_GLOBALS_INITIALIZER;
