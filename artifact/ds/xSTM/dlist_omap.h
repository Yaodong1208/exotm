#pragma once

#include "../../policies/xSTM/common/tm_api.h"

// NB: We need an operator new().  It can just forward to malloc()
TX_RENAME(_Znwm) void *my_new(std::size_t size) {
  void *ptr = malloc(size);
  return ptr;
}

/// An ordered map, implemented as a doubly-linked list.  This map supports
/// get(), insert(), and remove() operations.
///
/// @param K          The type of the keys stored in this map
/// @param V          The type of the values stored in this map
/// @param DESCRIPTOR A thread descriptor type, for safe memory reclamation
template <typename K, typename V, class DESCRIPTOR> class dlist_omap {

  /// A list node.  It has prev and next pointers, but no key or value.  It's
  /// useful for sentinels, so that K and V don't have to be default
  /// constructable.
  struct node_t {
    node_t *prev; // Pointer to predecessor
    node_t *next; // Pointer to successor

    /// Construct a node
    node_t() : prev(nullptr), next(nullptr) { TX_CTOR; }

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~node_t() {}
  };

  /// A list node that also has a key and value.  Note that keys are const.
  struct data_t : public node_t {
    const K key; // The key of this key/value pair
    V val;       // The value of this key/value pair

    /// Construct a data_t
    ///
    /// @param _key The key that is stored in this node
    /// @param _val The value that is stored in this node
    data_t(const K &_key, const V &_val) : node_t(), key(_key), val(_val) {
      TX_CTOR;
    }

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
  dlist_omap(DESCRIPTOR *, auto *cfg) : head(new node_t()), tail(new node_t()) {
    head->next = tail;
    tail->prev = head;
  }

private:
  /// get_leq is an inclusive predecessor query that returns the largest node
  /// whose key is <= the provided key.  It can return the head sentinel, but
  /// not the tail sentinel.
  ///
  /// @param key The key for which we are doing a predecessor query.
  ///
  /// @return The node that was found
  node_t *get_leq(const K key) {
    // Start at the head; read the next now, to avoid reading it in multiple
    // iterations of the loop
    node_t *curr = head;
    auto *next = curr->next;

    // Starting at `next`, search for key.  Breaking out of this will take us
    // back to the top of the function.
    while (true) {
      // Case 1: `next` is tail --> stop the search at curr
      if (next == tail)
        return curr;

      // read next's `next` and `key`
      auto next_next = next->next;
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
  /// @param me  Unused thread context
  /// @param key The key to search
  /// @param val A ref parameter for returning key's value, if found
  ///
  /// @return True if the key is found, false otherwise.  The reference
  ///         parameter `val` is only valid when the return value is true.
  bool get(DESCRIPTOR *, const K &key, V &val) {
    TX_RAII;
    // get_leq will use a read-only transaction to find the largest node with
    // a key <= `key`.
    auto n = get_leq(key);

    // Since we have EBR, we can read n.key without validating and fast-fail
    // on key-not-found
    if (n == head || static_cast<data_t *>(n)->key != key)
      return false;

    // NB: given EBR, we don't need to worry about n._obj being deleted, so
    //     we don't need to validate before looking at the value
    data_t *dn = static_cast<data_t *>(n);
    val = dn->val;
    return true;
  }

  /// Create a mapping from the provided `key` to the provided `val`, but only
  /// if no such mapping already exists.  This method does *not* have upsert
  /// behavior for keys already present.
  ///
  /// @param me  Unused thread context
  /// @param key The key for the mapping to create
  /// @param val The value for the mapping to create
  ///
  /// @return True if the value was inserted, false otherwise.
  bool insert(DESCRIPTOR *, const K &key, V &val) {
    TX_RAII;
    auto n = get_leq(key);
    if (n != head && static_cast<data_t *>(n)->key == key)
      return false;

    auto next = n->next;

    // stitch in a new node
    data_t *new_dn = new data_t(key, val);
    new_dn->next = next;
    new_dn->prev = n;
    n->next = new_dn;
    next->prev = new_dn;
    return true;
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  Unused thread context
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(DESCRIPTOR *, const K &key) {
    TX_RAII;
    auto n = get_leq(key);
    if (n == head || static_cast<data_t *>(n)->key != key)
      return false;

    // unstitch it
    auto pred = n->prev, succ = n->next;
    pred->next = succ;
    succ->prev = pred;
    delete (n);
    return true;
  }
};
