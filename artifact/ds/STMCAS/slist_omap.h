#pragma once

#include <atomic>
#include <type_traits>

/// An ordered map, implemented as a singly-linked list.  This map supports
/// get(), insert(), and remove() operations.
///
/// Note that the AVOID_OREC_CHECKS flag can be used to create an "optimized"
/// version of this data structure, where list traversal (get_leq) avoids
/// checking orecs in most cases.
///
/// @param K                 The type of the keys stored in this map
/// @param V                 The type of the values stored in this map
/// @param STMCAS            The STMCAS implementation (PO or PS)
/// @param AVOID_OREC_CHECKS A flag to enable an optimization that avoids
///                          checking orecs when get_leq is doing its read-only
///                          traversal
template <typename K, typename V, class STMCAS, bool AVOID_OREC_CHECKS>
class slist_omap {
  using WSTEP = typename STMCAS::WSTEP;
  using RSTEP = typename STMCAS::RSTEP;
  using snapshot_t = typename STMCAS::snapshot_t;
  using ownable_t = typename STMCAS::ownable_t;
  template <typename T> using FIELD = typename STMCAS::template sField<T>;

  /// A list node.  It has a next pointer, but no key or value.  It's useful for
  /// sentinels, so that K and V don't have to be default constructable.
  struct node_t : ownable_t {
    FIELD<node_t *> next; // Pointer to successor

    /// Construct a node
    node_t() : ownable_t(), next(nullptr) {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~node_t() {}
  };

  /// A list node that also has a key and value.  Note that keys are const, and
  /// values are only accessed while the node is locked, so neither is a
  /// tm_field.
  struct data_t : public node_t {
    const K key; // The key of this key/value pair
    V val;       // The value of this key/value pair

    /// Construct a data_t
    ///
    /// @param _key         The key that is stored in this node
    /// @param _val         The value that is stored in this node
    data_t(const K &_key, const V &_val) : node_t(), key(_key), val(_val) {}
  };

  /// The pair returned by predecessor queries: a node and it's observed version
  struct leq_t {
    node_t *_obj = nullptr; // The object
    uint64_t _ver = 0;      // The observed version of the object
  };

  node_t *const head; // The list head pointer
  node_t *const tail; // The list tail pointer

  /// During get_leq, we have a way to periodically capture snapshots, so that a
  /// failed search can resume from an intermediate point.  This specifies how
  /// frequently to take a snapshot (higher is less frequent, i.e., once per
  /// SNAPSHOT_FREQUENCY nodes).
  const int SNAPSHOT_FREQUENCY;

public:
  /// Default construct a list by constructing and connecting two sentinel nodes
  ///
  /// @param me  The operation that is constructing the list
  /// @param cfg A configuration object that has a `snapshot_freq` field
  slist_omap(STMCAS *me, auto *cfg)
      : head(new node_t()), tail(new node_t()),
        SNAPSHOT_FREQUENCY(cfg->snapshot_freq) {
    // NB: Even though this code can't abort and doesn't acquire orecs, we still
    //     need to use a transaction (WSTEP), because we can't set fields of a
    //     node_t without a legal WSTEP context.  We can cheat, though, and not
    //     bother to acquire orecs, because we know nothing is shared.
    WSTEP tx(me);
    head->next.set(tail, tx);
  }

private:
  /// Convert a snapshot_t into a leq_t
  leq_t leq(const snapshot_t &s) { return leq_t{(node_t *)s._obj, s._ver}; }

  /// Convert a leq_t into a snapshot_t
  snapshot_t snapshot(const leq_t &l) { return snapshot_t{l._obj, l._ver}; }

