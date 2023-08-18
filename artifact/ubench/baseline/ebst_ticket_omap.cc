#include "../../ds/baseline/ext_ticket_bst/ticket_impl.h"
#include "../../policies/baseline/thread.h"
#include "../include/experiment.h"

using descriptor = thread_t;
using map = ticket<int, int, descriptor, INT_MIN, INT_MAX, -1>;
using K2VAL = I2I;

#include "../include/launch.h"

THREAD_T_GLOBALS_INITIALIZER;
