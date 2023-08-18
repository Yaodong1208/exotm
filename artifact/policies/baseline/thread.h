#pragma once

#include <atomic>
#include <cstdint>

#include "../include/hash.h"
#include "../include/rdtsc_rand.h"
#include "../include/timestamp_smr.h"

/// thread_t provides the union of all per-thread functionality required by the
/// locking and lock-free algorithms in our baseline:
/// - A per-thread pseudorandom number generator
/// - A good hash function
/// - A per-thread context for a safe memory reclamation algorithm
///
/// Note that our primary goals are (1) to normalize as much as possible among
/// implementations, and (2) to normalize as much as possible in the benchmark
/// code.  Thus while some baseline algorithms do not require some (or any) of
/// the above features, we provide them in this common object anyway.
__thread int tid;
class thread_t {

  /// global_t tracks all state shared among threads
  struct global_t {
    timestamp_smr_t::global_t smr; // Globals used for safe memory reclamation
  };

  static global_t _globals; /// All of the globals for stmcas_po
  timestamp_smr_t smr;      // The safe memory reclamation context
  rdtsc_rand_t rng;         // A random number generator

public:
  int tid;
  /// ownable_t is how STMCAS_PO maps locations to orecs.  For now, all fields
  /// of an object will map to a single orec. All objects that are synchronized
  /// by STMCAS will inherit from ownable_t.  When ownable_t constructs, it will
  /// embed an orec in the object.
  ///
  /// Note that ownable_t is a reclaimable_t, and therefore it can be used with
  /// our safe memory reclamation
  struct reclaimable_t : timestamp_smr_t::reclaimable_t {
    /// Construct an object that can be reclaimed by timestamp_smr_t
    reclaimable_t() {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~reclaimable_t() {}
  };

  /// Construct a thread_t
  thread_t(int _tid = 0) : smr(_globals.smr), tid(_tid) {}
  /// Start an operation (notify SMR)
  void op_begin() { smr.enter(); }

  /// End an operation (notify SMR)
  void op_end() { smr.exit(_globals.smr); }

  /// A good hash function.  You should use std::hash to produce a hashed val,
  /// and then this will run a good hash on the result.
  ///
  /// @param hashed_val A size_t produced by std::hash
  ///
  /// @return A 64-bit hash value
  uint64_t hash(size_t hashed_val) { return mix13_hash(hashed_val); }

  /// Produce a random number from a thread-local generator
  ///
  /// NB: This is a convenience for things like skiplists, where we need
  ///     per-thread PRNGs
  int rand() { return rng.rand(); }

  /// Schedule an object for reclamation
  ///
  /// @param obj The object to reclaim
  void reclaim(timestamp_smr_t::reclaimable_t *obj) { smr.reclaim(obj); }
};

/// THREAD_T_GLOBALS_INITIALIZER should be called once, in the main C++ file
/// of a program.  It defines the globals used by THREAD_T, so that we can be
/// sure that any globals declared in this file are defined in a .o file.
/// Failure to use this correctly will lead to link errors.
#define THREAD_T_GLOBALS_INITIALIZER                                           \
  typename thread_t::global_t thread_t::_globals;
