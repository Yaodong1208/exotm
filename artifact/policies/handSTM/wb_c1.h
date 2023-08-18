#pragma once

#include "include/field.h"
#include "include/raii.h"
#include "include/redo_base.h"

/// wb_c1_t is an HandSTM policy with the following features:
/// - Uses ExoTM for orecs and rdtsc clock
/// - Check-once orecs
/// - Encounter-time locking with redo
/// - No quiescence, but safe memory reclamation
/// - Can be configured with per-object or per-stripe orecs
///
/// Note that check-once orecs don't really let us use orec checks to filter out
/// and avoid redolog lookups.
///
/// @tparam OP The orec policy to use.
template <template <typename, typename> typename OP>
struct wb_c1_t : public redo_base_t<OP> {
  using STM = Stm<wb_c1_t>;     // RAII ROSTM/WOSTM base
  using ROSTM = RoStm<wb_c1_t>; // RAII ROSTM manager
  using WOSTM = WoStm<wb_c1_t>; // RAII WOSTM manager

  /// Construct an wb_c1_t
  wb_c1_t() : redo_base_t<OP>() {}

private:
  // Types needed by the (friend) field template, but not worth making public
  using OWNABLE = typename redo_base_t<OP>::ownable_t;
  static const auto EOT = exotm_t::END_OF_TIME;

public:
  /// The type for fields that are shared and protected by HandSTM
  template <typename T> struct xField : public wb_c1_field<T, wb_c1_t> {
    /// Construct an xField
    ///
    /// @param val The initial value
    explicit xField(T val) : wb_c1_field<T, wb_c1_t>(val) {}

    /// Default construct an xField
    explicit xField() : wb_c1_field<T, wb_c1_t>() {}
  };

private:
  // These friends ensure that the rest of the API can access the parts of this
  // that they need
  friend STM;
  friend ROSTM;
  friend WOSTM;
  template <typename T, typename DESCRIPTOR> friend class field_base_t;
  template <typename T, typename DESCRIPTOR> friend class wb_field_t;
  template <typename T, typename DESCRIPTOR> friend class wb_c1_field;
};
