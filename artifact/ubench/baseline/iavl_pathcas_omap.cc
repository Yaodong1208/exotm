#include "../../ds/baseline/int_bst_pathcas/internal_kcas_avl.h"
#include "../../policies/baseline/thread.h"
#include "../include/experiment_pathcas.h"

using descriptor = thread_t;
const int numThreads = 96;
const int minKey = 0;
const long long maxKey = INT_MAX;
using map = InternalKCAS<descriptor, int, int, numThreads, minKey, maxKey>;
using K2VAL = I2I;

#include "../include/launch.h"

THREAD_T_GLOBALS_INITIALIZER;
