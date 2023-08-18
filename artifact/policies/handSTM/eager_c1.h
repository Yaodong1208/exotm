#pragma once

#include "include/field.h"
#include "include/raii.h"
#include "include/undo_base.h"

/// eager_c1_t is an HandSTM policy with the following features:
/// - Uses ExoTM for orecs and rdtsc clock
/// - Check-once orecs
/// - Encounter-time locking with undo
/// - No quiescence, but safe memory reclamation
/// - Can be configured with per-object or per-stripe orecs
///
/// @tparam OP The orec policy to use.
template <template <typename, typename> typename OP>
struct eager_c1_t : public undo_base_t<OP, true> {
  using STM = Stm<eager_c1_t>;     // RAII ROSTM/WOSTM base
  using ROSTM = RoStm<eager_c1_t>; // RAII ROSTM manager
  using WOSTM = WoStm<eager_c1_t>; // RAII WOSTM manager

  /// Construct an eager_c1_t
  eager_c1_t() : undo_base_t<OP, true>() {}

private:
  // Types needed by the (friend) field template, but not worth making public
  using OWNABLE = typename undo_base_t<OP, true>::ownable_t;
  using UNDO_T = undolog_t::undo_t;
  static const auto EOT = exotm_t::END_OF_TIME;

public:
  /// The type for fields that are shared and protected by HandSTM
  template <typename T> struct xField : public eager_c1_field<T, eager_c1_t> {
    /// Construct an xField
    ///
    /// @param val The initial value
    explicit xField(T val) : eager_c1_field<T, eager_c1_t>(val) {}

    /// Default construct an xField
    explicit xField() : eager_c1_field<T, eager_c1_t>() {}
  };

private:
  // These friends ensure that the rest of the API can access the parts of this
  // that they need
  friend STM;
  friend ROSTM;
  friend WOSTM;
  template <typename T, typename DESCRIPTOR> friend class field_base_t;
  template <typename T, typename DESCRIPTOR> friend class eager_field_t;
  template <typename T, typename DESCRIPTOR> friend class eager_c1_field;
};