  /// get_leq is an inclusive predecessor query that returns the largest node
  /// whose key is <= the provided key.  It can return the head sentinel, but
  /// not the tail sentinel.
  ///
  /// There is no atomicity between get_leq and its caller.  It returns the node
  /// it found, along with the value of the orec for that node at the time it
  /// was accessed.  The caller needs to validate the orec before using the
  /// returned node.
  ///
  /// @param me      The calling thread's descriptor
  /// @param key     The key for which we are doing a predecessor query.
  /// @param lt_mode When `true`, this behaves as `get_lt`.  When `false`, it
  ///                behaves as `get_leq`.
  ///
  /// @return The node that was found, and its orec value
  leq_t get_leq(STMCAS *me, const K key, bool lt_mode = false) {
    // Start a transactional traversal from the head node, or from the latest
    // valid snapshot, if we have one. If a transaction encounters an
    // inconsistency, it will come back to here to start a new traversal.
    while (true) {
      RSTEP tx(me);

      // Figure out where to start this traversal: initially we start at head,
      // but on a retry, we might have a snapshot.
      //
      // NB: snapshots are always < key
      leq_t curr =
          (me->snapshots.empty()) ? leq_t{head, 0} : leq(me->snapshots.top());

      // Validate the start point
      if (curr._obj == head) {
        // For head, be sure to save curr._ver in case we end up returning head
        if ((curr._ver = tx.check_orec(curr._obj)) == STMCAS::END_OF_TIME)
          continue;
      } else {
        // Validate snapshot as a continuation.  Drop the snapshot on failure.
        if (!tx.check_continuation(curr._obj, curr._ver)) {
          me->snapshots.drop();
          continue;
        }
      }

      // Prepare a countdown timer for snapshots
      int nodes_until_snapshot = SNAPSHOT_FREQUENCY;

      // Starting at `next`, search for key.  Breaking out of this will take us
      // back to the top of the function.
      while (true) {
        // Read the next node, fail if we can't do it consistently
        auto *next = curr._obj->next.get(tx);
        uint64_t next_ver = 0;
        if (!AVOID_OREC_CHECKS) {
          next_ver = tx.check_orec(next);
          if (next_ver == STMCAS::END_OF_TIME)
            break;
        }

        // Stop if next's key is too big or next is tail
        if (next == tail) {
          if (AVOID_OREC_CHECKS &&
              (curr._ver = tx.check_orec(curr._obj)) == STMCAS::END_OF_TIME)
            break;
          return curr;
        }
        data_t *dn = static_cast<data_t *>(next);
        if (lt_mode ? dn->key >= key : dn->key > key) {
          if (AVOID_OREC_CHECKS &&
              (curr._ver = tx.check_orec(curr._obj)) == STMCAS::END_OF_TIME)
            break;
          return curr;
        }

        // Stop if `next` is the match we were hoping for
        if (dn->key == key) {
          if (AVOID_OREC_CHECKS) {
            next_ver = tx.check_orec(next);
            if (next_ver == STMCAS::END_OF_TIME)
              break;
          }
          return {next, next_ver};
        }

        // Keep traversing to `next`.  Maybe take a snapshot first
        if (--nodes_until_snapshot == 0) {
          // if (AVOID_OREC_CHECKS &&
          //     (curr._ver = tx.check_orec(curr._obj)) == STMCAS::END_OF_TIME)
          //   break;
          // me->snapshots.push_back(snapshot(curr));
          if (AVOID_OREC_CHECKS) {
            if ((curr._ver = tx.check_orec(curr._obj)) != STMCAS::END_OF_TIME)
              me->snapshots.push_back(snapshot(curr));
          } else
            me->snapshots.push_back(snapshot(curr));
          nodes_until_snapshot = SNAPSHOT_FREQUENCY;
        }
        curr._obj = next;
        if (!AVOID_OREC_CHECKS)
          curr._ver = next_ver;
      }
    }
  }

public:
  /// Search the data structure for a node with key `key`.  If not found, return
  /// false.  If found, return true, and set `val` to the value associated with
  /// `key`.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key to search
  /// @param val A ref parameter for returning key's value, if found
  ///
  /// @return True if the key is found, false otherwise.  The reference
  ///         parameter `val` is only valid when the return value is true.
  bool get(STMCAS *me, const K &key, V &val) {
    // If we can't use the result of get_leq, we'll loop back, and the next
    // get_leq will start from a snapshot
    me->snapshots.clear();
    while (true) {
      // get_leq will use a read-only transaction to find the largest node with
      // a key <= `key`.
      //
      // Postconditions of get_leq: n != null, n != tail, we have a valid
      // node/version pair, and n.key <= `key`
      auto n = get_leq(me, key);

      // Since we have EBR, we can read n.key without validating and fast-fail
      // on key-not-found
      if (n._obj == head || static_cast<data_t *>(n._obj)->key != key)
        return false;

      // Use a hand-over-hand TM pattern to finish the get().  If the value is
      // scalar, we can cast it to atomic, read it, and validate.  Otherwise we
      // need to lock the node.
      if (std::is_scalar<V>::value) {
        RSTEP tx(me);

        // NB: given EBR, we don't need to worry about n._obj being deleted, so
        //     we don't need to validate before looking at the value
        data_t *dn = static_cast<data_t *>(n._obj);
        V val_copy = reinterpret_cast<std::atomic<V> *>(&dn->val)->load(
            std::memory_order_acquire);
        if (!tx.check_continuation(n._obj, n._ver))
          continue;
        val = val_copy;
        return true;
      } else {
        WSTEP tx(me);

        // If this acquire continuation succeeds, it's not deleted, it's a data
        // node, and it's valid.  If it fails, we need to restart
        if (!tx.acquire_continuation(n._obj, n._ver)) {
          tx.unwind(); // not strictly needed, but a good habit :)
          continue;
        }

        // NB: we aren't changing val, so we can unwind when we're done with it
        val = static_cast<data_t *>(n._obj)->val;
        tx.unwind();
        return true;
      }
    }
  }

