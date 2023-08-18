#pragma once

#include <atomic>

/// field_base_t has the code that is shared among all of our HandSTM policies'
/// field implementations
///
/// @tparam T          The type that is tored in this field_base_t
/// @tparam DESCRIPTOR The HandSTM policy descriptor type
template <typename T, typename DESCRIPTOR> class field_base_t {
protected:
  T _val; // The value.  It's going to be cast to atomic by the undolog.

  /// Construct a field_base_t
  ///
  /// @param val The initial value
  explicit field_base_t(T val) : _val(val) {}

  /// Default-construct a field_base_t
  explicit field_base_t() : _val() {}

  /// OP is a workaround so that all of the different field types do not need to
  /// be friends of the RAII objects
  DESCRIPTOR &OP(typename DESCRIPTOR::STM &tx) { return *tx.op; }

public:
  /// Read from shared memory (middle read in a sequence of reads of `o` by
  /// `tx`, without any control flow / computed addresses)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T xGet_in_seq(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    // Check log first
    T ret;
    if (tx.op->redolog.get(&this->_val, ret))
      return ret;

    // read the location.  It'll get validated later
    return tx.op->redolog.safe_read(&this->_val);
  }

  /// Write to shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void xSet_mine(typename DESCRIPTOR::WOSTM &tx,
                 typename DESCRIPTOR::OWNABLE *o, T val) {
    tx.op->redolog.insert(&this->_val, val);
  }

  /// Write to shared memory (captured / not-actually-shared memory)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void xSet_cap(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
                T val) {
    this->_val = val;
  }

  /// Read the sField from a RSTEP or WSTEP.  The caller is responsible for
  /// validating the orec.
  ///
  /// @param STEP An unused parameter to restrict access to *STEP contexts
  ///
  /// @return The current value
  T sGet(typename DESCRIPTOR::STEP &) {
    // NB: we're only concerned about x86 for now, so we'll just use
    //     memory_order_acquire.  On ARM, we would want to use a relaxed read,
    //     and have a thread fence later on (i.e., before the validate).
    //
    // NB: We don't specialize to RO_TM or WO_TM, because sometimes a WO_TM
    //     will read without owning an object, and thus won't be able to do a
    //     memory_order_relaxed read.
    return std::atomic_ref(this->_val).load(std::memory_order_acquire);
  }

  /// Write the sField from a WSTEP.  The caller must ensure the corresponding
  /// orec is owned before calling this.
  ///
  /// NB: memory_order_relaxed, because we assume it is owned
  ///
  /// @param val   The new value
  /// @param WSTEP An unused parameter to restrict access to WSTEP contexts
  void sSet(T val, typename DESCRIPTOR::WSTEP &) {
    std::atomic_ref(this->_val).store(val, std::memory_order_relaxed);
  }
};

/// lazy_field is a wrapper around simple types so that they can only be
/// accessed via STMCAS steps or HandSTM transactions.
///
/// NB: When using STMCAS, the programmer has the same responsibilities as in
///     regular STMCAS.
///
/// @tparam T          The type that is stored in this field
/// @tparam DESCRIPTOR The type of HyPol policy using this field
template <typename T, typename DESCRIPTOR>
class lazy_field : public field_base_t<T, DESCRIPTOR> {
  // A helper for bouncing through the base_t to get an op's descriptor
  DESCRIPTOR &op(typename DESCRIPTOR::STM &tx) {
    return field_base_t<T, DESCRIPTOR>::OP(tx);
  }

public:
  /// Construct a lazy_field
  ///
  /// @param val The initial value
  explicit lazy_field(T val) : field_base_t<T, DESCRIPTOR>(val) {}

  /// Default-construct a lazy_field
  explicit lazy_field() : field_base_t<T, DESCRIPTOR>() {}

  /// Read from shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  ///
  /// TODO: Throughout HandSTM and HyPol, we should specialize get() for RO
  /// versus WO, because RO never needs to do the costly lookups.
  T xGet(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    // Lookup in redo log
    T ret;
    if (op(tx).redolog.get(&this->_val, ret))
      return ret;

    // Start a loop to read a consistent value
    while (true) {
      // read the location, then orec
      ret = op(tx).redolog.safe_read(&this->_val);

      // If validation passes, then we can log it and return it
      bool locked = false;
      if (op(tx).exo.check_orec(o->orec(), locked) != DESCRIPTOR::EOT) {
        op(tx).readset.push_back(o->orec());
        return ret;
      }

      // wait if locked
      while (locked)
        op(tx).exo.check_orec(o->orec(), locked);

      // Extend the validity range, then try again
      auto old_start = op(tx).exo.get_start_time();
      op(tx).exo.wo_begin();
      op(tx).validate(old_start);
    }
  }

