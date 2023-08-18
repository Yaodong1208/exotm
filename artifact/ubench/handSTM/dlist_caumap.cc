#include "../../ds/handSTM/dlist_omap.h"
#include "../../ds/include/ca_umap_list_adapter.h"
#include "../include/experiment.h"

using descriptor = HANDSTM_ALG<HANDSTM_OREC>; // defined by Makefile
using map = ca_umap_list_adapter_t<int, int, descriptor,
                                   dlist_omap<int, int, descriptor>>;
using K2VAL = I2I;

#include "../include/launch.h"

HANDSTM_GLOBALS_INITIALIZER;