  /// Create a mapping from the provided `key` to the provided `val`, but only
  /// if no such mapping already exists.  This method does *not* have upsert
  /// behavior for keys already present.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for the mapping to create
  /// @param val The value for the mapping to create
  ///
  /// @return True if the value was inserted, false otherwise.
  bool insert(STMCAS *me, const K &key, V &val) {
    // NB: The pattern here is similar to `get`
    me->snapshots.clear();
    while (true) {
      auto n = get_leq(me, key);

      // Since we have EBR, we can look at n._obj->key without validation.  If
      // it matches `key`, return false.
      if (n._obj != head && static_cast<data_t *>(n._obj)->key == key)
        return false;

      // either n._obj is `head`, or it's a key that's too small.  Let's insert!
      WSTEP tx(me);
      // lock n, fail if we can't get it
      if (!tx.acquire_continuation(n._obj, n._ver)) {
        tx.unwind();
        continue;
      }

      // stitch in a new node
      data_t *new_dn = new data_t(key, val);
      new_dn->next.set(n._obj->next.get(tx), tx);
      n._obj->next.set(new_dn, tx);
      return true;
    }
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(STMCAS *me, const K &key) {
    // NB: The pattern here is similar to `get`
    me->snapshots.clear();
    while (true) {
      // NB: this will be a lt query, not a leq query
      auto prev = get_leq(me, key, true);

      WSTEP tx(me);
      // lock the predecessor, read its next
      if (!tx.acquire_continuation(prev._obj, prev._ver)) {
        tx.unwind();
        continue;
      }
      auto curr = prev._obj->next.get(tx);

      // if curr doesn't have a matching key, fail
      if (curr == tail || static_cast<data_t *>(curr)->key != key) {
        tx.unwind();
        return false;
      }

      // lock the node to remove, then unstitch it
      if (!tx.acquire_aggressive(curr)) {
        tx.unwind();
        continue;
      }
      auto next = curr->next.get(tx);
      prev._obj->next.set(next, tx);
      tx.reclaim(curr);
      return true;
    }
  }
};
