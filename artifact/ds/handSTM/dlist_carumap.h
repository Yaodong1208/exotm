#pragma once

#include <bit>
#include <cassert>
#include <iostream>

/// An unordered map, implemented as a resizable array of lists (closed
/// addressing, resizable).  This map supports get(), insert() and remove()
/// operations.
///
/// This implementation is based loosely on Liu's nonblocking resizable hash
/// table from PODC 2014.  At the current time, we do not support the heuristic
/// for contracting the list, but we do support expanding the list.
///
/// @tparam K       The type of the keys stored in this map
/// @tparam V       The type of the values stored in this map
/// @tparam HANDSTM The thread's descriptor type, for interacting with STM
template <typename K, typename V, class HANDSTM> class dlist_carumap {
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
    node_t() : ownable_t(), prev(nullptr), next(nullptr) {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~node_t() {}
  };

  /// We need to know if buckets have been rehashed to a new table.  We do this
  /// by making the head of each bucket a `sentinel_t`, and adding a `closed`
  /// bool.  Note that the tail of each bucket's list is just a node_t.
  struct sentinel_t : node_t {
    /// Track if this sentinel is for a bucket that has been rehashed
    FIELD<bool> closed;

    /// Construct a sentinel_t
    sentinel_t() : node_t(), closed(false) {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~sentinel_t() {}
  };

  /// A list node that also has a key and value
  struct data_t : node_t {
    const K key;  // The key of this key/value pair
    FIELD<V> val; // The value of this key/value pair

    /// Construct a data_t
    ///
    /// @param _key The key that is stored in this node
    /// @param _val The value that is stored in this node
    data_t(const K &_key, const V &_val) : node_t(), key(_key), val(_val) {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~data_t() {}
  };

  /// An array of lists, along with its size
  ///
  /// NB: to avoid indirection, the array is inlined into the tbl_t.  To make
  ///     this compatible with SMR, tbl_t must be ownable.
  class tbl_t : public ownable_t {
    using bucket_t = FIELD<sentinel_t *>;

    /// Construct a table
    ///
    /// @param _size The desired size of the table
    tbl_t(uint64_t _size) : size(_size) {}

  public:
    const uint64_t size; // The size of the table
    bucket_t tbl[];      // The buckets of the table

    /// Allocate a tbl_t of size `size`
    ///
    /// @param size The desired size
    /// @param tx   The calling operation's descriptor
    ///
    /// @return A table, all of whose buckets are set to null
    static tbl_t *make(uint64_t size, WOSTM &tx) {
      tbl_t *tbl =
          tx.LOG_NEW((tbl_t *)malloc(sizeof(tbl_t) + size * sizeof(bucket_t)));
      auto ret = new (tbl) tbl_t(size);
      for (size_t i = 0; i < size; ++i)
        ret->tbl[i].set(tx, ret, nullptr);
      return ret;
    }

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~tbl_t() {}
  };

  ownable_t *tbl_orec;    // An orec for protecting `active` and `frozen`
  FIELD<tbl_t *> active;  // The active table
  FIELD<tbl_t *> frozen;  // The frozen table
  std::hash<K> _pre_hash; // A weak hash function for converting keys to ints
  const uint64_t RESIZE_THRESHOLD; // Max bucket size before resizing

  /// Given a key, determine the bucket into which it should go.  As in the Liu
  /// hash, we do not change the hash function when we resize, we just change
  /// the number of bits to use
  ///
  /// @param key  The key to hash
  /// @param size The size of the table into which this should be hashed
  ///
  /// @return An integer between 0 and size
  uint64_t table_hash(HANDSTM *me, const K &key, const uint64_t size) const {
    return me->hash(_pre_hash(key)) % size;
  }

public:
  /// Default construct a map as having a valid active table.
  ///
  /// NB: Calls std::terminate if the provided size is not a power of 2.
  ///
  /// @param me  The operation that is creating this umap
  /// @param cfg A config object with `buckets` and `resize_threshold`
  dlist_carumap(HANDSTM *me, auto *cfg)
      : tbl_orec(new ownable_t()), RESIZE_THRESHOLD(cfg->resize_threshold) {
    // Enforce power-of-2 initial size
    if (std::popcount(cfg->buckets) != 1)
      throw("cfg->buckets should be power of 2");

    // Create an initial active table in which all of the buckets are
    // initialized but empty (null <- head <-> tail -> null).
    BEGIN_WO(me);
    auto n = tbl_t::make(cfg->buckets, wo);
    for (size_t i = 0; i < cfg->buckets; ++i)
      n->tbl[i].set(wo, n, create_list(wo));
    // NB: since all buckets are initialized, nobody will ever go to the
    //     frozen table, so we can leave it as null
    active.set(wo, tbl_orec, n);
    frozen.set(wo, tbl_orec, nullptr);
  }

private:
  /// Create a dlist with head and tail sentinels
  ///
  /// @param tx A writing TM context.  Even though this code can't fail, we need
  ///           the context in order to use tm_field correctly.
  ///
  /// @return A pointer to the head sentinel of the list
  sentinel_t *create_list(WOSTM &tx) {
    // NB: By default, a node's prev and next will be nullptr, which is what we
    //     want for head->prev and tail->next.
    auto head = tx.LOG_NEW(new sentinel_t());
    auto tail = tx.LOG_NEW(new node_t());
    head->next.set(tx, head, tail);
    tail->prev.set(tx, tail, head);
    return head;
  }

  /// `resize()` is an internal method for changing the size of the active
  /// table. Strictly speaking, it should be called `expand`, because for now we
  /// only support expansion, not contraction.  When `insert()` discovers that
  /// it has made a bucket "too big", it will linearize its insertion, then call
  /// resize().
  ///
  /// At a high level, `resize()` is supposed to be open-nested and not to incur
  /// any blocking, except due to orec conflicts.  We accomplish this through
  /// laziness and stealing.  resize() finishes the /last/ resize, moves the
  /// `active` table to `frozen`, and installs a new `active` table.  Subsequent
  /// operations will do most of the migrating.  Note that resize() returns once
  /// *anyone* resizes the table.
  ///
  /// @param me     The calling thread's descriptor
  /// @param a_tbl  The active table, to resize
  void resize(HANDSTM *me, tbl_t *a_tbl) {
    tbl_t *ft = nullptr; // The frozen table
    while (true) {
      // If ft is null, then there's no frozen table, so just install a new
      // active table and all is good.
      {
        BEGIN_WO(me);
        // If someone else initiated a resize, then this attempt can end
        // immediately
        auto new_at = active.get(wo, tbl_orec);
        if (new_at != a_tbl)
          return;

        // If the frozen table is clean, just do a swap and we're done
        ft = frozen.get(wo, tbl_orec);
        if (ft == nullptr) {
          // Make and initialize a table that is twice as big, move active to
          // frozen, and make the new table active.
          auto new_tbl = tbl_t::make(a_tbl->size * 2, wo);
          frozen.set(wo, tbl_orec, a_tbl);
          active.set(wo, tbl_orec, new_tbl);
          return;
        }
      }

      // There is still an incomplete migration from frozen to active.  Migrate
      // everything out of frozen, remove the frozen table, and retry
      prepare_resize(me, ft, a_tbl);
    }
  }

  /// Finish one lazy resize, so that another may begin.
  ///
  /// This really just boils down to migrating everything from `frozen` to
  /// `active` and then nulling `frozen` and reclaiming it.
  ///
  /// NB: This code takes the "frozen" and "active" tables as arguments.
  ///     Consequently, we don't care about arbitrary delays.  If a thread calls
  ///     this, rehashes half the table, and then suspends, another thread can
  ///     rehash everything else and install a new active table.  When the first
  ///     thread wakes, it'll find a bunch of empty buckets, and it'll be safe.
  ///
  /// @param me     The calling thread's descriptor
  /// @param f_tbl  The "frozen table", really the "source" table
  /// @param a_tbl  The "active table", really the "destination" table
  void prepare_resize(HANDSTM *me, tbl_t *f_tbl, tbl_t *a_tbl) {
    // NB: Right now, next_index == completed.  If we randomized the start
    //     point, concurrent calls to prepare_resize() would contend less
    uint64_t next_index = 0; // Next bucket to migrate
    uint64_t completed = 0;  // Number of buckets migrated

    // Migrate all data from `frozen` to `active`
    while (completed != f_tbl->size) {
      BEGIN_WO(me);

      // Try to rehash the next bucket.  If it was already rehashed, there's a
      // chance that the current resize phase is finished, so check
      auto bkt = f_tbl->tbl[next_index].get(wo, f_tbl);
      if (!rehash_expand_bucket(me, bkt, next_index, f_tbl->size, a_tbl, wo)) {
        // NB:  A "finished" concurrent resize will change `active`, but if
        //      another thread just got past this loop and uninstalled `frozen`,
        //      that's also cause for early return.
        if (active.get(wo, tbl_orec) != a_tbl || !frozen.get(wo, tbl_orec))
          return;
      }

      // Move to the next bucket
      ++next_index;
      ++completed;
    }

    // Try to uninstall the `frozen` table, since it has been emptied.
    {
      BEGIN_WO(me);
      if (frozen.get(wo, tbl_orec) != f_tbl)
        return;
      frozen.set(wo, tbl_orec, nullptr);
    }

    // Reclaim `old`'s buckets, then `old` itself
    //
    // NB:  This needs to be a transaction because of the API for reclamation,
    //      but there shouldn't be conflicts.
    {
      BEGIN_WO(me);
      for (size_t i = 0; i < f_tbl->size; i++) {
        // reclaim head and tail of each bucket
        auto head = f_tbl->tbl[i].get(wo, f_tbl);
        auto tail = head->next.get(wo, head);
        wo.reclaim(head);
        wo.reclaim(tail);
      }
      wo.reclaim(f_tbl);
    }
  }

  /// Get a pointer to the bucket in the active table that holds `key`.  This
  /// may cause some rehashing to happen.
  ///
  /// NB: The pattern here is unconventional.  get_bucket() is the first step in
  ///     WSTEP transactions.  If it doesn't rehash, then the caller WSTEP
  ///     continues its operation.  If it does rehash, then the caller WSTEP
  ///     commits and restarts, which is a poor-man's open-nested transaction.
  ///     If it encounters an inconsistency, the caller WSTEP will be aborted.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key whose bucket is sought
  /// @param tx  An active WSTEP transaction
  ///
  /// @return On success, a pointer to the head of a bucket, along with
  ///         `tbl_orec`'s value.  nullptr on any rehash.
  node_t *get_bucket(HANDSTM *me, const K &key, WOSTM &tx) {
    // Get the head of the appropriate bucket in the active table
    //
    // NB: Validate or else a_tbl[a_idx] could be out of bounds
    auto a_tbl = active.get(tx, tbl_orec);
    auto a_idx = table_hash(me, key, a_tbl->size);
    if (auto a_bucket = a_tbl->tbl[a_idx].get(tx, a_tbl))
      return a_bucket; // not null --> no resize needed

    // Find the bucket in the frozen table that needs rehashing
    auto f_tbl = frozen.get(tx, tbl_orec);
    auto f_idx = table_hash(me, key, f_tbl->size);
    auto f_bucket = f_tbl->tbl[f_idx].get(tx, f_tbl);

    // Rehash it, tell caller to commit so the rehash appears to be open nested
    //
    // NB: if the rehash fails, it's due to someone else rehashing, which is OK
    rehash_expand_bucket(me, f_bucket, f_idx, f_tbl->size, a_tbl, tx);
    return nullptr;
  }

  /// Re-hash one list in the frozen table into two lists in the active table
  ///
  /// @param me     The calling thread's descriptor
  /// @param f_list A pointer to an (acquired!) list head in the frozen table
  /// @param f_idx  The index of flist in the frozen table
  /// @param f_size The size of the frozen table
  /// @param a_tbl  A reference to the active table
  /// @param tx     An active WSTEP transaction
  ///
  /// @return RESIZE_OK       - The frozen bucket was rehashed into `a_tbl`
  ///         ALREADY_RESIZED - The frozen bucket was empty
  bool rehash_expand_bucket(HANDSTM *me, sentinel_t *f_list, uint64_t f_idx,
                            uint64_t f_size, tbl_t *a_tbl, WOSTM &tx) {
    // Stop if this bucket is already rehashed
    if (f_list->closed.get(tx, f_list))
      return false;

    // Shuffle nodes from f_list into two new lists that will go into `a_tbl`
    auto l1 = create_list(tx), l2 = create_list(tx);
    auto curr = f_list->next.get(tx, f_list);
    while (curr->next.get(tx, curr) != nullptr) {
      auto next = curr->next.get(tx, curr);
      auto data = static_cast<data_t *>(curr);
      auto dest = table_hash(me, data->key, a_tbl->size) == f_idx ? l1 : l2;
      auto succ = dest->next.get(tx, dest);
      dest->next.set(tx, dest, data);
      data->next.set(tx, data, succ);
      data->prev.set(tx, data, dest);
      succ->prev.set(tx, succ, data);
      curr = next;
    }
    // curr is tail, set head->tail
    f_list->next.set(tx, f_list, curr);
    // put the lists into the active table, close the frozen bucket
    a_tbl->tbl[f_idx].set(tx, a_tbl, l1);
    a_tbl->tbl[f_idx + f_size].set(tx, a_tbl, l2);
    f_list->closed.set(tx, f_list, true);
    return true;
  }

  /// Given the head sentinel of a list, search through the list to find the
  /// node with key `key`, if such a node exists in the list.  If it doesn't,
  /// then return the head pointer, along with a count of non-sentinel nodes in
  /// the list
  ///
  /// @param key  The key for which we are searching
  /// @param head The start of the list to search
  /// @param tx   An active WSTEP transaction
  ///
  /// @return {head, count} if the key was not found
  ///         {node, 0}     if the key was found at `node`
  std::pair<node_t *, uint64_t> list_get_or_head(const K &key, sentinel_t *head,
                                                 WOSTM &tx) {
    // Get the head's successor; on any inconsistency, it'll abort
    auto curr = head->next.get(tx, head);
    uint64_t count = 0; // Number of nodes encountered during the loop
    while (true) {
      // if we reached the tail, return the head
      if (curr->next.get(tx, curr) == nullptr)
        return {head, count};

      // return curr if it has a matching key
      if (static_cast<data_t *>(curr)->key == key)
        return {curr, 0};

      // read `next` consistently
      auto next = curr->next.get(tx, curr);
      curr = next;
      ++count;
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
    while (true) {
      BEGIN_WO(me);
      // Get the bucket in `active` where `key` should be.  Returns `nullptr` if
      // it did some resizing, in which case we should commit the resize, then
      // try again.
      auto bucket = get_bucket(me, key, wo);
      if (!bucket)
        continue;

      // Find the node in `bucket` that matches `key`.  If it can't be found,
      // we'll get the head node.
      auto [node, _] =
          list_get_or_head(key, static_cast<sentinel_t *>(bucket), wo);

      // If we got back the head, return false, otherwise read out the data
      if (node == bucket)
        return false;
      data_t *dn = static_cast<data_t *>(node);
      val = dn->val.get(wo, dn);
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
  bool insert(HANDSTM *me, const K &key, V &val) {
    // If we discover that a bucket becomes too full, we'll insert, linearize,
    // and then resize in a new transaction before returning.
    tbl_t *a_tbl = nullptr;
    while (true) {
      BEGIN_WO(me);
      auto bucket = get_bucket(me, key, wo);
      if (!bucket)
        continue;

      // Find the node in `bucket` that matches `key`.  If it can't be found,
      // we'll get the head node.
      auto [node, count] =
          list_get_or_head(key, static_cast<sentinel_t *>(bucket), wo);

      // If we didn't get the head, the key already exists, so return false
      if (node != bucket)
        return false;

      auto next = node->next.get(wo, node);

      // Stitch in a new node
      data_t *new_dn = wo.LOG_NEW(new data_t(key, val));
      new_dn->next.set(wo, new_dn, next);
      new_dn->prev.set(wo, new_dn, node);
      node->next.set(wo, node, new_dn);
      next->prev.set(wo, next, new_dn);
      if (count >= RESIZE_THRESHOLD) {
        a_tbl = active.get(wo, tbl_orec);
        break; // need to resize!
      }
      return true;
    }

    resize(me, a_tbl);
    return true;
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(HANDSTM *me, const K &key) {
    while (true) {
      BEGIN_WO(me);
      // Get the bucket in `active` where `key` should be.  Returns `nullptr` if
      // it did some resizing, in which case we should commit the resize, then
      // try again.
      auto bucket = get_bucket(me, key, wo);
      if (!bucket)
        continue;

      // Find the node in `bucket` that matches `key`.  If it can't be found,
      // we'll get the head node.
      //
      // NB:  This is a big transaction, so the active table can't have changed
      auto [node, __] =
          list_get_or_head(key, static_cast<sentinel_t *>(bucket), wo);

      // If we got back the head, return false
      if (node == bucket)
        return false;

      // unstitch it
      auto pred = node->prev.get(wo, node), succ = node->next.get(wo, node);
      pred->next.set(wo, pred, succ);
      succ->prev.set(wo, succ, pred);
      wo.reclaim(node);
      return true;
    }
  }
};
