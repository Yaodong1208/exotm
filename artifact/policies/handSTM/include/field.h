#pragma once

#include <cstdint>

/// field_base_t has the code that is shared among all of our HandSTM policies'
/// field implementations
///
/// @tparam T          The type that is tored in this field_base_t
/// @tparam DESCRIPTOR The type of HandSTM policy using this field
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
  /// Write to shared memory (captured / not-actually-shared memory)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set_cap(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
               T val) {
    _val = val;
  }
};

/// eager_field_t extends field_base_t with code that is shared among the eager
/// HandSTM policies' field implementations
///
/// @tparam T          The type that is stored in this eager_c1_field @tparam
/// @tparam DESCRIPTOR The type of HandSTM policy using this field
template <typename T, typename DESCRIPTOR>
class eager_field_t : public field_base_t<T, DESCRIPTOR> {
  // A helper for bouncing through the base_t to get an op's descriptor
  DESCRIPTOR &op(typename DESCRIPTOR::STM &tx) {
    return field_base_t<T, DESCRIPTOR>::OP(tx);
  }

protected:
  /// Construct an eager_c1_field
  ///
  /// @param val The initial value
  explicit eager_field_t(T val) : field_base_t<T, DESCRIPTOR>(val) {}

  /// Default-construct an eager_c1_field
  explicit eager_field_t() : field_base_t<T, DESCRIPTOR>() {}

public:
  /// Read from shared memory (middle read in a sequence of reads of `o` by
  /// `tx`, without any control flow / computed addresses)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get_in_seq(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    return op(tx).undolog.safe_read(&this->_val);
  }

  /// Read from shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get_mine(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o) {
    return op(tx).undolog.safe_read(&this->_val);
  }

  /// Write to shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
           T val) {
    while (true) {
      // If I have it or can get it, that's the easy case
      bool locked = false;
      if (op(tx).exo.acquire_consistent(o->orec(), locked)) {
        typename DESCRIPTOR::UNDO_T u;
        u.initFromAddr(&this->_val);
        op(tx).undolog.push_back(u);
        op(tx).undolog.safe_write(&this->_val, val);
        return;
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

  /// Write to shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set_mine(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
                T val) {
    typename DESCRIPTOR::UNDO_T u;
    u.initFromAddr(&this->_val);
    op(tx).undolog.push_back(u);
    op(tx).undolog.safe_write(&this->_val, val);
  }
};

/// eager_c1_field is a wrapper around simple types so that they can only be
/// accessed via HandSTM.
///
/// @tparam T          The type that is stored in this eager_c1_field
/// @tparam DESCRIPTOR The type of HandSTM policy using this field
template <typename T, typename DESCRIPTOR>
class eager_c1_field : public eager_field_t<T, DESCRIPTOR> {
  // A helper for bouncing through the base_t to get an op's descriptor
  DESCRIPTOR &op(typename DESCRIPTOR::STM &tx) {
    return field_base_t<T, DESCRIPTOR>::OP(tx);
  }

public:
  /// Construct an eager_c1_field
  ///
  /// @param val The initial value
  explicit eager_c1_field(T val) : eager_field_t<T, DESCRIPTOR>(val) {}

  /// Default-construct an eager_c1_field
  explicit eager_c1_field() : eager_field_t<T, DESCRIPTOR>() {}

  /// Read from shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    while (true) {
      // read the location, then orec
      T from_mem = op(tx).undolog.safe_read(&this->_val);
      bool locked = false;
      auto post = op(tx).exo.check_orec(o->orec(), locked);
      // If validation passes, then we can log it and return
      if (post != DESCRIPTOR::EOT) {
        if (!locked)
          op(tx).readset.push_back(o->orec());
        return from_mem;
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
  T re_get(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    // read the location, then orec
    T from_mem = op(tx).undolog.safe_read(&this->_val);
    // If validation fails, abort, else return the value without logging
    if (op(tx).exo.check_orec(o->orec()) == DESCRIPTOR::EOT)
      op(tx).abort();
    return from_mem;
  }
};

/// eager_c2_field is a wrapper around simple types so that they can only be
/// accessed via HandSTM.
///
/// @tparam T          The type that is stored in this eager_c1_field
/// @tparam DESCRIPTOR The type of HandSTM policy using this field
template <typename T, typename DESCRIPTOR>
class eager_c2_field : public eager_field_t<T, DESCRIPTOR> {
  // A helper for bouncing through the base_t to get an op's descriptor
  DESCRIPTOR &op(typename DESCRIPTOR::STM &tx) {
    return field_base_t<T, DESCRIPTOR>::OP(tx);
  }

public:
  /// Construct an eager_c2_field
  ///
  /// @param val The initial value
  explicit eager_c2_field(T val) : eager_field_t<T, DESCRIPTOR>(val) {}

  /// Default-construct an eager_c2_field
  explicit eager_c2_field() : eager_field_t<T, DESCRIPTOR>() {}

  /// Read from shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    while (true) {
      // Pre-check the orec, and record if it's locked
      bool locked = false;
      auto pre = op(tx).exo.check_orec(o->orec(), locked);
      // read the location, then orec
      T from_mem = op(tx).undolog.safe_read(&this->_val);
      // return if location is owned by me
      if (locked && pre != DESCRIPTOR::EOT)
        return from_mem; // owned by me: don't need another check
      auto post = op(tx).exo.check_orec(o->orec());
      // If validation passes, then we can log it and return
      if (pre == post && pre != DESCRIPTOR::EOT) {
        op(tx).readset.push_back(o->orec());
        return from_mem;
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
  T re_get(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    // Pre-check the orec, and record if it's locked
    bool locked = false;
    auto pre = op(tx).exo.check_orec(o->orec(), locked);
    // read the location, then orec
    T from_mem = op(tx).undolog.safe_read(&this->_val);
    // return if location is owned by me
    if (locked && pre != DESCRIPTOR::EOT)
      return from_mem; // owned by me: don't need another check
    auto post = op(tx).exo.check_orec(o->orec());
    // If validation fails, then even on a transient pre/post issue, orec
    // bumps are going to cause us to abort in validation, so just abort now
    if (pre != post || pre == DESCRIPTOR::EOT)
      op(tx).abort();
    // Validation succeeded, so return the value without logging
    return from_mem;
  }
};

/// lazy_field is a wrapper around simple types so that they can only be
/// accessed via HandSTM.
///
/// @tparam T          The type that is stored in this eager_c1_field
/// @tparam DESCRIPTOR The type of HandSTM policy using this field
template <typename T, typename DESCRIPTOR>
class lazy_field : public field_base_t<T, DESCRIPTOR> {
  // A helper for bouncing through the base_t to get an op's descriptor
  DESCRIPTOR &op(typename DESCRIPTOR::STM &tx) {
    return field_base_t<T, DESCRIPTOR>::OP(tx);
  }

public:
  /// Construct an lazy_field
  ///
  /// @param val The initial value
  explicit lazy_field(T val) : field_base_t<T, DESCRIPTOR>(val) {}

  /// Default-construct an lazy_field
  explicit lazy_field() : field_base_t<T, DESCRIPTOR>() {}

  /// Read from shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
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
  T re_get(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
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

  /// Read from shared memory (middle read in a sequence of reads of `o` by
  /// `tx`, without any control flow / computed addresses)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get_in_seq(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    // Check log first
    T ret;
    if (op(tx).redolog.get(&this->_val, ret))
      return ret;

    // read the location, then orec.  If validation fails, we must abort.
    return op(tx).redolog.safe_read(&this->_val);
  }

  /// Read from shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get_mine(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o) {
    // NB: in lazy, we *never* own the orec, so there's no optimization here.
    return get(tx, o);
  }

  /// Write to shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
           T val) {
    op(tx).lockset.push_back(o->orec());
    op(tx).redolog.insert(&this->_val, val);
  }

  /// Write to shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set_mine(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
                T val) {
    op(tx).redolog.insert(&this->_val, val);
  }
};

