#pragma once

#include "include/base.h"
#include "include/field.h"
#include "include/raii.h"

/// lazy_t is a hybrid HandSTM+STMCAS policy:
/// - Uses ExoTM for orecs and rdtsc clock
/// - Check-once orecs
/// - Commit-time locking with redo
/// - No quiescence, but safe memory reclamation
/// - Can be configured with per-object or per-stripe orecs
///
/// @tparam OP The orec policy to use.
template <template <typename, typename> typename OP>
struct lazy_t : public base_t<OP> {
  using STM = Stm<lazy_t>;     // RAII ROSTM/WOSTM base
  using ROSTM = RoStm<lazy_t>; // RAII ROSTM manager
  using WOSTM = WoStm<lazy_t>; // RAII WOSTM manager
  using STEP = Step<lazy_t>;   // RAII RSTEP/WSTEP base
  using RSTEP = RStep<lazy_t>; // RAII RSTEP manager
  using WSTEP = WStep<lazy_t>; // RAII WSTEP manager

  /// Construct a lazy_t
  lazy_t() : base_t<OP>() {}

private:
  // Types needed by the (friend) field template, but not worth making public
  using OWNABLE = typename base_t<OP>::ownable_t;
  static const auto EOT = exotm_t::END_OF_TIME;

public:
  /// The type for fields that are shared and protected by HyPol
  template <typename T> struct sxField : public lazy_field<T, lazy_t> {
    /// Construct an sxField
    ///
    /// @param val The initial value
    explicit sxField(T val) : lazy_field<T, lazy_t>(val) {}

    /// Default-construct an sxField
    explicit sxField() : lazy_field<T, lazy_t>() {}
  };

private:
  friend STM;
  friend ROSTM;
  friend WOSTM;
  friend STEP;
  friend WSTEP;
  friend RSTEP;
  template <typename T, typename DESCRIPTOR> friend class field_base_t;
  template <typename T, typename DESCRIPTOR> friend class lazy_field;
};
