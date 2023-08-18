/// A traditional xSTM algorithm implementation using undo logging and a bespoke
/// table of check-twice orecs

#pragma once

#include <setjmp.h>

#include "../../../include/minivector.h"
#include "../../../include/undolog.h"
#include "../include/orec_t.h"
#include "include/alloc.h"
#include "include/deferred.h"
#include "include/stackframe.h"

/// OrecEagerC2 is an STM algorithm that is compatible with our LLVM STM plugin.
/// It has the following features:
/// - Uses a custom orec table with pluggable clock types
/// - Check-twice orecs
/// - Encounter-time locking with undo
///
/// @param ORECTABLE a table of orecs, and a clock
/// @param EPOCH     an epoch type, for quiescence and irrevocability
/// @param CM        a contention manager, invoked only at begin/commit/abort
template <class ORECTABLE, class EPOCH, class CM> class OrecEagerC2 {
  /// All of the global variables used by this STM algorithm
  struct Globals {
    ORECTABLE orecs;               // Orecs and a clock
    typename CM::Globals cm;       // Global Contention Management info
    typename EPOCH::Globals epoch; // Quiescence and Irrevocability
  };

  static Globals globals;           // All metadata shared among threads
  jmp_buf *checkpoint = nullptr;    // Register checkpoint, for aborts
  EPOCH epoch;                      // Quiescence and Irrevocability
  CM cm;                            // Contention manager
  OptimizedStackFrameManager frame; // For tracking the transaction's stack
  uint64_t start_time;              // Transaction start time
  uint64_t my_lock;                 // Per-thread unique lock word
  minivector<orec_t *> readset;     // Orecs to validate
  minivector<orec_t *> lockset;     // Locks that are held
  undolog_t undolog;                // An undo log, for undoing writes on abort
  BasicAllocationManager allocator; // Manage malloc/free/aligned alloc
  DeferredActionHandler defers;     // Functions to run after commit/abort

public:
  /// Return the irrevocability state of the thread
  bool isIrrevoc() { return epoch.isIrrevoc(); }

  /// Set the current bottom of the transactional part of the stack
  void adjustStackBottom(void *addr) { frame.setBottom(addr); }

  /// construct a thread's transaction context
  OrecEagerC2() : epoch(globals.epoch), cm() {
    my_lock = ORECTABLE::make_lockword(epoch.id);
  }

  /// Instrumentation to run at the beginning of a transaction
  void beginTx(jmp_buf *b) {
    // onBegin == false -> flat nesting
    if (frame.onBegin()) {
      // Save the checkpoint and set the stack bottom
      checkpoint = b;
      frame.setBottom(b);

      // Start logging allocations
      allocator.onBegin();

      // Get the start time, and put it into the epoch.  epoch.onBegin will wait
      // until there are no irrevocable transactions.
      start_time = globals.orecs.get_time_strong_ordering();
      epoch.onBegin(globals.epoch, start_time);

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
        epoch.onCommitIrrevoc(globals.epoch);
        cm.afterCommit(globals.cm);
        defers.onCommit();
        frame.onCommit();
        return;
      }

      // fast-path for read-only transactions must still quiesce before freeing
      if (lockset.empty()) {
        // NB: CM before quiesce, in case CM needs to unblock others
        epoch.clearEpoch(globals.epoch);
        cm.afterCommit(globals.cm);
        epoch.quiesce(globals.epoch, start_time);
        // Clean up
        readset.clear();
        allocator.onCommit();
        defers.onCommit();
        frame.onCommit();
        return;
      }

      // Writer commit: we have all locks, so just validate
      uint64_t end_time = globals.orecs.increment_get();
      if (end_time != start_time + 1) {
        for (auto o : readset) {
          uint64_t v = o->curr;
          if (v > start_time && v != my_lock)
            abortTx();
        }
      }

      // release locks, exit epoch
      epoch.clearEpoch(globals.epoch);
      for (auto o : lockset)
        o->curr = end_time;

      // CM, then quiesce, then clean up everything, so that we quiesce before
      // allocator cleanup
      cm.afterCommit(globals.cm);
      epoch.quiesce(globals.epoch, end_time);
      undolog.clear();
      lockset.clear();
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

  /// Transactional read
  template <typename T> T read(T *addr) {
    // No instrumentation if on stack or we're irrevocable
    if (accessDirectly(addr))
      return *addr;

    // get the orec address, then start a loop to read a consistent value
    orec_t *o = globals.orecs.get(addr);
    while (true) {
      // read the orec, then location
      local_orec_t pre, post;
      pre.all = o->curr; // fenced read of o->curr
      T from_mem = undolog_t::safe_read(addr);
      // If I've got it locked, we're done, otherwise re-check orec
      if (pre.all == my_lock)
        return from_mem;

      // If validation passes, then we can log it and return
      post.all = o->curr;
      if ((pre.all == post.all) && (pre.all <= start_time)) {
        readset.push_back(o);
        return from_mem;
      }

      // abort if locked
      if (post.fields.lock)
        abortTx();

      // Extend the validity range, then try again
      uintptr_t newts = globals.orecs.get_time_strong_ordering();
      epoch.setEpoch(globals.epoch, newts);
      validate();
      start_time = newts;
    }
  }

  /// Transactional write
  template <typename T> void write(T *addr, T val) {
    // No instrumentation if on stack or we're irrevocable
    if (accessDirectly(addr)) {
      *addr = val;
      return;
    }

    // get the orec address, then start a loop to ensure a consistent value
    orec_t *o = globals.orecs.get(addr);
    while (true) {
      // If I have it or can get it, that's the easy case
      local_orec_t pre;
      pre.all = o->curr;
      if (pre.all <= start_time) {
        if (!o->curr.compare_exchange_strong(pre.all, my_lock))
          abortTx();
        lockset.push_back(o);
        o->prev = pre.all;
        break;
      }

      // If lock held by me, all good
      if (pre.all == my_lock)
        break;

      // abort if locked
      if (pre.fields.lock)
        abortTx();

      // Extend the validity range, then try again
      uintptr_t newts = globals.orecs.get_time_strong_ordering();
      epoch.setEpoch(globals.epoch, newts);
      validate();
      start_time = newts;
    }
    // Add old value to undo log, update memory, and return
    typename undolog_t::undo_t u;
    u.initFromAddr(addr);
    undolog.push_back(u);
    undolog_t::safe_write(addr, val);
    return;
  }

  /// Instrumentation to become irrevocable in-flight.  This is essentially an
  /// early commit
  void becomeIrrevocable() {
    // Immediately return if we are already irrevocable
    if (epoch.isIrrevoc())
      return;

    // try_irrevoc will return true only if we got the token and quiesced
    if (!epoch.tryIrrevoc(globals.epoch))
      abortTx();

    // now validate.  If it fails, release irrevocability
    for (auto o : readset) {
      local_orec_t lo;
      lo.all = o->curr;
      if (lo.all > start_time && lo.all != my_lock) {
        epoch.onCommitIrrevoc(globals.epoch);
        abortTx();
      }
    }

    // get a commit time and release locks
    uint64_t end_time = globals.orecs.increment_get();
    for (auto o : lockset)
      o->curr = end_time;

    // clear lists
    allocator.onCommit();
    readset.clear();
    undolog.clear();
    lockset.clear();
  }

  /// Register an action to run after transaction commit
  void registerCommitHandler(void (*func)(void *), void *args) {
    defers.registerHandler(func, args);
  }

private:
  /// Validation.  We need to make sure that all orecs that we've read have
  /// timestamps older than our start time, unless we locked those orecs. If we
  /// locked the orec, we did so when the time was smaller than our start time,
  /// so we're sure to be OK.
  void validate() {
    // NB: on relaxed architectures, we may have unnecessary fences here
    for (auto o : readset) {
      local_orec_t lo;
      lo.all = o->curr;
      if (lo.all > start_time && lo.all != my_lock) {
        abortTx();
      }
    }
  }

  /// Abort the transaction.  We must handle mallocs and frees, and we need to
  /// ensure that the descriptor is in an appropriate state for starting a new
  /// transaction.  Note that we *will* call beginTx again, unlike libITM.
  void abortTx() {
    // undo any writes
    undolog.undo_writes();

    // At this point, we can exit the epoch so that other threads don't have to
    // wait on this thread
    epoch.clearEpoch(globals.epoch);
    cm.afterAbort(globals.cm, epoch.id);

    // Release locks.  Bump orecs, because of check-twice orecs
    uintptr_t max = 0;
    for (auto o : lockset) {
      uint64_t val = o->prev + 1;
      o->curr = val;
      max = (val > max) ? val : max;
    }
    uintptr_t ts = globals.orecs.get_time_strong_ordering();
    if (max > ts)
      globals.orecs.increment();

    // reset all lists, undo mallocs, and try again
    readset.clear();
    undolog.clear();
    lockset.clear();
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