  /// Read from shared memory (guaranteed not the first read to `o` by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T re_xGet(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    // Check log first
    T ret;
    if (op(tx).redolog.get(&this->_val, ret))
      return ret;

    // read the location, then orec.  If validation fails, we must abort.
    ret = op(tx).redolog.safe_read(&this->_val);
    if (op(tx).exo.check_orec(o->orec()) == DESCRIPTOR::EOT)
      op(tx).abort();
    return ret;
  }

  /// Read from shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T xGet_mine(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o) {
    // NB: in lazy, we *never* own the orec, so there's no optimization here.
    return xGet(tx, o);
  }

  /// Write to shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void xSet(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
            T val) {
    op(tx).lockset.push_back(o->orec());
    op(tx).redolog.insert(&this->_val, val);
  }
};

/// wb_c1_field is a wrapper around simple types so that they can only be
/// accessed via STMCAS steps or HandSTM transactions.
///
/// NB: When using STMCAS, the programmer has the same responsibilities as in
/// regular STMCAS.
///
/// @tparam T          The type that is stored in this field
/// @tparam DESCRIPTOR The type of HyPol policy using this field
template <typename T, typename DESCRIPTOR>
class wb_c1_field : public field_base_t<T, DESCRIPTOR> {
  // A helper for bouncing through the base_t to get an op's descriptor
  DESCRIPTOR &op(typename DESCRIPTOR::STM &tx) {
    return field_base_t<T, DESCRIPTOR>::OP(tx);
  }

public:
  /// Construct a wb_c1_field
  ///
  /// @param val The initial value
  explicit wb_c1_field(T val) : field_base_t<T, DESCRIPTOR>(val) {}

  /// Default-construct a wb_c1_field
  explicit wb_c1_field() : field_base_t<T, DESCRIPTOR>() {}

  /// Read from shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  ///
  /// TODO: Throughout HandSTM and HyPol, we should specialize get() for RO
  /// versus WO, because RO never needs to do the costly lookups.
  T xGet(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    // Since this is check-once orecs, we can't really use the lock state to
    // avoid redo log checks, so let's just check right away:
    T ret;
    if (op(tx).redolog.get(&this->_val, ret))
      return ret;

    // Start a loop to read a consistent value
    while (true) {
      // read the location, then orec
      ret = op(tx).redolog.safe_read(&this->_val);

      // If validation passes, then (maybe) log it and return it
      bool locked = false;
      if (op(tx).exo.check_orec(o->orec(), locked) != exotm_t::END_OF_TIME) {
        // Don't log the read if `tx` owns it
        if (!locked)
          op(tx).readset.push_back(o->orec());
        return ret;
      }

      // abort if locked
      while (locked)
        op(tx).abort();

      // Extend the validity range, then try again
      auto old_start = op(tx).exo.get_start_time();
      op(tx).exo.wo_begin();
      op(tx).validate(old_start);
    }
  }

  /// Read from shared memory (guaranteed not the first read to `o` by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T re_xGet(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    // Check log first
    T ret;
    if (op(tx).redolog.get(&this->_val, ret))
      return ret;

    // read the location, then orec.  If validation fails, we must abort.
    ret = op(tx).redolog.safe_read(&this->_val);
    if (op(tx).exo.check_orec(o->orec()) == exotm_t::END_OF_TIME)
      op(tx).abort();
    return ret;
  }

  /// Read from shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T xGet_mine(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o) {
    T ret;
    if (op(tx).redolog.get(&this->_val, ret))
      return ret;
    return op(tx).redolog.safe_read(&this->_val);
  }

  /// Write to shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void xSet(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
            T val) {
    // Put it in the redo log right away
    op(tx).redolog.insert(&this->_val, val);

    // Now either consistently get the lock, or else abort
    while (true) {
      // If I have it or can get it, that's the easy case
      bool locked = false;
      if (op(tx).exo.acquire_consistent(o->orec(), locked))
        return;

      // abort if locked
      if (locked)
        op(tx).abort();

      // Extend the validity range, then try again
      auto old_start = op(tx).exo.get_start_time();
      op(tx).exo.wo_begin();
      op(tx).validate(old_start);
    }
  }
};

