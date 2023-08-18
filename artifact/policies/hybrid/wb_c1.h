#pragma once

#include "include/base.h"
#include "include/field.h"
#include "include/raii.h"

/// wb_c1_t is a hybrid HandSTM+STMCAS policy:
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
struct wb_c1_t : public base_t<OP> {
  using STM = Stm<wb_c1_t>;     // RAII ROSTM/WOSTM base
  using ROSTM = RoStm<wb_c1_t>; // RAII ROSTM manager
  using WOSTM = WoStm<wb_c1_t>; // RAII WOSTM manager
  using STEP = Step<wb_c1_t>;   // RAII RSTEP/WSTEP base
  using RSTEP = RStep<wb_c1_t>; // RAII RSTEP manager
  using WSTEP = WStep<wb_c1_t>; // RAII WSTEP manager

  /// Construct a wb_c1_t
  wb_c1_t() : base_t<OP>() {}

private:
  // Types needed by the (friend) field template, but not worth making public
  using OWNABLE = typename base_t<OP>::ownable_t;
  static const auto EOT = exotm_t::END_OF_TIME;

public:
  /// The type for fields that are shared and protected by HyPol
  template <typename T> struct sxField : public wb_c1_field<T, wb_c1_t> {
    /// Construct an sxField
    ///
    /// @param val The initial value
    explicit sxField(T val) : wb_c1_field<T, wb_c1_t>(val) {}

    /// Default-construct an sxField
    explicit sxField() : wb_c1_field<T, wb_c1_t>() {}
  };

private:
  friend STM;
  friend ROSTM;
  friend WOSTM;
  friend STEP;
  friend WSTEP;
  friend RSTEP;
  template <typename T, typename DESCRIPTOR> friend class field_base_t;
  template <typename T, typename DESCRIPTOR> friend class wb_c1_field;
};
