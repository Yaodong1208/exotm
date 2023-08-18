#pragma once

/// An ordered map, implemented as a singly-linked list.  This map supports
/// get(), insert(), and remove() operations.
///
/// @param K       The type of the keys stored in this map
/// @param V       The type of the values stored in this map
/// @param HANDSTM A thread descriptor type, for safe memory reclamation
template <typename K, typename V, class HANDSTM> class slist_omap {
  using WOSTM = typename HANDSTM::WOSTM;
  using ROSTM = typename HANDSTM::ROSTM;
  using STM = typename HANDSTM::STM;
  using ownable_t = typename HANDSTM::ownable_t;
  template <typename T> using FIELD = typename HANDSTM::template xField<T>;

  /// A list node.  It has a next pointer, but no key or value.  It's useful for
  /// sentinels, so that K and V don't have to be default constructable.
  struct node_t : ownable_t {
    FIELD<node_t *> next; // Pointer to successor

    /// Construct a node
    node_t() : next(nullptr) {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~node_t() {}
  };

  /// A list node that also has a key and value.  Note that keys are const.
  struct data_t : public node_t {
    const K key;  // The key of this pair
    FIELD<V> val; // The value of this  pair

    /// Construct a data_t
    ///
    /// @param _key         The key that is stored in this node
    /// @param _val         The value that is stored in this node
    data_t(const K &_key, const V &_val) : node_t(), key(_key), val(_val) {}
  };

  node_t *const head; // The list head pointer
  node_t *const tail; // The list tail pointer

public:
  /// Default construct a list by constructing and connecting two sentinel nodes
  ///
  /// @param me  The operation that is constructing the list
  /// @param cfg A configuration object that has a `snapshot_freq` field
  slist_omap(HANDSTM *me, auto *cfg) : head(new node_t()), tail(new node_t()) {
    BEGIN_WO(me);
    head->next.set(wo, head, tail);
  }

private:
  /// get_leq is an inclusive predecessor query that returns the largest node
  /// whose key is <= the provided key.  It can return the head sentinel, but
  /// not the tail sentinel.
  ///
  /// @param key     The key for which we are doing a predecessor query.
  /// @param lt_mode When `true`, this behaves as `get_lt`.  When `false`, it
  ///                behaves as `get_leq`.
  ///
  /// @return The node that was found, and its orec value
  node_t *get_leq(STM &tx, const K key, bool lt_mode = false) {
    // Start at the head; read the next now, to avoid reading it in multiple
    // iterations of the loop
    node_t *curr = head;

    // Starting at `next`, search for key.
    while (true) {
      // Read the next node, fail if we can't do it consistently
      auto next = curr->next.get(tx, curr);

      // Stop if next's key is too big or next is tail
      if (next == tail)
        return curr;
      data_t *dn = static_cast<data_t *>(next);
      K k = dn->key;
      if (lt_mode ? k >= key : k > key)
        return curr;

      // Stop if `next` is the match we were hoping for
      if (k == key)
        return next;

      // Keep traversing to `next`
      curr = next;
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
  bool get(HANDSTM *me, const K &key, V &val) {
    BEGIN_RO(me);
    // find the largest node with a key <= `key`.
    auto n = get_leq(ro, key);
    if (n == head || static_cast<data_t *>(n)->key != key)
      return false;
    data_t *dn = static_cast<data_t *>(n);
    val = dn->val.get(ro, dn);
    return true;
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
  bool insert(HANDSTM *me, const K &key, V &val) {
    BEGIN_WO(me);

    auto n = get_leq(wo, key);
    if (n != head && static_cast<data_t *>(n)->key == key)
      return false;

    // stitch in a new node
    data_t *new_dn = wo.LOG_NEW(new data_t(key, val));
    new_dn->next.set(wo, new_dn, n->next.get(wo, n));
    n->next.set(wo, n, new_dn);
    return true;
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(HANDSTM *me, const K &key) {
    BEGIN_WO(me);
    // NB: this will be a lt query, not a leq query
    auto prev = get_leq(wo, key, true);
    auto curr = prev->next.get(wo, prev);
    // if curr doesn't have a matching key, fail
    if (curr == tail || static_cast<data_t *>(curr)->key != key)
      return false;
    auto next = curr->next.get(wo, curr);
    prev->next.set(wo, prev, next);
    wo.reclaim(curr);
    return true;
  }
};
