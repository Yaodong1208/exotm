#pragma once

#include <setjmp.h>

#include "../../exoTM/exotm.h"
#include "../../include/hash.h"
#include "../../include/orec_policies.h"
#include "../../include/rdtsc_rand.h"
#include "../../include/redolog_nocast.h"
#include "../../include/timestamp_smr.h"

/// redo_base_t has the common parts for building HandSTM algorithms that use
/// redo logging.
/// - Uses ExoTM for orecs and rdtsc clock
/// - Redo log support
/// - No quiescence, but safe memory reclamation
/// - Can be configured with per-object or per-stripe orecs
///
/// redo_base_t is a re-usable descriptor.  This means that it can have some
/// dynamic memory allocation internally.
///
/// @tparam OP The orec policy to use.
template <template <typename, typename> typename OP> struct redo_base_t {
  using orec_t = exotm_t::orec_t;                                // Orec type
  using OrecPolicy = OP<timestamp_smr_t::reclaimable_t, orec_t>; // Orec policy
  using REDOLOG = redolog_nocast_t<32>;                          // Redo log

  /// ownable_t from OP, but with a zero-argument constructor.
  struct ownable_t : public OrecPolicy::ownable_t {
    /// Construct an ownable_t
    ownable_t() : OrecPolicy::ownable_t(_globals.op) {}
  };

  /// A packet holding all globals for redo HandSTM policies
  struct global_t {
    timestamp_smr_t::global_t smr;    // Globals for safe memory reclamation
    typename OrecPolicy::global_t op; // Globals for the orec policy
  };

  static global_t _globals; // lightweight singleton-like access to the globals

  exotm_t exo;                     // The thread's exoTM context
  timestamp_smr_t smr;             // The safe memory reclamation context
  rdtsc_rand_t rng;                // A random number generator
  jmp_buf *checkpoint;             // Register checkpoint, for aborts
  minivector<orec_t *> readset;    // Orecs to validate
  minivector<orec_t *> lockset;    // Locks to acquire
  REDOLOG redolog;                 // A redo log, for replaying writes on commit
  minivector<ownable_t *> mallocs; // pending allocations
  minivector<ownable_t *> frees;   // pending reclaims

  /// Construct a redo_base_t
  redo_base_t() : exo(), smr(_globals.smr) {}

  /// Acquire all un-acquired orecs from the lockset
  void acquire_all() {
    for (auto o : lockset)
      if (!exo.acquire_consistent(o))
        abort();
  }

  /// Ensure that all orecs that we've read have timestamps older than the start
  /// time, unless we locked those orecs. If we locked the orec, we did so when
  /// the time was smaller than our start time, so we're sure to be OK.
  void validate() {
    // NB: on relaxed architectures, we may have unnecessary fences here
    for (auto o : readset)
      if (exo.check_orec(o) == exotm_t::END_OF_TIME)
        abort();
  }

  /// Specialized version of validation for timestamp extension.  Compare
  /// against old_start, not exo.start_time.
  void validate(uint64_t old_start) {
    for (auto o : readset) {
      bool mine = false;
      bool ok = exo.check_continuation(o, old_start, mine);
      if (!ok && !mine)
        abort();
    }
  }

  /// Unwind the transaction
  void abort() {
    exo.unwind(exotm_t::ROLLBACK_ORECS); // roll back locks to release them

    // reset all lists.  Note that we can free right away, without SMR.
    frees.clear();
    for (auto p : mallocs)
      free(p);
    mallocs.clear();
    readset.clear();
    redolog.clear();
    lockset.clear();
    longjmp(*checkpoint, 1);
  }

  /// Start an operation (notify SMR)
  void op_begin() { smr.enter(); }

  /// End an operation (notify SMR)
  void op_end() { smr.exit(_globals.smr); }

  /// A good hash function.  Works nicely to "finalize" after std::hash().
  ///
  /// @param val The value to hash
  ///
  /// @return A 64-bit hash value
  uint64_t hash(size_t val) { return mix13_hash(val); }

  /// Produce a random number from a thread-local generator
  int rand() { return rng.rand(); }

  /// Commit a writing transaction
  void commit() {
    // read-only fast-path
    if (lockset.empty() && !exo.has_orecs()) {
      exo.ro_end();
      readset.clear();
      return;
    }

    // Acquire locks, if there are any that aren't acquired yet, then validate
    acquire_all();
    validate();

    // We're committed, so write-back, release locks, and clean up
    redolog.writeback();
    exo.wo_end();
    mallocs.clear();
    for (auto a : frees)
      smr.reclaim(a); // Need SMR here!
    frees.clear();
    redolog.clear();
    lockset.clear();
    readset.clear();
  }
};

/// HANDSTM_GLOBALS_INITIALIZER should be called once, in the main C++ file of a
/// program.  It defines the globals used by redo HandSTM policies, so that we
/// can be sure that any globals declared in this file are defined in a .o file.
/// Failure to use this correctly will lead to link errors.
#define HANDSTM_GLOBALS_INITIALIZER                                            \
  template <template <typename, typename> typename T>                          \
  typename redo_base_t<T>::global_t redo_base_t<T>::_globals