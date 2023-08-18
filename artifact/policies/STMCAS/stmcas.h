#pragma once

#include "include/base.h"
#include "include/field.h"
#include "include/raii.h"

/// stmcas_t implements the STMCAS policy on top of exoTM.  All of the
/// functionality is in the included files.  This class's only job is to put all
/// the pieces together in a single object, with appropriate language-level
/// protection.
///
/// @tparam OP The orec policy to use.
template <template <typename, typename> typename OP>
struct stmcas_t : public base_t<OP> {
  using STEP = Step<stmcas_t>;   // RAII RSTEP/WSTEP base
  using RSTEP = RStep<stmcas_t>; // RAII RSTEP manager
  using WSTEP = WStep<stmcas_t>; // RAII WSTEP manager

  /// Construct an stmcas_t
  stmcas_t() : base_t<OP>() {}

  /// The type for fields that are shared and protected by STMCAS
  template <typename T> struct sField : public stmcas_field_t<T, stmcas_t> {
    /// Construct an sField
    ///
    /// @param val The initial value
    explicit sField(T val) : stmcas_field_t<T, stmcas_t>(val) {}

    /// Default-construct an sField
    explicit sField() : stmcas_field_t<T, stmcas_t>() {}
  };

private:
  friend STEP;
  friend WSTEP;
  friend RSTEP;
  friend class ccds_t;
};
