/// An xSTM algorithm instantiation with the following features:
/// - exotm_ps check-twice orecs
/// - rdtscp clock
/// - redo logging
/// - quiescence and irrevocability
/// - exponential backoff for contention management

#include "../stm_algs/exo_lazy_c2.h"

#include "include/clone.h"
#include "include/execute.h"
#include "include/frame.h"
#include "include/loadstore.h"
#include "include/mem.h"
#include "include/stats.h"

#include "../include/cm.h"
#include "../include/constants.h"
#include "../include/epochs.h"

typedef ExoLazyC2<true, ExpBackoffCM<BACKOFF_MIN, BACKOFF_MAX>> TxThread;

template <bool Q, class C>
typename ExoLazyC2<Q, C>::Globals ExoLazyC2<Q, C>::globals;

API_TM_DESCRIPTOR;
API_TM_MALLOC_FREE;
API_TM_MEMFUNCS_GENERIC;
API_TM_LOADFUNCS;
API_TM_STOREFUNCS;
API_TM_STATS_NOP;
API_TM_EXECUTE_NOEXCEPT;
API_TM_CLONES_THREAD_UNSAFE;
API_TM_STACKFRAME_OPT;
