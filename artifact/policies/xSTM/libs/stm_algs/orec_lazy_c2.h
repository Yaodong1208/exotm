/// A traditional xSTM algorithm implementation using redo logging and a bespoke
/// table of check-twice orecs

#pragma once

#include <setjmp.h>

#include "../../../include/minivector.h"
#include "../include/constants.h"
#include "../include/orec_t.h"
#include "include/alloc.h"
#include "include/deferred.h"
#include "include/redolog.h"
#include "include/stackframe.h"

/// OrecLazyC2 is an STM algorithm that is compatible with our LLVM STM plugin.
/// It has the following features:
/// - Uses a custom orec table with pluggable clock types
/// - Check-twice orecs
/// - Commit-time locking with redo
///
/// @param ORECTABLE a table of orecs, and a clock
/// @param EPOCH     an epoch type, for quiescence and irrevocability
/// @param CM        a contention manager, invoked only at begin/commit/abort
template <class ORECTABLE, class EPOCH, class CM> class OrecLazyC2 {
  /// The type of the redo log
  using REDOLOG = redolog_t<1 << OREC_COVERAGE>;

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
  REDOLOG redolog;                  // A redo log, for redoing writes at commit
  BasicAllocationManager allocator; // Manage malloc/free/aligned alloc
  DeferredActionHandler defers;     // Functions to run after commit/abort

public:
  /// Return the irrevocability state of the thread
  bool isIrrevoc() { return epoch.isIrrevoc(); }

  /// Set the current bottom of the transactional part of the stack
  void adjustStackBottom(void *addr) { frame.setBottom(addr); }

  /// construct a thread's transaction context
  OrecLazyC2() : epoch(globals.epoch), cm() {
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
      if (redolog.size() == 0) {
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

      // Writer commit: acquire locks, then validate
      acquireLocks();
      uint64_t end_time = globals.orecs.increment_get();
      // validate if there were any intervening commits
      if (end_time !=
          start_time + 1) { // TODO: Is this behavior correct for rdtscp?
        for (auto o : readset) {
          uint64_t v = o->curr;
          if (v > start_time && v != my_lock)
            abortTx();
        }
      }

      // replay redo log, then release locks and exit epoch table
      redolog.writeback();
      epoch.clearEpoch(globals.epoch);
      releaseLocks(end_time);

      // CM, then quiesce, then clean up everything, so that we quiesce before
      // allocator cleanup
      cm.afterCommit(globals.cm);
      epoch.quiesce(globals.epoch, end_time);
      redolog.reset();
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

    // Lookup in redo log to populate ret.  Note that prior casting can lead to
    // ret having only some bytes properly set
    T ret;
    int found_mask = redolog.find(addr, ret);
    // If we found all the bytes in the redo log, then it's easy
    int desired_mask = (1UL << sizeof(T)) - 1;
    if (desired_mask == found_mask)
      return ret;

    // get the orec address, then start a loop to read a consistent value
    orec_t *o = globals.orecs.get(addr);
    T from_mem;
    while (true) {
      // read the orec, then location, then orec
      local_orec_t pre, post;
      pre.all = o->curr; // fenced read of o->curr
      from_mem = REDOLOG::safe_read(addr);
      post.all = o->curr; // fenced read of o->curr

      // If validation passes, then we can log it and reconstruct
      if ((pre.all == post.all) && (pre.all <= start_time)) {
        readset.push_back(o);
        break;
      }

      // wait if locked
      while (post.fields.lock)
        post.all = o->curr;

      // Extend the validity range, then try again
      uintptr_t newts = globals.orecs.get_time_strong_ordering();
      epoch.setEpoch(globals.epoch, newts);
      validate();
      start_time = newts;
    }

    // If redolog was a partial hit, reconstruction is needed
    if (!found_mask)
      return from_mem;
    REDOLOG::reconstruct(from_mem, ret, found_mask);
    return ret;
  }

  /// Transactional write
  template <typename T> void write(T *addr, T val) {
    // No instrumentation if on stack or we're irrevocable
    if (accessDirectly(addr)) {
      *addr = val;
      return;
    }
    redolog.insert(addr, val);
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
      if (lo.all > start_time) {
        epoch.onCommitIrrevoc(globals.epoch);
        abortTx();
      }
    }

    // replay redo log
    redolog.writeback();

    // clear lists
    allocator.onCommit();
    readset.clear();
    redolog.reset();
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
    bool to_abort = false;
    for (auto o : readset)
      to_abort |= (o->curr > start_time);
    if (to_abort)
      abortTx();
  }

  /// Abort the transaction.  We must handle mallocs and frees, and we need to
  /// ensure that the descriptor is in an appropriate state for starting a new
  /// transaction.  Note that we *will* call beginTx again, unlike libITM.
  void abortTx() {
    // At this point, we can exit the epoch so that other threads don't have to
    // wait on this thread
    epoch.clearEpoch(globals.epoch);
    cm.afterAbort(globals.cm, epoch.id);

    // release any locks held by this thread
    for (auto o : lockset)
      if (o->curr == my_lock)
        o->curr.store(o->prev);

    // reset all lists, undo mallocs, and try again
    readset.clear();
    redolog.reset();
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

  /// During commit, the transaction acquires all locks for its write set
  void acquireLocks() {
    size_t entries = redolog.size();
    for (size_t i = 0; i < entries; ++i) {
      orec_t *o = globals.orecs.get(redolog.get_address(i));
      local_orec_t pre;
      pre.all = o->curr;

      // If lock unheld, acquire; abort on fail to acquire
      if (pre.all <= start_time) {
        if (!o->curr.compare_exchange_strong(pre.all, my_lock))
          abortTx();
        o->prev = pre.all;
        lockset.push_back(o);
      }
      // If lock is not held by me, abort
      else if (pre.all != my_lock)
        abortTx();
    }
  }

  /// Release the locks held by this transaction
  void releaseLocks(uint64_t end_time) {
    // NB: there may be unnecessary fences in this loop
    for (auto o : lockset)
      if (o->curr == my_lock)
        o->curr = end_time;
  }
};