/// wb_field_t is a wrapper around simple types so that they can only be
/// accessed via HandSTM.
///
/// @tparam T          The type that is stored in this eager_c1_field
/// @tparam DESCRIPTOR The type of HandSTM policy using this field
template <typename T, typename DESCRIPTOR>
class wb_field_t : public field_base_t<T, DESCRIPTOR> {
  // A helper for bouncing through the base_t to get an op's descriptor
  DESCRIPTOR &op(typename DESCRIPTOR::STM &tx) {
    return field_base_t<T, DESCRIPTOR>::OP(tx);
  }

public:
  /// Construct a wb_field_t
  ///
  /// @param val The initial value
  explicit wb_field_t(T val) : field_base_t<T, DESCRIPTOR>(val) {}

  /// Default-construct a wb_field_t
  explicit wb_field_t() : field_base_t<T, DESCRIPTOR>() {}

  /// Read from shared memory (middle read in a sequence of reads of `o` by
  /// `tx`, without any control flow / computed addresses)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get_in_seq(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    T ret;
    if (op(tx).redolog.get(&this->_val, ret))
      return ret;
    return op(tx).redolog.safe_read(&this->_val);
  }

  /// Read from shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get_mine(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o) {
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
  void set(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
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

  /// Write to shared memory (`o` is owned by `tx`)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  /// @param val The new value
  void set_mine(typename DESCRIPTOR::WOSTM &tx, typename DESCRIPTOR::OWNABLE *o,
                T val) {
    op(tx).redolog.insert(&this->_val, val);
  }
};

