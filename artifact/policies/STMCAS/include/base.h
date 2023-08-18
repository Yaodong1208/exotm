#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>

#include "../../exoTM/exotm.h"
#include "../../include/minivector.h"

#include "../../include/hash.h"
#include "../../include/orec_policies.h"
#include "../../include/rdtsc_rand.h"
#include "../../include/timestamp_smr.h"

/// base_t holds common fields and methods for STMCAS policies.
///
/// base_t is a re-usable descriptor.  This means that it can have some
/// dynamic memory allocation internally.
///
/// @tparam OP The orec policy to use.
template <template <typename, typename> typename OP> class base_t {
  using orec_t = exotm_t::orec_t;                                // Orec type
  using OrecPolicy = OP<timestamp_smr_t::reclaimable_t, orec_t>; // Orec policy

public:
  /// The maximum value an orec can ever have
  static const auto END_OF_TIME = exotm_t::END_OF_TIME;

  /// ownable_t from OP, but with a zero-argument constructor
  struct ownable_t : public OrecPolicy::ownable_t {
    /// Construct an ownable_t
    ownable_t() : OrecPolicy::ownable_t(_globals.op) {}
  };

protected:
  /// A packet holding all globals for STMCAS
  struct global_t {
    timestamp_smr_t::global_t smr;    // Globals for safe memory reclamation
    typename OrecPolicy::global_t op; // Globals for the orec policy
  };

  static global_t _globals; // lightweight singleton-like access to the globals

  exotm_t exo;         // The thread's exoTM context
  timestamp_smr_t smr; // The safe memory reclamation context
  rdtsc_rand_t rng;    // A random number generator

public:
  /// A pair consisting of an ownable and its version.  We use this for
  /// snapshots: _ver is the time at which _obj was last accessed (modified).
  ///
  /// NB: For PS orec configurations, we could have SMR at the granularity of
  ///     steps, but then this would also need an orec reference, since _obj
  ///     could be deleted between steps.
  struct snapshot_t {
    ownable_t *_obj; // An object
    uint64_t _ver;   // A version number associated with `_obj`
  };

  minivector<snapshot_t> snapshots; // Nodes observed during a ds operation

protected:
  /// Construct a base_t
  base_t() : exo(), smr(_globals.smr) {}

public:
  /// Return the time when this thread's last write step committed
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

private:
  // NB:  In order for ccds to be able to do has-a instead of is-a, we need a
  //      friend relationship
  friend class ccds_t;
};

/// STMCAS_GLOBALS_INITIALIZER should be called once, in the main C++ file of a
/// program.  It defines the globals used by STMCAS policies, so that we can be
/// sure that any globals declared in this file are defined in a .o file.
/// Failure to use this correctly will lead to link errors.
#define STMCAS_GLOBALS_INITIALIZER                                             \
  template <template <typename, typename> typename T>                          \
  typename base_t<T>::global_t base_t<T>::_globals;
