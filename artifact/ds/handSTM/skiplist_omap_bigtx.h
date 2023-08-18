#pragma once

#include <cstdint>
#include <cstdlib>
#include <type_traits>

/// NB: in this implementation we use same orec for each node and its chimney
/// nodes

/// An ordered map, implemented as a doubly-linked skip list.  This map supports
/// get(), insert(), and remove() operations.
///
/// @param K          The type of the keys stored in this map
/// @param V          The type of the values stored in this map
/// @param HANDSTM    A thread descriptor type, for safe memory reclamation
/// @param dummy_key  A fake key, to use in sentinel nodes
/// @param dummy_val  A fake value, to use in sentinel nodes
template <typename K, typename V, class HANDSTM, K dummy_key, V dummy_val>
class skiplist_omap_bigtx {
  using WOSTM = typename HANDSTM::WOSTM;
  using ROSTM = typename HANDSTM::ROSTM;
  using STM = typename HANDSTM::STM;
  using ownable_t = typename HANDSTM::ownable_t;
  template <typename T> using FIELD = typename HANDSTM::template xField<T>;

  /// data_t is a node in the skip list.  It has a key, a value, an owner, a
  /// state, and a "tower" of predecessor and successor pointers
  ///
  /// NB: Height isn't always the size of tower... it tracks how many levels are
  ///     fully and correctly stitched, so it changes during insertion and
  ///     removal.
  struct data_t : ownable_t {
    /// A pair of data pointers, for the successor and predecessor at a level of
    /// the tower
    struct level_t {
      FIELD<data_t *> next; // Succ at this level
      FIELD<data_t *> prev; // Pred at this level
    };

    const K key;          // The key stored in this node
    FIELD<V> val;         // The value stored in this node
    const uint8_t height; // # valid tower nodes
    level_t tower[0];     // Tower of pointers to pred/succ

  private:
    /// Construct a data node.  This is private to force the use of our make_*
    /// methods, which handle allocating enough space for the tower.
    ///
    /// @param _key    The key that is stored in this node
    /// @param _val    The value that is stored in this node
    data_t(K _key, V _val, uint8_t _height)
        : key(_key), val(_val), height(_height) {}

  public:
    /// Construct a sentinel (head or tail) node.  Note that the sentinels can't
    /// easily be of a node type that lacks key and value fields, or else the
    /// variable-length array would preclude inheriting from it.
    ///
    /// @param iHeight  The max number of index layers this node will have
    static data_t *make_sentinel(uint8_t iHeight) {
      int node_size = sizeof(data_t) + (iHeight + 1) * sizeof(level_t);
      void *region = malloc(node_size);
      return new (region) data_t(dummy_key, dummy_val, iHeight);
    }

    /// Construct a data node
    ///
    /// @param iHeight The max number of index layers this node will have
    /// @param key     The key to store in this node
    /// @param val     The value to store in this node
    static data_t *make_data(WOSTM &wo, uint64_t iHeight, K key, V val) {
      int node_size = sizeof(data_t) + (iHeight + 1) * sizeof(level_t);
      void *region = malloc(node_size);
      return wo.LOG_NEW(new (region) data_t(key, val, iHeight));
    }
  };

