/// An xSTM algorithm built from exotm, with undo logging and check-once orecs

#pragma once

#include <setjmp.h>

#include "../../../exoTM/exotm.h"
#include "../../../include/minivector.h"
#include "../../../include/undolog.h"
#include "../include/constants.h"
#include "../include/epochs.h"
#include "include/alloc.h"
#include "include/deferred.h"
#include "include/stackframe.h"

/// ExoEagerC1 is an STM algorithm that is compatible with our LLVM STM plugin.
/// It has the following features:
/// - Uses ExoTM for orecs and rdtsc clock
/// - Check-once orecs
/// - Encounter-time locking with undo
///
/// @param QUIESCE true for quiescence, false if transactions don't quiesce
/// @param CM      a contention manager, invoked only at begin/commit/abort
template <bool QUIESCE, class CM> class ExoEagerC1 {
  /// The type of the Epoch table
  ///
  /// NB: There's a very close interaction with the Epoch table.  Since we want
  ///     to share it among the xSTM::exo algorithms, we need a `friend`.
  using Epoch = CCSTMEpochManager<ExoEagerC1, exotm_t, QUIESCE>;
  friend Epoch;

  /// All of the global variables used by this STM algorithm
  struct Globals {
    static const int NUM_ORECS = 1048576; // The number of orecs to use
    exotm_t::orec_t orecs[NUM_ORECS];     // The table of orecs
    typename CM::Globals cm;              // Global Contention Management info
    typename Epoch::Globals epoch;        // Quiescence and Irrevocability
  };

  static Globals globals;           // All metadata shared among threads
  jmp_buf *checkpoint = nullptr;    // Register checkpoint, for aborts
  exotm_t exo;                      // Per-thread ExoTM metadata
  Epoch epoch;                      // Quiescence and Irrevocability
  CM cm;                            // Contention manager
  OptimizedStackFrameManager frame; // For tracking the transaction's stack
  minivector<int> readset;          // Orecs to validate
  undolog_t undolog;                // An undo log, for undoing writes on abort
  BasicAllocationManager allocator; // Manage malloc/free/aligned alloc
  DeferredActionHandler defers;     // Functions to run after commit/abort

public:
  /// Return the irrevocability state of the thread
  bool isIrrevoc() { return epoch.isIrrevoc(); }

  /// Set the current bottom of the transactional part of the stack
  void adjustStackBottom(void *addr) { frame.setBottom(addr); }

  /// construct a thread's transaction context
  ExoEagerC1() : epoch(this, globals.epoch), cm() {}

  /// Instrumentation to run at the beginning of a transaction
  void beginTx(jmp_buf *b) {
    // onBegin == false -> flat nesting
    if (frame.onBegin()) {
      // Save the checkpoint and set the stack bottom
      checkpoint = b;
      frame.setBottom(b);

      // Start logging allocations
      allocator.onBegin();

      // Get the start time, and put it into the epoch.  Then make sure there
      // isn't an irrevocable transaction.
      while (true) {
        exo.wo_begin();
        if (!globals.epoch.token.val)
          break;
        exo.ro_end(); // Just enough to exit the epoch, nothing more :)
        while (globals.epoch.token.val)
          ;
      }

      // Notify CM of intention to start.  If return true, become irrevocable
      if (cm.beforeBegin(globals.cm))
        becomeIrrevocable();
    }
  }

  /// Instrumentation to run at the end of a transaction
  void commitTx() {
    // onEnd == false -> flat nesting
    if (frame.onEnd()) {
      if (epoch.isIrrevoc()) {
        // NB: we stay in the epoch until the transaction is done
        epoch.onCommitIrrevoc(globals.epoch);
        exo.wo_end(); // because there may be locks to release
        // Do the remaining clean-up
        // NB: We reset most lists when becoming irrevocable
        cm.afterCommit(globals.cm);
        defers.onCommit();
        frame.onCommit();
        return;
      }

      // fast-path for read-only transactions must still quiesce before freeing
      if (undolog.size() == 0) {
        // To quiesce, we need to wait for anyone who started *before* we
        // started, since we linearized at start time.
        auto end_time = exo.get_start_time();
        exo.ro_end();
        // NB: CM before quiesce, in case CM needs to unblock others
        cm.afterCommit(globals.cm);
        epoch.quiesce(end_time, this, globals.epoch);
        // Clean up
        readset.clear();
        allocator.onCommit();
        defers.onCommit();
        frame.onCommit();
        return;
      }

      // Writer commit: we have all locks, so just validate
      for (auto o : readset)
        if (exo.check_orec(&globals.orecs[o]) == exotm_t::END_OF_TIME)
          abortTx();

      // release locks and exit epoch table
      exo.wo_end();

      // CM, then quiesce, then clean up everything, so that we quiesce before
      // allocator cleanup
      cm.afterCommit(globals.cm);
      epoch.quiesce(exo.get_last_wo_end_time(), this, globals.epoch);
      undolog.clear();
      readset.clear();
      allocator.onCommit();
      defers.onCommit();
      frame.onCommit();
    }
  }

  /// To allocate memory, we must also log it, so we can reclaim it if the
  /// transaction aborts
  void *txAlloc(size_t size) { return allocator.alloc(size); }

  /// To allocate aligned memory, we must also log it, so we can reclaim it if
  /// the transaction aborts
  void *txAAlloc(size_t A, size_t size) {
    return allocator.alignAlloc(A, size);
  }

  /// To free memory, we simply wait until the transaction has committed, and
  /// then we free.
  void txFree(void *addr) { allocator.reclaim(addr); }

  /// Use a simple hash to transform an address into the index of an orec
  int get_orec_index(void *addr) {
    return (reinterpret_cast<uintptr_t>(addr) >> OREC_COVERAGE) %
           globals.NUM_ORECS;
  }

  /// Transactional read
  template <typename T> T read(T *addr) {
    // No instrumentation if on stack or we're irrevocable
    if (accessDirectly(addr))
      return *addr;

    // get the orec address, then start a loop to read a consistent value
    int o = get_orec_index(addr);
    while (true) {
      // read the location, then orec
      T from_mem = undolog_t::safe_read(addr);

      // If validation passes, then we can log it and return
      bool locked = false;
      if (exo.check_orec(&globals.orecs[o], locked) != exotm_t::END_OF_TIME) {
        if (!locked)
          readset.push_back(o);
        return from_mem;
      }

      // abort if locked
      if (locked)
        abortTx();

      // Extend the validity range, then try again
      auto old_start = exo.get_start_time();
      exo.wo_begin();
      validate(old_start);
    }
  }

  /// Transactional write
  template <typename T> void write(T *addr, T val) {
    // No instrumentation if on stack or we're irrevocable
    if (accessDirectly(addr)) {
      *addr = val;
      return;
    }

    // get the orec address, then start a loop to acquire consistently
    int o = get_orec_index(addr);
    while (true) {
      // If I have it or can get it, that's the easy case
      bool locked = false;
      if (exo.acquire_consistent(&globals.orecs[o], locked)) {
        // Add old value to undo log, update memory, and return
        typename undolog_t::undo_t u;
        u.initFromAddr(addr);
        undolog.push_back(u);
        undolog_t::safe_write(addr, val);
        return;
      }

      // abort if locked
      if (locked)
        abortTx();

      // Extend the validity range, then try again
      auto old_start = exo.get_start_time();
      exo.wo_begin();
      validate(old_start);
    }
  }

  /// Instrumentation to become irrevocable in-flight.  This is essentially an
  /// early commit
  void becomeIrrevocable() {
    // Immediately return if we are already irrevocable
    if (epoch.isIrrevoc())
      return;

    // Get the token and quiesce, or else abort
    if (!epoch.tryIrrevoc(globals.epoch, this))
      abortTx();

    // now validate.  If it fails, release irrevocability
    for (auto o : readset) {
      if (exo.check_orec(&globals.orecs[o]) == exotm_t::END_OF_TIME) {
        epoch.onCommitIrrevoc(globals.epoch);
        abortTx();
      }
    }

    // clear lists
    allocator.onCommit();
    readset.clear();
    undolog.clear();
  }

  /// Register an action to run after transaction commit
  void registerCommitHandler(void (*func)(void *), void *args) {
    defers.registerHandler(func, args);
  }

private:
  /// Validation.  We need to make sure that all orecs that we've read have
  /// timestamps older than the given time, unless we locked those orecs. If we
  /// locked the orec, we did so when the time was smaller than our start time,
  /// so we're sure to be OK.
  void validate(uint64_t time) {
    // NB: on relaxed architectures, we may have unnecessary fences here
    for (auto o : readset) {
      bool mine = false;
      bool ok = exo.check_continuation(&globals.orecs[o], time, mine);
      if (!ok && !mine)
        abortTx();
    }
  }

  /// Abort the transaction.  We must handle mallocs and frees, and we need to
  /// ensure that the descriptor is in an appropriate state for starting a new
  /// transaction.  Note that we *will* call beginTx again, unlike libITM.
  void abortTx() {
    // undo any writes
    undolog.undo_writes();

    // Exit the Epoch and CM, so other threads don't have to wait on this thread
    //
    // NB: has to be silent store, because of undo and check-once orecs
    exo.wo_end();
    cm.afterAbort(globals.cm, 0);

    // reset all lists, undo mallocs, and try again
    readset.clear();
    undolog.clear();
    allocator.onAbort();
    defers.onAbort();
    frame.onAbort();
    longjmp(*checkpoint, 1);
  }

  /// Check if the given address is on the thread's stack, and hence does not
  /// need instrumentation.  Note that if the thread is irrevocable, we also say
  /// that instrumentation is not needed.  Also, the allocator may suggest
  /// skipping instrumentation.
  bool accessDirectly(void *ptr) {
    if (epoch.isIrrevoc())
      return true;
    if (allocator.checkCaptured(ptr))
      return true;
    return frame.onStack(ptr);
  }
};
