#pragma once

#include <atomic>
#include <climits>
#include <deque>
#include <utility>
#include <x86intrin.h>

#include "minivector.h"

/// timestamp_smr_t is a safe memory reclamation algorithm based on the use of
/// timestamps.
///
/// The key idea behind timestamp_smr_t is that an in-flight operation can
/// create a bundle of pointers that it would like to delete if it successfully
/// completes.  If the operation fails, it will discard the bundle.  If the
/// operation succeeds, it will associate a timestamp with that bundle of
/// pointers, and add it to a large collection of timestamped pointers.  After
/// every K additions of a bundle to the collection, the thread will sweep its
/// collection and reclaim any pointers that it can prove to be unreachable.
///
/// The reachability argument for pointers is based on the timestamps.  An
/// operation gets a timestamp when it starts, and clears that timestamp when it
/// finishes.  Thus any thread with a cleared timestamp, or a timestamp strictly
/// larger than a bundle's timestamp, cannot have references to anything in the
/// bundle.
///
/// Note that for convenience, we don't actually have a bundle.  Instead, we
/// store a flat list of {pointer, timestamp} pairs.
///
/// Each thread should have its own timestamp_smr_t instance, all of which
/// should share the same timestamp_smr_t::global_t instance
class timestamp_smr_t {
  /// How many times can we insert into the unreachable set before requiring a
  /// sweep()
  static const int SWEEP_THRESHOLD = 1024;

public:
  /// The parent type for all objects managed by timestamp_smr_t. It consists
  /// solely of a virtual destructor, to ensure a proper chain of destruction
  /// when anything is reclaimed.
  struct reclaimable_t {
    /// Destroy this object and reclaim its memory
    virtual ~reclaimable_t() {}
  };

  /// Global variables related to timestamp_smr_t.  A synchronization policy
  /// that uses timestamp_smr_t is responsible for defining exactly one instance
  /// of this object.
  struct global_t {
    /// A pointer to the head of the list of timestamp_smr_t contexts
    std::atomic<timestamp_smr_t *> all_threads;

    /// Construct a global context by zeroing the pointer to thread contexts
    global_t() : all_threads(nullptr) {}
  };

private:
  timestamp_smr_t *next;               // Next thread in the chain
  std::atomic<uint64_t> ts;            // This thread's timestamp
  minivector<reclaimable_t *> pending; // Objects to reclaim

  /// Objects that are logically unreachable, but maybe not reclaimable yet due
  /// to concurrent optimistic accesses.
  std::deque<std::pair<reclaimable_t *, uint64_t>> unreachable;

  /// How many more exits before we need to sweep
  int exits_remaining = SWEEP_THRESHOLD;

public:
  /// Construct a timestamp_smr_t context by atomically adding it to the end of
  /// the global list
  timestamp_smr_t(global_t &_globals) : ts(ULLONG_MAX) {
    while (true) {
      timestamp_smr_t *curr_head = _globals.all_threads;
      next = curr_head;
      if (_globals.all_threads.compare_exchange_strong(curr_head, this))
        break;
    }
  }

  /// Begin a region that will optimistically access reclaimable_t objects
  void enter() {
    // enter the "epoch"
    unsigned int dummy;
    // TODO: Can we get by with rdtsc, since ts.exchange is a load/store fence
    //        and there is a data dependence?
    ts.exchange(__rdtscp(&dummy));
  }

  /// Exit a region that optimistically accesses reclaimable_t objects
  ///
  /// @param globals A reference to the global state for timestamp_smr_t
  void exit(global_t &globals) {
    // exit the "epoch"
    ts = (ULLONG_MAX); // only need store fence, not load fence
    // If we have pendings, we need a timestamp for them, then we can move them
    // to `unreachable`
    if (!pending.size())
      return;
    unsigned int dummy;
    uint64_t time = __rdtscp(&dummy);
    for (auto p : pending)
      unreachable.push_back({p, time});
    pending.clear();
    // Check if it's time to sweep...
    if (--exits_remaining > 0)
      return;
    exits_remaining = SWEEP_THRESHOLD;
    sweep(globals);
  }

  /// Schedule an object for reclamation
  void reclaim(reclaimable_t *ptr) { pending.push_back(ptr); }

private:
  /// Traverse the `unreachable` collection and reclaim anything whose timestamp
  /// indicates that it cannot be undergoing optimistic access.
  ///
  /// @param globals A reference to the global state for timestamp_smr_t
  void sweep(global_t &globals) {
    // Find ts of oldest running operation
    uint64_t oldest = ULLONG_MAX;
    auto head = globals.all_threads.load();
    while (head != nullptr) {
      auto t = head->ts.load();
      if (t < oldest)
        oldest = t;
      head = head->next;
    }
    // We know the deque is ordered from oldest to newest, so keep sweeping from
    // the front until there's nothing old enough
    while (!unreachable.empty()) {
      auto [ptr, ts] = unreachable.front();
      if (ts >= oldest)
        return;
      delete ptr;
      unreachable.pop_front();
    }
  }
};
