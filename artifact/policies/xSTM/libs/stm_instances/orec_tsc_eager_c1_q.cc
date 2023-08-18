/// An xSTM algorithm instantiation with the following features:
/// - traditional check-once orecs
/// - rdtscp clock
/// - undo logging
/// - quiescence and irrevocability
/// - exponential backoff for contention management

#include "../stm_algs/orec_eager_c1.h"

#include "include/clone.h"
#include "include/execute.h"
#include "include/frame.h"
#include "include/loadstore.h"
#include "include/mem.h"
#include "include/stats.h"

#include "../include/cm.h"
#include "../include/constants.h"
#include "../include/epochs.h"
#include "../include/orec_t.h"
#include "../include/timesource.h"

typedef OrecEagerC1<OrecTable<NUM_STRIPES, OREC_COVERAGE, RdtscpTimesource>,
                    IrrevocQuiesceEpochManager<MAX_THREADS>,
                    ExpBackoffCM<BACKOFF_MIN, BACKOFF_MAX>>
    TxThread;

template <class O, class E, class C>
typename OrecEagerC1<O, E, C>::Globals OrecEagerC1<O, E, C>::globals;

API_TM_DESCRIPTOR;
API_TM_MALLOC_FREE;
API_TM_MEMFUNCS_GENERIC;
API_TM_LOADFUNCS;
API_TM_STOREFUNCS;
API_TM_STATS_NOP;
API_TM_EXECUTE_NOEXCEPT;
API_TM_CLONES_THREAD_UNSAFE;
API_TM_STACKFRAME_OPT;
