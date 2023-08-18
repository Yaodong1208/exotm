#pragma once

#include <setjmp.h>

#include "../../exoTM/exotm.h"
#include "../../include/hash.h"
#include "../../include/orec_policies.h"
#include "../../include/rdtsc_rand.h"
#include "../../include/timestamp_smr.h"
#include "../../include/undolog.h"

/// undo_base_t has the common parts for building HandSTM algorithms that use
/// undo logging.
/// - Uses ExoTM for orecs and rdtsc clock
/// - Undo log support
/// - No quiescence, but safe memory reclamation
/// - Can be configured with per-object or per-stripe orecs
///
/// undo_base_t is a re-usable descriptor.  This means that it can have some
/// dynamic memory allocation internally.
///
/// @tparam OP                     The orec policy to use.
/// @tparam ABORT_AS_SILENT_STORE  True if the policy needs silent stores on
///                                abort (get new orec version), false if
///                                bumping orecs by one is sufficient.
template <template <typename, typename> typename OP, bool ABORT_AS_SILENT_STORE>
class undo_base_t {
  using orec_t = exotm_t::orec_t;                                // Orec type
  using OrecPolicy = OP<timestamp_smr_t::reclaimable_t, orec_t>; // Orec Policy

public:
  /// ownable_t from OP, but with a zero-argument constructor.
  struct ownable_t : public OrecPolicy::ownable_t {
    /// Construct an ownable_t
    ownable_t() : OrecPolicy::ownable_t(_globals.op) {}
  };

protected:
  /// A packet storing all globals for undo HandSTM policies
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
  undolog_t undolog;               // An undo log, for undoing writes on abort
  minivector<ownable_t *> mallocs; // pending allocations
  minivector<ownable_t *> frees;   // pending reclaims

  /// Construct an undo_base_t
  undo_base_t() : exo(), smr(_globals.smr) {}

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
    // NB: on relaxed architectures, we may have unnecessary fences here
    for (auto o : readset) {
      bool mine = false;
      bool ok = exo.check_continuation(o, old_start, mine);
      if (!ok && !mine)
        abort();
    }
  }

  /// Unwind the transaction
  void abort() {
    undolog.undo_writes();
    if (ABORT_AS_SILENT_STORE)
      exo.wo_end(); // commit as silent store to release locks
    else
      exo.unwind(exotm_t::BUMP_ORECS); // bump locks to release them

    // reset all lists.  Note that we can free right away, without SMR.
    frees.clear();
    for (auto p : mallocs)
      free(p);
    mallocs.clear();
    readset.clear();
    undolog.clear();
    longjmp(*checkpoint, 1);
  }

public:
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

protected:
  /// Commit a writing transaction
  void commit() {
    // read-only fast-path
    if (!exo.has_orecs()) {
      exo.ro_end();
      readset.clear();
      return;
    }

    // Locks are already acquired, so just validate
    validate();

    // We're committed, so release locks and clean up
    exo.wo_end();
    mallocs.clear();
    for (auto a : frees)
      smr.reclaim(a); // Need SMR here!
    frees.clear();
    undolog.clear();
    readset.clear();
  }
};

/// HANDSTM_GLOBALS_INITIALIZER should be called once, in the main C++ file of a
/// program.  It defines the globals used by undo HandSTM policies, so that we
/// can be sure that any globals declared in this file are defined in a .o file.
/// Failure to use this correctly will lead to link errors.
#define HANDSTM_GLOBALS_INITIALIZER                                            \
  template <template <typename, typename> typename T, bool B>                  \
  typename undo_base_t<T, B>::global_t undo_base_t<T, B>::_globals
