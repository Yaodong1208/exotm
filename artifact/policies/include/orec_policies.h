#pragma once

#include "hash.h"

/// A policy that places orecs directly in reclaimable objects
///
/// @tparam SMR  The safe memory reclamation's reclaimable object
/// @tparam OREC The orec type (presumably from exoTM)
template <class SMR, class OREC> struct orec_po_t {
  /// The global state for this policy
  ///
  /// NB: This policy does not require any global state
  struct global_t {};

  /// ownable_t places an orec into the object.  The object is compatible with
  /// SMR
  class ownable_t : public SMR {
    OREC _orec; // the orec for this object is embedded directly in it

  protected:
    /// Construct an ownable_t
    ///
    /// @param _ An unused reference to this policy's global state
    ownable_t(global_t &) {}

  public:
    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~ownable_t() {}

    /// Return a reference to the ownable_t's orec
    OREC *orec() { return &_orec; }
  };
};

/// A policy that maps reclaimable objects to entries in a table of orecs
///
/// @tparam SMR  The safe memory reclamation's reclaimable object
/// @tparam OREC The orec type (presumably from exoTM)
template <class SMR, class OREC> struct orec_ps_t {
  /// The global state for this policy
  struct global_t {
    static const int NUM_ORECS = 1048576; // The number of orecs to use
    OREC orecs[NUM_ORECS];                // The table of orecs

    /// Map an address to an orec table entry
    ///
    /// @param obj An ownable for which we require an orec address
    ///
    /// @return The address of the orec associated with the given key
    OREC *get_orec(void *obj) {
      return &orecs[mix13_hash((uintptr_t)obj) % NUM_ORECS];
    }
  };

  /// ownable_t places a reference to an orec into the object.  The object is
  /// compatible with SMR
  ///
  /// NB: By placing a reference into the object, we can use expensive hash
  ///     functions, since we only compute them at object construction time.
  class ownable_t : public SMR {
    OREC *const _orec; // reference to the orec for this object

  protected:
    /// Construct an ownable_t
    ///
    /// @param globals The global state (where the orecs are)
    ownable_t(global_t &globals) : _orec(globals.get_orec(this)) {}

  public:
    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~ownable_t() {}

    /// Return a reference to the ownable_t's orec
    OREC *orec() { return _orec; }
  };
};
