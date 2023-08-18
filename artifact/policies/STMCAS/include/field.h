#pragma once

#include <atomic>

/// stmcas_field_t is a wrapper around simple types so that they can only be
/// accessed via STMCAS.
///
/// NB: It is the programmer's responsibility to ensure that these fields are
///     accessed correctly.  We seek razor-thin overheads, so the programmer
///     needs to validate after a read, or acquire before a write.
///
/// @tparam T       The type to store
/// @tparam STMCAS  The STMCAS version to use
template <typename T, typename STMCAS> class stmcas_field_t {
  std::atomic<T> _val; // The value.  Must be atomic per C++ memory model.

public:
  /// Construct an stmcas_field_t
  ///
  /// @param val The initial value
  explicit stmcas_field_t(T val) : _val(val) {}

  /// Default-construct an stmcas_field_t
  explicit stmcas_field_t() : _val() {}

  /// Read the field from a RSTEP or WSTEP.  The caller is responsible
  /// for validating the orec.
  ///
  /// @param STEP An unused parameter to restrict access to *STEP contexts
  ///
  /// @return The current value
  T get(typename STMCAS::STEP &) const {
    // TODO:  We're only concerned about x86 for now, so we'll just use
    //        memory_order_acquire.  On ARM, we would want to use a relaxed
    //        read, and have a thread fence later on (i.e., before the
    //        validate).
    return _val.load(std::memory_order_acquire);
  }

  /// Write the field from a WSTEP.  The caller must ensure the
  /// corresponding orec is owned before calling this.
  ///
  /// NB: memory_order_relaxed, because we assume it is owned
  ///
  /// @param val   The new value
  /// @param WSTEP An unused parameter to restrict access to WSTEP contexts
  void set(T val, typename STMCAS::WSTEP &) {
    _val.store(val, std::memory_order_relaxed);
  }
};
