#pragma once

#include "include/field.h"
#include "include/raii.h"
#include "include/redo_base.h"

/// lazy_t is an HandSTM policy with the following features:
/// - Uses ExoTM for orecs and rdtsc clock
/// - Check-once orecs
/// - Commit-time locking with redo
/// - No quiescence, but safe memory reclamation
/// - Can be configured with per-object or per-stripe orecs
///
/// Note that hardware clocks + lazy --> no benefit to check-twice orecs, so we
/// only have one lazy version of HandSTM.
///
/// @tparam OP The orec policy to use.
template <template <typename, typename> typename OP>
struct lazy_t : public redo_base_t<OP> {
  using STM = Stm<lazy_t>;     // RAII ROSTM/WOSTM base
  using ROSTM = RoStm<lazy_t>; // RAII ROSTM manager
  using WOSTM = WoStm<lazy_t>; // RAII WOSTM manager

  /// Construct a lazy_t
  lazy_t() : redo_base_t<OP>() {}

private:
  // Types needed by the (friend) field template, but not worth making public
  using OWNABLE = typename redo_base_t<OP>::ownable_t;
  static const auto EOT = exotm_t::END_OF_TIME;

public:
  /// The type for fields that are shared and protected by HandSTM
  template <typename T> struct xField : public lazy_field<T, lazy_t> {
    /// Construct an xField
    ///
    /// @param val The initial value
    explicit xField(T val) : lazy_field<T, lazy_t>(val) {}

    /// Default construct an xField
    explicit xField() : lazy_field<T, lazy_t>() {}
  };

private:
  // These friends ensure that the rest of the API can access the parts of this
  // that they need
  friend STM;
  friend ROSTM;
  friend WOSTM;
  template <typename T, typename DESCRIPTOR> friend class field_base_t;
  template <typename T, typename DESCRIPTOR> friend class lazy_field;
};
