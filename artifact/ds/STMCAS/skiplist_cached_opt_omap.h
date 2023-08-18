#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

/// An ordered map, implemented as a doubly-linked skip list.  This map supports
/// get(), insert(), and remove() operations.
///
/// This version of the skiplist is heavily optimized to use the biggest STMCAS
/// operations it can, while still avoiding aborts.  This means, for example,
/// trying to stitch as many layers as possible (and to do so via recording old
/// values).
///
/// @param K         The type of the keys stored in this map
/// @param V         The type of the values stored in this map
/// @param STMCAS    The STMCAS implementation (PO or PS)
/// @param dummy_key A fake key, to use in sentinel nodes
/// @param dummy_val A fake value, to use in sentinel nodes
template <typename K, typename V, class STMCAS, K dummy_key, V dummy_val>
class skiplist_cached_opt_omap {
  using WSTEP = typename STMCAS::WSTEP;
  using RSTEP = typename STMCAS::RSTEP;
  using STEP = typename STMCAS::STEP;
  using ownable_t = typename STMCAS::ownable_t;
  template <typename T> using FIELD = typename STMCAS::template sField<T>;

  /// data_t is a node in the skip list.  It has a key, a value, an owner, and a
  /// "tower" of predecessor and successor pointers
  ///
  /// NB: Height isn't always the size of tower... it tracks how many levels are
  ///     fully and correctly stitched, so it changes during insertion and
  ///     removal.
  struct data_t : public ownable_t {
    /// A pair of data pointers, for the successor and predecessor at a level of
    /// the tower
    struct level_t {
      FIELD<K> key;         // Key of the successor
      FIELD<data_t *> next; // Succ at this level
    };

    const K key;          // The key stored in this node
    std::atomic<V> val;   // The value stored in this node
    const uint8_t height; // # valid tower nodes
    level_t tower[];      // Tower of pointers to pred/succ

  private:
    /// Construct a data node.  This is private to force the use of our make_*
    /// methods, which handle allocating enough space for the tower.
    ///
    /// @param _key    The key that is stored in this node
    /// @param _val    The value that is stored in this node
    data_t(K _key, V _val, uint8_t _height)
        : ownable_t(), key(_key), val(_val), height(_height) {}

  public:
    /// Construct a sentinel (head or tail) node.  Note that the sentinels can't
    /// easily be of a node type that lacks key and value fields, or else the
    /// variable-length array would preclude inheriting from it.
    ///
    /// @param iHeight  The max number of index layers this node will have
    static data_t *make_sentinel(uint8_t iHeight) {
      int node_size = sizeof(data_t) + (iHeight + 1) * sizeof(level_t);
      void *region = calloc(1, node_size);
      return new (region) data_t(dummy_key, dummy_val, iHeight);
    }

    /// Construct a data node
    ///
    /// @param iHeight The max number of index layers this node will have
    /// @param key     The key to store in this node
    /// @param val     The value to store in this node
    static data_t *make_data(uint64_t iHeight, K key, V val) {
      int node_size = sizeof(data_t) + (iHeight + 1) * sizeof(level_t);
      void *region = calloc(1, node_size);
      return new (region) data_t(key, val, iHeight);
    }
  };

