#pragma once

#include "include/field.h"
#include "include/raii.h"
#include "include/undo_base.h"

/// eager_c2_t is an HandSTM policy with the following features:
/// - Uses ExoTM for orecs and rdtsc clock
/// - Check-twice orecs
/// - Encounter-time locking with undo
/// - No quiescence, but safe memory reclamation
/// - Can be configured with per-object or per-stripe orecs
///
/// @tparam OP The orec policy to use.
template <template <typename, typename> typename OP>
struct eager_c2_t : public undo_base_t<OP, false> {
  using STM = Stm<eager_c2_t>;     // RAII ROSTM/WOSTM base
  using ROSTM = RoStm<eager_c2_t>; // RAII ROSTM manager
  using WOSTM = WoStm<eager_c2_t>; // RAII WOSTM manager

  /// Construct an eager_c2_t
  eager_c2_t() : undo_base_t<OP, false>() {}

private:
  // Types needed by the (friend) field template, but not worth making public
  using OWNABLE = typename undo_base_t<OP, false>::ownable_t;
  using UNDO_T = undolog_t::undo_t;
  static const auto EOT = exotm_t::END_OF_TIME;

public:
  /// The type for fields that are shared and protected by HandSTM
  template <typename T> struct xField : public eager_c2_field<T, eager_c2_t> {
    /// Construct an xField
    ///
    /// @param val The initial value
    explicit xField(T val) : eager_c2_field<T, eager_c2_t>(val) {}

    /// Default construct an xField
    explicit xField() : eager_c2_field<T, eager_c2_t>() {}
  };

private:
  // These friends ensure that the rest of the API can access the parts of this
  // that they need
  friend STM;
  friend ROSTM;
  friend WOSTM;
  template <typename T, typename DESCRIPTOR> friend class field_base_t;
  template <typename T, typename DESCRIPTOR> friend class eager_field_t;
  template <typename T, typename DESCRIPTOR> friend class eager_c2_field;
};