/// wb_c2_field is a wrapper around simple types so that they can only be
/// accessed via STMCAS steps or HandSTM transactions.
///
/// NB: When using STMCAS, the programmer has the same responsibilities as in
/// regular STMCAS.
///
/// @tparam T          The type that is stored in this field
/// @tparam DESCRIPTOR The type of HyPol policy using this field
template <typename T, typename DESCRIPTOR>
class wb_c2_field : public field_base_t<T, DESCRIPTOR> {
  // A helper for bouncing through the base_t to get an op's descriptor
  DESCRIPTOR &op(typename DESCRIPTOR::STM &tx) {
    return field_base_t<T, DESCRIPTOR>::OP(tx);
  }

public:
  /// Construct a wb_c2_field
  ///
  /// @param val The initial value
  explicit wb_c2_field(T val) : field_base_t<T, DESCRIPTOR>(val) {}

  /// Default-construct a wb_c2_field
  explicit wb_c2_field() : field_base_t<T, DESCRIPTOR>() {}

  /// Read from shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  ///
  /// TODO: Throughout HandSTM and HyPol, we should specialize get() for RO
  /// versus WO, because RO never needs to do the costly lookups.
  T xGet(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    while (true) {
      T ret;
      // Pre-check the orec, and record if it's locked
      bool locked = false;
      auto pre = op(tx).exo.check_orec(o->orec(), locked);
      // If tx owns `o`, try the redo log, else return directly from memory
      // without logging or double-checking the orec
      if (pre != exotm_t::END_OF_TIME && locked) {
        if (op(tx).redolog.get(&this->_val, ret))
          return ret;
        else
          return op(tx).redolog.safe_read(&this->_val);
      }
      ret = op(tx).redolog.safe_read(&this->_val);
      auto post = op(tx).exo.check_orec(o->orec());
      // If validation passes, then we can log it and return
      if (pre == post && pre != exotm_t::END_OF_TIME) {
        op(tx).readset.push_back(o->orec());
        return ret;
      }

      // abort if locked
      if (locked)
        op(tx).abort();

      // Extend the validity range, then try again
      auto old_start = op(tx).exo.get_start_time();
      op(tx).exo.wo_begin();
      op(tx).validate(old_start);
    }
  }

  /// Read from shared memory (guaranteed not the first read to `o` by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T re_xGet(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    T ret;
    // Pre-check the orec, and record if it's locked
    bool locked = false;
    auto pre = op(tx).exo.check_orec(o->orec(), locked);
    // If tx owns `o`, try the redo log, else return directly from memory
    // without logging or double-checking the orec
    if (pre != exotm_t::END_OF_TIME && locked) {
      if (op(tx).redolog.get(&this->_val, ret))
        return ret;
      else
        return op(tx).redolog.safe_read(&this->_val);
    }
    ret = op(tx).redolog.safe_read(&this->_val);
    auto post = op(tx).exo.check_orec(o->orec());
    // If validation fails, we should just abort, because it's unlikely that
    // whoever owns it will abort and reset the orec to the value we logged in
    // a prior get().
    if (pre != post || pre == exotm_t::END_OF_TIME)
      op(tx).abort();
    // Validation succeeded, so return the value without logging
    return ret;
  }

  /// Read from shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T xGet_mine(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o) {
    T ret;
    if (op(tx).redolog.get(&this->_val, ret))
      return ret;
    return op(tx).redolog.safe_read(&this->_val);
  }

  /// Write to shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void xSet(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
            T val) {
    // Put it in the redo log right away
    op(tx).redolog.insert(&this->_val, val);

    while (true) {
      // If I have it or can get it, that's the easy case
      bool locked = false;
      if (op(tx).exo.acquire_consistent(o->orec(), locked))
        return;

      // abort if locked
      if (locked)
        op(tx).abort();

      // Extend the validity range, then try again
      auto old_start = op(tx).exo.get_start_time();
      op(tx).exo.wo_begin();
      op(tx).validate(old_start);
    }
  }
};
