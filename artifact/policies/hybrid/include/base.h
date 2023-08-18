#pragma once

#include <setjmp.h>

#include "../../exoTM/exotm.h"
#include "../../include/hash.h"
#include "../../include/orec_policies.h"
#include "../../include/rdtsc_rand.h"
#include "../../include/redolog_nocast.h"
#include "../../include/timestamp_smr.h"

/// base_t has the common parts for building hybrid policies that combine
/// HandSTM with STMCAS:
/// - Uses ExoTM for orecs and rdtsc clock
/// - Redo log support
/// - No quiescence, but safe memory reclamation
/// - Can be configured with per-object or per-stripe orecs
///
/// base_t is a re-usable descriptor.  This means that it can have some dynamic
/// memory allocation internally.
///
/// @tparam OP The orec policy to use.
template <template <typename, typename> typename OP> class base_t {
  using orec_t = exotm_t::orec_t;                                // Orec type
  using OrecPolicy = OP<timestamp_smr_t::reclaimable_t, orec_t>; // Orec policy
  using REDOLOG = redolog_nocast_t<32>;                          // Redo log

public:
  /// The maximum value an orec can ever have
  static const auto END_OF_TIME = exotm_t::END_OF_TIME;

  /// ownable_t from OP, but with a zero-argument constructor
  struct ownable_t : public OrecPolicy::ownable_t {
    /// Construct an ownable_t
    ownable_t() : OrecPolicy::ownable_t(_globals.op) {}
  };

protected:
  /// A packet holding all globals for hybrid policies
  struct global_t {
    timestamp_smr_t::global_t smr;    // Globals for safe memory reclamation
    typename OrecPolicy::global_t op; // Globals for the orec policy
  };

  static global_t _globals; // lightweight singleton-like access to the globals

  exotm_t exo;                     // Per-thread ExoTM metadata
  timestamp_smr_t smr;             // The safe memory reclamation context
  rdtsc_rand_t rng;                // A random number generator
  jmp_buf *checkpoint;             // Register checkpoint, for aborts
  minivector<orec_t *> readset;    // Orecs to validate
  minivector<orec_t *> lockset;    // Locks to acquire / locks that are acquired
  REDOLOG redolog;                 // A redo log, for replaying writes on commit
  minivector<ownable_t *> mallocs; // pending allocations
  minivector<ownable_t *> frees;   // pending reclaims

public:
  /// A copy of snapshot_t from STMCAS
  struct snapshot_t {
    ownable_t *_obj; // An object
    uint64_t _ver;   // A version number associated with `_obj`
  };

  minivector<snapshot_t> snapshots; // Nodes observed during a ds operation

protected:
  /// Construct a base_t
  base_t() : exo(), smr(_globals.smr) {}

  /// acquire_all(), copied from HandSTM::redo_base_t
  void acquire_all() {
    for (auto o : lockset)
      if (!exo.acquire_consistent(o))
        abort();
  }

  /// validate(), copied from HandSTM::redo_base_t
  void validate() {
    // NB: on relaxed architectures, we may have unnecessary fences here
    for (auto o : readset) {
      if (exo.check_orec(o) == exotm_t::END_OF_TIME)
        abort();
    }
  }

  /// validate(uint64_t), copied from HandSTM::redo_base_t
  void validate(uint64_t old_start) {
    // NB: on relaxed architectures, we may have unnecessary fences here
    for (auto o : readset) {
      bool mine = false;
      bool ok = exo.check_continuation(o, old_start, mine);
      if (!ok && !mine)
        abort();
    }
  }

  /// abort(), copied from HandSTM::redo_base_t
  void abort() {
    exo.unwind(exotm_t::ROLLBACK_ORECS); // roll back locks to release them

    // reset all lists.  Note that we can free right away, without SMR, because
    // writes are in the redo log.
    frees.clear();
    for (auto p : mallocs)
      free(p);
    mallocs.clear();
    readset.clear();
    redolog.clear();
    lockset.clear();
    longjmp(*checkpoint, 1);
  }

  /// commit(), copied from HandSTM::redo_base_t
  void commit() {
    // NB: Good use of RSTEP should ensure this never happens, but let's
    // optimize just in case.
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
      smr.reclaim(a);
    frees.clear();
    redolog.clear();
    lockset.clear();
    readset.clear();
  }

  /// In order for an STM step to follow an STMCAS step, we need to be able to
  /// check orecs and add them to the read set.  This method provides that
  /// ability.
  ///
  /// @param obj The ownable object whose orec needs to match some value
  /// @param val The value that we must see
  ///
  /// @return True if the value matches and the STM can continue, false
  ///         otherwise
  bool inheritOrec(ownable_t *obj, uint64_t val) {
    readset.push_back(obj->orec());
    return exo.check_continuation(obj->orec(), val);
  }

public:
  /// get_last_wo_end_time(), copied from STMCAS::base_t
  uint64_t get_last_wo_end_time() { return exo.get_last_wo_end_time(); }

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
};

/// HYBRID_GLOBALS_INITIALIZER should be called once, in the main C++ file of a
/// program.  It defines the globals used by hybrid policies, so that we can be
/// sure that any globals declared in this file are defined in a .o file.
/// Failure to use this correctly will lead to link errors.
#define HYBRID_GLOBALS_INITIALIZER                                             \
  template <template <typename, typename> typename T>                          \
  typename base_t<T>::global_t base_t<T>::_globals