/// wb_c1_field is a wrapper around simple types so that they can only be
/// accessed via HandSTM.
///
/// @tparam T          The type that is stored in this eager_c1_field
/// @tparam DESCRIPTOR The type of HandSTM policy using this field
template <typename T, typename DESCRIPTOR>
class wb_c1_field : public wb_field_t<T, DESCRIPTOR> {
  // A helper for bouncing through the base_t to get an op's descriptor
  DESCRIPTOR &op(typename DESCRIPTOR::STM &tx) {
    return field_base_t<T, DESCRIPTOR>::OP(tx);
  }

public:
  /// Construct a wb_c1_field
  ///
  /// @param val The initial value
  explicit wb_c1_field(T val) : wb_field_t<T, DESCRIPTOR>(val) {}

  /// Default-construct a wb_c1_field
  explicit wb_c1_field() : wb_field_t<T, DESCRIPTOR>() {}

  /// Read from shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
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
      if (op(tx).exo.check_orec(o->orec(), locked) != DESCRIPTOR::EOT) {
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
  T re_get(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
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
};

/// wb_c2_field is a wrapper around simple types so that they can only be
/// accessed via HandSTM.
///
/// @tparam T          The type that is stored in this eager_c1_field
/// @tparam DESCRIPTOR The type of HandSTM policy using this field
template <typename T, typename DESCRIPTOR>
class wb_c2_field : public wb_field_t<T, DESCRIPTOR> {
  // A helper for bouncing through the base_t to get an op's descriptor
  DESCRIPTOR &op(typename DESCRIPTOR::STM &tx) {
    return field_base_t<T, DESCRIPTOR>::OP(tx);
  }

public:
  /// Construct a wb_c2_field
  ///
  /// @param val The initial value
  explicit wb_c2_field(T val) : wb_field_t<T, DESCRIPTOR>(val) {}

  /// Default-construct a wb_c2_field
  explicit wb_c2_field() : wb_field_t<T, DESCRIPTOR>() {}

  /// Read from shared memory (general-purpose version)
  ///
  /// @param tx  The transaction performing this read
  /// @param o   The ownable for this location (locates the orec)
  ///
  /// @return The current value
  T get(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    while (true) {
      T ret;
      // Pre-check the orec, and record if it's locked
      bool locked = false;
      auto pre = op(tx).exo.check_orec(o->orec(), locked);
      // If tx owns `o`, try the redo log, else return directly from memory
      // without logging or double-checking the orec
      if (pre != DESCRIPTOR::EOT && locked) {
        if (op(tx).redolog.get(&this->_val, ret))
          return ret;
        else
          return op(tx).redolog.safe_read(&this->_val);
      }
      ret = op(tx).redolog.safe_read(&this->_val);
      auto post = op(tx).exo.check_orec(o->orec());
      // If validation passes, then we can log it and return
      if (pre == post && pre != DESCRIPTOR::EOT) {
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
  T re_get(typename DESCRIPTOR::STM &tx, typename DESCRIPTOR::OWNABLE *o) {
    T ret;
    // Pre-check the orec, and record if it's locked
    bool locked = false;
    auto pre = op(tx).exo.check_orec(o->orec(), locked);
    // If tx owns `o`, try the redo log, else return directly from memory
    // without logging or double-checking the orec
    if (pre != DESCRIPTOR::EOT && locked) {
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
    if (pre != post || pre == DESCRIPTOR::EOT)
      op(tx).abort();
    // Validation succeeded, so return the value without logging
    return ret;
  }
};