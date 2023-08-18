#pragma once

/// An ordered map, implemented as a doubly-linked list.  This map supports
/// get(), insert(), and remove() operations.
///
/// @param K       The type of the keys stored in this map
/// @param V       The type of the values stored in this map
/// @param HANDSTM A thread descriptor type, for safe memory reclamation
template <typename K, typename V, class HANDSTM> class dlist_omap {
  using WOSTM = typename HANDSTM::WOSTM;
  using ROSTM = typename HANDSTM::ROSTM;
  using STM = typename HANDSTM::STM;
  using ownable_t = typename HANDSTM::ownable_t;
  template <typename T> using FIELD = typename HANDSTM::template xField<T>;

  /// A list node.  It has prev and next pointers, but no key or value.  It's
  /// useful for sentinels, so that K and V don't have to be default
  /// constructable.
  struct node_t : ownable_t {
    FIELD<node_t *> prev; // Pointer to predecessor
    FIELD<node_t *> next; // Pointer to successor

    /// Construct a node
    node_t() : prev(nullptr), next(nullptr) {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~node_t() {}
  };

  /// A list node that also has a key and value.  Note that keys are const.
  struct data_t : public node_t {
    const K key;  // The key of this pair
    FIELD<V> val; // The value of this pair

    /// Construct a data_t
    ///
    /// @param _key The key that is stored in this node
    /// @param _val The value that is stored in this node
    data_t(const K &_key, const V &_val) : node_t(), key(_key), val(_val) {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~data_t() {}
  };

  node_t *const head; // The list head pointer
  node_t *const tail; // The list tail pointer

public:
  /// Default construct a list by constructing and connecting two sentinel nodes
  ///
  /// @param me  The operation that is constructing the list
  /// @param cfg A configuration object
  dlist_omap(HANDSTM *me, auto *cfg) : head(new node_t()), tail(new node_t()) {
    BEGIN_WO(me);
    head->next.set(wo, head, tail);
    tail->prev.set(wo, tail, head);
  }

private:
  /// get_leq is an inclusive predecessor query that returns the largest node
  /// whose key is <= the provided key.  It can return the head sentinel, but
  /// not the tail sentinel.
  ///
  /// @param tx  The active transaction
  /// @param key The key for which we are doing a predecessor query.
  ///
  /// @return The node that was found
  node_t *get_leq(STM &tx, const K key) {
    // Start at the head; read the next now, to avoid reading it in multiple
    // iterations of the loop
    node_t *curr = head;
    auto *next = curr->next.get(tx, curr);

    // Starting at `next`, search for key.
    while (true) {
      // Case 1: `next` is tail --> stop the search at curr
      if (next == tail)
        return curr;

      // read next's `next` and `key`
      auto next_next = next->next.get(tx, next);
      auto nkey = static_cast<data_t *>(next)->key;

      // Case 2: `next` is a data node: stop if next->key >= key
      if (nkey > key)
        return curr;
      if (nkey == key)
        return next;

      // Case 3: keep traversing to `next`
      curr = next;
      next = next_next;
    }
  }

public:
  /// Search the data structure for a node with key `key`.  If not found, return
  /// false.  If found, return true, and set `val` to the value associated with
  /// `key`.
  ///
  /// @param me  The thread context
  /// @param key The key to search
  /// @param val A ref parameter for returning key's value, if found
  ///
  /// @return True if the key is found, false otherwise.  The reference
  ///         parameter `val` is only valid when the return value is true.
  bool get(HANDSTM *me, const K &key, V &val) {
    BEGIN_RO(me); // RO tx(me);
    // get_leq will use a read-only transaction to find the largest node with
    // a key <= `key`.
    auto n = get_leq(ro, key);

    // Since we have EBR, we can read n.key without validating and fast-fail
    // on key-not-found
    if (n == head || static_cast<data_t *>(n)->key != key)
      return false;

    // NB: given EBR, we don't need to worry about n._obj being deleted, so
    //     we don't need to validate before looking at the value
    data_t *dn = static_cast<data_t *>(n);
    val = dn->val.get(ro, dn);
    return true;
  }

  /// Create a mapping from the provided `key` to the provided `val`, but only
  /// if no such mapping already exists.  This method does *not* have upsert
  /// behavior for keys already present.
  ///
  /// @param me  The thread context
  /// @param key The key for the mapping to create
  /// @param val The value for the mapping to create
  ///
  /// @return True if the value was inserted, false otherwise.
  bool insert(HANDSTM *me, const K &key, V &val) {
    BEGIN_WO(me); // WO tx(me);

    auto n = get_leq(wo, key);
    if (n != head && static_cast<data_t *>(n)->key == key)
      return false;

    auto next = n->next.get(wo, n);

    // stitch in a new node
    data_t *new_dn = wo.LOG_NEW(new data_t(key, val));
    new_dn->next.set(wo, new_dn, next);
    new_dn->prev.set(wo, new_dn, n);
    n->next.set(wo, n, new_dn);
    next->prev.set(wo, next, new_dn);
    return true;
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  The thread context
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(HANDSTM *me, const K &key) {
    BEGIN_WO(me); // WO tx(me);

    auto n = get_leq(wo, key);
    if (n == head || static_cast<data_t *>(n)->key != key)
      return false;

    // unstitch it
    auto pred = n->prev.get(wo, n), succ = n->next.get(wo, n);
    pred->next.set(wo, pred, succ);
    succ->prev.set(wo, succ, pred);
    wo.reclaim(n);
    return true;
  }
};