  const int NUM_INDEX_LAYERS;   // # of index layers.  Doesn't count data layer
  const int SNAPSHOT_FREQUENCY; // # of nodes between snapshots
  data_t *const head;           // The head sentinel
  data_t *const tail;           // The tail sentinel

public:
  /// Default construct a skip list by stitching a head sentinel to a tail
  /// sentinel at each level
  ///
  /// @param _op The operation that is constructing the list
  /// @param cfg A configuration object that has a `snapshot_freq` field
  skiplist_omap_bigtx(HANDSTM *me, auto *cfg)
      : NUM_INDEX_LAYERS(cfg->max_levels),
        SNAPSHOT_FREQUENCY(cfg->snapshot_freq),
        head(data_t::make_sentinel(NUM_INDEX_LAYERS)),
        tail(data_t::make_sentinel(NUM_INDEX_LAYERS)) {
    // NB: Even though the constructor is operating on private data, it needs a
    //     TM context in order to set the head and tail's towers to each other
    BEGIN_WO(me);
    for (auto i = 0; i <= NUM_INDEX_LAYERS; i++) {
      head->tower[i].next.set(wo, head, tail);
      tail->tower[i].prev.set(wo, tail, head);
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
  bool get(HANDSTM *me, const K &key, V &val) {
    BEGIN_RO(me);
    // Do a leq... if head, we fail.  n will never be null or tail
    auto n = get_leq(ro, key);
    if (n == head || n->key != key)
      return false;

    val = n->val.get(ro, n);
    return true;
  }

  /// Create a mapping from the provided `key` to the provided `val`, but only
  /// if no such mapping already exists.  This method does *not* have upsert
  /// behavior for keys already present.
  ///
  /// @param me  The calling thread's HANDSTM
  /// @param key The key for the mapping to create
  /// @param val The value for the mapping to create
  ///
  /// @return True if the value was inserted, false otherwise.
  bool insert(HANDSTM *me, const K &key, V &val) {

    BEGIN_WO(me);
    data_t *new_dn = nullptr;            // The node that we insert, if any
    int target_height = randomLevel(me); // The target index height of new_dn
    // This transaction linearizes the insert by adding the node to the data
    // layer

    // Get the insertion point, make sure `key` not already present
    auto n = get_leq(wo, key);

    if (n != head && n->key == key)
      return false;

    // Stitch new node in at lowest level.  Get() can see it immediately.
    // Remove() has to wait
    auto next = n->tower[0].next.get(wo, n);
    new_dn = data_t::make_data(wo, target_height, key, val);
    new_dn->tower[0].next.set(wo, new_dn, next);
    new_dn->tower[0].prev.set(wo, new_dn, n);
    n->tower[0].next.set(wo, n, new_dn);
    next->tower[0].prev.set(wo, next, new_dn);

    // If this doesn't have any index nodes, we can unmark it and return
    if (target_height == 0)
      return true;

    // 'me' did an insert, still owns the node, and needs to stitch it into
    // index levels.  Do so from bottom to top.  Release after the last level.
    for (int level = 0; level < target_height; ++level)
      index_stitch(wo, new_dn, level);

    return true;
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  The calling thread's HANDSTM
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(HANDSTM *me, const K &key) {
    BEGIN_WO(me);
    // Find the node.  Fail if key not present
    auto n = get_leq(wo, key);
    if (n == head || n->key != key)
      return false;

    // Unstitch the node, starting from its topmost level
    //
    // NB: When depth = 0, it's inefficient to unlock node and then re-lock.
    //     However, it's also inefficient to abort because we can't lock the
    //     predecessor or successor.  Taking ownership then committing avoids
    //     re-traversing, so we'll go with it, especially since doing so won't
    //     block get() or nonconflicting insert().
    index_unstitch(wo, n, n->height);
    return true;
  }

private:
  /// get_leq uses the towers to skip from the head sentinel to the node
  /// with the largest key <= the search key.  It can return the head data
  /// sentinel, but not the tail sentinel.
  ///
  /// get_leq can return an OWNED node.
  ///
  /// @param key The key for which we are doing a predecessor query.
  ///
  /// @return The data node that was found
  data_t *get_leq(STM &tx, const K &key) {
    // We always start at the head sentinel.  Scan its tower to find the
    // highest non-tail level
    data_t *curr = head;
    int current_level = 0;
    for (int i = NUM_INDEX_LAYERS; i > 0; --i) {
      if (head->tower[i].next.get(tx, head) != tail) {
        current_level = i;
        break;
      }
    }

    // Traverse over and down through the index layers
    while (current_level > 0) {
      curr = index_leq(tx, key, curr, current_level);
      if (curr->key == key)
        return curr;
      --current_level;
    }

    // Search in the data layer.  Only return if result valid, not DELETED
    return data_leq(tx, key, curr);
  }

  /// Traverse forward from `start`, considering only tower level `level`,
  /// stopping at the largest key <= `key`
  ///
  /// This can return nodes that are OWNED.  The caller must check.
  ///
  /// @param key   The key for which we are doing a predecessor query.
  /// @param start The start position of this traversal.
  /// @param level The tower level to consider
  ///
  /// @return The node that was found (possibly `start`).
  data_t *index_leq(STM &tx, K key, data_t *start, uint64_t level) {
    auto curr = start;
    while (true) {
      data_t *next = curr->tower[level].next.get(tx, curr);
      if (next == tail)
        return curr;
      auto next_key = next->key; // not tail => next has a valid key
      if (next_key == key)
        return next;
      if (next_key > key)
        return curr;
      curr = next;
    }
  }

  /// Traverse in the data layer to find the largest node with key <= `key`.
  /// This can return an OWNED node.
  ///
  /// @param key   The key for which we are doing a predecessor query.
  /// @param start The start position of this traversal.  This may be the head,
  ///              or an intermediate point in the list
  ///
  /// @return The node that was found (possibly `start`).
  data_t *data_leq(STM &tx, K key, data_t *start) {
    auto curr = start;
    auto next = curr->tower[0].next.get(tx, curr);
    while (true) {
      if (next == tail)
        return curr;
      auto nkey = next->key;
      if (nkey > key)
        return curr;
      if (nkey == key)
        return next;
      curr = next;
      next = next->tower[0].next.get(tx, next);
    }
  }

  /// Generate a random level for a new node
  ///
  /// NB: This code has been verified to produce a nice geometric distribution
  ///     in constant time per call
  ///
  /// @param me The caller's HANDSTM operation
  ///
  /// @return a random number between 0 and NUM_INDEX_LAYERS, inclusive
  int randomLevel(HANDSTM *me) {
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
  /// @param node    The node that was just inserted and stitched into `level`
  /// @param level   The level below where we're stitching
  void index_stitch(WOSTM &wo, data_t *node, uint8_t level) {
    // Go backwards, then up, to find a node at current `level` that is tall
    // enough.  Then get its successor
    data_t *pred = node;
    while (true) {
      pred = pred->tower[level].prev.get(wo, pred);
      if (pred->height > level)
        break;
    }
    auto succ = pred->tower[level + 1].next.get(wo, pred);

    // Stitch `node` in between pred and succ, update node's height
    node->tower[level + 1].next.set(wo, node, succ);
    node->tower[level + 1].prev.set(wo, node, pred);
    pred->tower[level + 1].next.set(wo, pred, node);
    succ->tower[level + 1].prev.set(wo, succ, node);
  }

  /// Unstitch `node`, starting at its topmost index layer.  It is currently
  /// OWNED.
  ///
  /// @param node   The node that we are unstitching
  /// @param height The highest index layer for `node`
  void index_unstitch(WOSTM &wo, data_t *node, int height) {
    // Work our way downward, unstitching at each level
    for (int level = height; level >= 0; --level) {
      auto pre = node->tower[level].prev.get(wo, node);
      auto nxt = node->tower[level].next.get(wo, node);
      pre->tower[level].next.set(wo, pre, nxt);
      nxt->tower[level].prev.set(wo, nxt, pre);
    }
    wo.reclaim(node);
  }
};