  const int NUM_INDEX_LAYERS; // # of index layers.  Doesn't count data layer
  data_t *const head;         // The head sentinel
  data_t *const tail;         // The tail sentinel

public:
  /// Default construct a skip list by stitching a head sentinel to a tail
  /// sentinel at each level
  ///
  /// @param _op  The operation that is constructing the list
  /// @param cfg A configuration object that has a `snapshot_freq` field
  skiplist_cached_opt_omap(STMCAS *_op, auto *cfg)
      : NUM_INDEX_LAYERS(cfg->max_levels),
        head(data_t::make_sentinel(NUM_INDEX_LAYERS)),
        tail(data_t::make_sentinel(NUM_INDEX_LAYERS)) {
    // NB: Even though the constructor is operating on private data, it needs a
    //     TM context in order to set the head and tail's towers to each other
    WSTEP tx(_op);
    for (auto i = 0; i <= NUM_INDEX_LAYERS; i++) {
      head->tower[i].key.set(dummy_key, tx);
      head->tower[i].next.set(tail, tx);
    }
  }

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
    while (true) {
      RSTEP tx(me);
      // Do a leq... if head, we fail.  n will never be null or tail
      auto n = get_leq(tx, key);
      if (n == nullptr)
        continue;

      if (n == head || n->key != key)
        return false;

      // since we have EBR, `val` can be atomic, making this code quite simple

      // NB: get() doesn't care if the node is owned, just that it's still in
      //     the skiplist
      V val_copy = n->val.load(std::memory_order_acquire);
      // Check after reading value
      if (tx.check_orec(n) == STMCAS::END_OF_TIME)
        continue;
      val = val_copy;
      return true;
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
    data_t *preds[NUM_INDEX_LAYERS];
    int target_height = randomLevel(me); // The target index height of new_dn

    while (true) {
      WSTEP tx(me);
      // Get the insertion point, lock it or retry
      auto n = get_leq(tx, key, preds, target_height);

      if (n == nullptr)
        continue;

      // Since we have EBR, we can look at n->key without validation.  If
      // it matches `key`, return false.
      if (n != head && n->key == key)
        return false;

      // Acquire the pred of the to-be-inserted node
      if (!tx.acquire_consistent(n)) {
        tx.unwind();
        continue;
      }
      auto next = n->tower[0].next.get(tx);

      // If this is a "short" insert, we can finish quickly
      if (target_height == 0) {
        auto new_dn = data_t::make_data(target_height, key, val);
        new_dn->tower[0].key.set(next->key, tx);
        new_dn->tower[0].next.set(next, tx);
        // NB: we don't need to acquire new_dn in this case, because anyone who
        // finds their way to it will find it fully stitched in.
        n->tower[0].key.set(key, tx);
        n->tower[0].next.set(new_dn, tx);
        return true;
      }

      // Slow path for when the node is tall, and we have a lot of acquiring to
      // do
      if (index_stitch(tx, me, n, next, preds, key, val, target_height))
        return true;
      tx.unwind();
    }
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(STMCAS *me, const K &key) {
    data_t *preds[NUM_INDEX_LAYERS];

    while (true) {
      WSTEP tx(me);
      // Get predecessor, find its next, if != key return false
      data_t *n = get_le(tx, key, preds);
      if (n == nullptr)
        continue;
      auto found = n->tower[0].next.get(tx);
      if (found == nullptr) {
        tx.unwind();
        continue;
      }
      if (found == tail || found->key != key)
        return false;

      // Acquire the target, make sure it's not owned
      if (!tx.acquire_consistent(found)) {
        tx.unwind();
        continue;
      }
      // Acquire the predecessor so we can edit its next pointer
      if (!tx.acquire_consistent(n)) {
        tx.unwind();
        continue;
      }

      // Fast-path unstitch when it has height 0
      if (found->height == 0) {
        auto nxt = found->tower[0].next.get(tx);
        n->tower[0].next.set(nxt, tx);
        n->tower[0].key.set(nxt->key, tx);
        // NB: don't forget to set `node`'s pointers to null!
        found->tower[0].next.set(nullptr, tx);
        tx.reclaim(found);
        return true;
      }

      // Slow-path unstitch when it's tall
      if (index_unstitch(tx, me, found, n, preds))
        return true;
      tx.unwind();
    }
  }

private:
  /// get_leq uses the towers to skip from the head sentinel to the node
  /// with the largest key <= the search key.  It can return the head data
  /// sentinel, but not the tail sentinel.
  ///
  /// There is no atomicity between get_leq and its caller.  It returns the
  /// node it found, along with the value of the orec for that node at the time
  /// it was accessed.  The caller needs to validate the orec before using the
  /// returned node.
  ///
  /// get_leq *can* return an OWNED node.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for which we are doing a predecessor query.
  ///
  /// @return The data node that was found, and its orec's value
  __attribute__((noinline)) data_t *get_leq(STEP &tx, const K &key) {
    // We always start at the head sentinel.  Scan its tower to find the
    // highest non-tail level
    data_t *curr = head;
    int current_level = 0;
    for (int i = NUM_INDEX_LAYERS; i > 0; --i) {
      if (head->tower[i].next.get(tx) != tail) {
        current_level = i;
        break;
      }
    }

    // Traverse over and down through the index layers
    while (current_level > 0) {
      // Advance curr by moving forward in this index layer
      curr = index_leq(tx, key, curr, current_level);
      if (curr == nullptr)
        return nullptr;
      // On a key match, we can exit immediately
      if (curr->key == key)
        return curr;
      --current_level; // Move down a level
    }

    // Search in the data layer.  Only return if result valid
    return data_leq(tx, key, curr);
  }

  /// A version of get_leq that is specialized for insert, where we need to get
  /// the predecessors at all levels
  __attribute__((noinline)) data_t *get_leq(WSTEP &tx, const K &key,
                                            data_t **preds, int target_height) {
    // We always start at the head sentinel.  Scan its tower to find the
    // highest non-tail level
    data_t *curr = head;
    int current_level = 0;
    for (int i = NUM_INDEX_LAYERS; i > 0; --i) {
      if (head->tower[i].next.get(tx) != tail) {
        current_level = i;
        break;
      }
      if (current_level <= target_height)
        preds[i - 1] = head;
    }

    // Traverse over and down through the index layers
    while (current_level > 0) {
      // Advance curr by moving forward in this index layer
      curr = index_leq(tx, key, curr, current_level);
      if (curr == nullptr)
        return nullptr;
      // On a key match, we can exit immediately
      if (curr->key == key)
        return curr;
      // we need to save current node to preds
      if (current_level <= target_height)
        preds[current_level - 1] = curr;
      --current_level; // Move down a level
    }

    // Search in the data layer.  Only return if result valid
    return data_leq(tx, key, curr);
  }

  /// A version of get_le that is specialized for remove, where we need to get
  /// the predecessors at all levels
  __attribute__((noinline)) data_t *get_le(WSTEP &tx, const K &key,
                                           data_t **preds) {
    // We always start at the head sentinel.  Scan its tower to find the
    // highest non-tail level
    data_t *curr = head;
    int current_level = 0;
    for (int i = NUM_INDEX_LAYERS; i > 0; --i) {
      if (head->tower[i].next.get(tx) != tail) {
        current_level = i;
        break;
      }
      preds[i - 1] = head;
    }

    // Traverse over and down through the index layers
    while (current_level > 0) {
      // Advance curr by moving forward in this index layer
      curr = index_le(tx, key, curr, current_level);
      // Deal with index_le failing by returning null
      if (curr == nullptr)
        return nullptr;
      // we need to save current node to preds
      preds[current_level - 1] = curr;
      --current_level; // Move down a level
    }

    // Search in the data layer.  Only return if result valid
    return data_le(tx, key, curr);
  }

  /// Traverse forward from `start`, considering only tower level `level`,
  /// stopping at the largest key <= `key`
  ///
  /// This can return nodes that are OWNED.  The caller must check.
  ///
  /// @param tx    The enclosing RSTEP_TM operation's descriptor
  /// @param key   The key for which we are doing a predecessor query.
  /// @param start The start position of this traversal.
  /// @param level The tower level to consider
  ///
  /// @return The node that was found (possibly `start`).  The caller must
  ///         validate the node
  data_t *index_leq(STEP &tx, K key, data_t *start, uint64_t level) {
    // NB: The consistency argument here is nuanced: keys are immutable. Next
    //     pointers are never modified during an unstitch.  Thus we can race
    //     forward, and let the caller validate whatever we find.
    auto curr = start;
    while (true) {
      data_t *next = curr->tower[level].next.get(tx);
      auto next_key = curr->tower[level].key.get(tx);
      if (tx.check_orec(curr) == STMCAS::END_OF_TIME)
        return nullptr;
      if (next == nullptr)
        return nullptr;
      if (next == tail)
        return curr;
      if (next_key == key)
        return next;
      if (next_key > key)
        return curr;
      curr = next;
    }
  }

  /// Traverse forward from `start`, considering only tower level `level`,
  /// stopping at the largest key <= `key`
  ///
  /// This can return nodes that are OWNED.  The caller must check.
  ///
  /// @param tx    The enclosing RSTEP_TM operation's descriptor
  /// @param key   The key for which we are doing a predecessor query.
  /// @param start The start position of this traversal.
  /// @param level The tower level to consider
  ///
  /// @return The node that was found (possibly `start`).  The caller must
  ///         validate the node
  data_t *index_le(STEP &tx, K key, data_t *start, uint64_t level) {
    // NB: The consistency argument here is nuanced: keys are immutable. Next
    //     pointers are never modified during an unstitch.  Thus we can race
    //     forward, and let the caller validate whatever we find.
    auto curr = start;
    while (true) {
      data_t *next = curr->tower[level].next.get(tx);
      auto next_key = curr->tower[level].key.get(tx);
      if (tx.check_orec(curr) == STMCAS::END_OF_TIME)
        return nullptr;
      if (next == nullptr)
        return nullptr;
      if (next == tail || next_key >= key)
        return curr;
      curr = next;
    }
  }

  /// Traverse in the data layer to find the largest node with key <= `key`.
  ///
  /// This can return an OWNED node
  ///
  /// @param tx    The enclosing RSTEP_TM operation's descriptor
  /// @param key   The key for which we are doing a predecessor query.
  /// @param start The start position of this traversal.  This may be the head,
  ///              or an intermediate point in the list
  ///
  /// @return The node that was found (possibly `start`), and its orec value.
  ///         {nullptr, 0} can be returned on inconsistency
  data_t *data_leq(STEP &tx, K key, data_t *start) {
    // Set up the start point for our traversal, then start iterating
    data_t *curr = start;
    data_t *next = curr->tower[0].next.get(tx);
    while (true) {
      // Case 0: `next` is nullptr: restart
      if (next == nullptr)
        return nullptr;
      // Case 1: `next` is tail --> stop the search at curr
      if (next == tail)
        return curr;
      // Case 2: `next` is a data node: stop if next->key >= key
      auto nkey = next->key;
      if (nkey > key)
        return curr;
      if (nkey == key)
        return next;
      // Case 3: Keep traversing
      curr = next;
      next = next->tower[0].next.get(tx);
    }
  }

  /// Traverse in the data layer to find the largest node with key <= `key`.
  ///
  /// @param tx    The enclosing RSTEP_TM operation's descriptor
  /// @param key   The key for which we are doing a predecessor query.
  /// @param start The start position of this traversal.  This may be the head,
  ///              or an intermediate point in the list
  ///
  /// @return The node that was found (possibly `start`), and its orec value.
  ///         {nullptr, 0} can be returned on inconsistency
  data_t *data_le(STEP &tx, K key, data_t *start) {
    // Set up the start point for our traversal, then start iterating
    data_t *curr = start;
    data_t *next = curr->tower[0].next.get(tx);
    while (true) {
      if (next == nullptr)
        return nullptr;
      if (next == tail)
        return curr;
      auto nkey = next->key;
      if (nkey >= key)
        return curr;
      curr = next;
      next = next->tower[0].next.get(tx);
    }
  }

  /// Generate a random level for a new node
  ///
  /// NB: This code has been verified to produce a nice geometric distribution
  ///     in constant time per call
  ///
  /// @param me The caller's STMCAS operation
  ///
  /// @return a random number between 0 and NUM_INDEX_LAYERS, inclusive
  int randomLevel(STMCAS *me) {
    // Get a random int between 0 and 0xFFFFFFFF
    int rr = me->rand();
    // Add 1 to it, then find the lowest nonzero bit.  This way, we never return
    // a zero for small integers, and the distribution is correct.
    int res = __builtin_ffs(rr + 1);
    // Now take one off of that, so that we return a zero-based integer
    res -= 1;
    // But if rr was 0xFFFFFFFF, we've got a problem, so coerce it back
    // Also, drop it down to within NUM_INDEX_LAYERS
    return (res < 0 || res > NUM_INDEX_LAYERS) ? NUM_INDEX_LAYERS : res;
  }

  /// index_stitch is a small atomic operation that stitches a node in at a
  /// given index level.
  ///
  /// @param me      The currently active STMCAS operation
  /// @param node    The node that was just inserted and stitched into `level`
  /// @param level   The level below where we're stitching
  /// @param release Should `node` be marked UNOWNED before returning?
  bool index_stitch(WSTEP &tx, STMCAS *me, data_t *n, data_t *s, data_t **preds,
                    const K &key, V &val, int target_height) {
    // acquire all the levels or fail.  n and s are already acquired
    for (int level = 0; level < target_height; ++level) {
      // preds[level] is actually a /level + 1/ height node
      auto pred = preds[level];
      if (!tx.acquire_consistent(pred))
        return false;
    }

    // `n` is the predecessor to the node we're making, `s` is the successor
    // Fully initialize new_dn before we make it visible at any level.  This
    // suffices to avoid acquiring the new node.
    data_t *new_dn = data_t::make_data(target_height, key, val);
    for (int level = 0; level < new_dn->height; ++level) {
      auto succ = preds[level]->tower[level + 1].next.get(tx);
      new_dn->tower[level + 1].key.set(succ->key, tx);
      new_dn->tower[level + 1].next.set(succ, tx);
    }
    new_dn->tower[0].key.set(s->key, tx);
    new_dn->tower[0].next.set(s, tx);

    // Make it visible in the data level, then in index levels from bottom up
    n->tower[0].next.set(new_dn, tx);
    n->tower[0].key.set(new_dn->key, tx);
    for (int level = 0; level < new_dn->height; ++level) {
      preds[level]->tower[level + 1].next.set(new_dn, tx);
      preds[level]->tower[level + 1].key.set(new_dn->key, tx);
    }
    return true;
  }

  /// Unstitch `node`, starting at its topmost index layer.  Reclaim once it's
  /// fully unstitched.
  ///
  /// @param me     The currently active STMCAS operation
  /// @param node   The node that we are unstitching
  bool index_unstitch(WSTEP &tx, STMCAS *me, data_t *node, data_t *prev,
                      data_t **preds) {
    // Acquire everything, from bottom to top
    for (int level = 0; level < node->height; ++level)
      if (!tx.acquire_consistent(preds[level]))
        return false;

    // Now update all the pointers, from top to bottom
    for (int level = node->height; level >= 0; --level) {
      auto pre = (level > 0) ? preds[level - 1] : prev;
      auto nxt = node->tower[level].next.get(tx);
      pre->tower[level].key.set(nxt->key, tx);
      pre->tower[level].next.set(nxt, tx);
    }

    // NB: don't forget to set `node`'s pointers to null!
    for (int level = node->height; level >= 0; --level)
      node->tower[level].next.set(nullptr, tx);
    // Reclaim it and we're done
    tx.reclaim(node);
    return true;
  }
};
