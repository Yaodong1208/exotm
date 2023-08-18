#include "../../ds/STMCAS/slist_omap.h"
#include "../../ds/include/ca_umap_list_adapter.h"
#include "../include/experiment.h"

using descriptor = STMCAS_ALG<STMCAS_OREC>; // defined by Makefile
using map = ca_umap_list_adapter_t<int, int, descriptor,
                                   slist_omap<int, int, descriptor, true>>;
using K2VAL = I2I;

#include "../include/launch.h"

STMCAS_GLOBALS_INITIALIZER;