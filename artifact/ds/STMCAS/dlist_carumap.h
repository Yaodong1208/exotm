#pragma once

#include <atomic>
#include <bit>
#include <functional>

/// An unordered map, implemented as a resizable array of lists (closed
/// addressing, resizable).  This map supports get(), insert() and remove()
/// operations.
///
/// This implementation is based loosely on Liu's nonblocking resizable hash
/// table from PODC 2014.  At the current time, we do not support the heuristic
/// for contracting the list, but we do support expanding the list.
///
/// @param K      The type of the keys stored in this map
/// @param V      The type of the values stored in this map
/// @param STMCAS The STMCAS implementation (PO or PS)
template <typename K, typename V, class STMCAS> class dlist_carumap {
  using WSTEP = typename STMCAS::WSTEP;
  using RSTEP = typename STMCAS::RSTEP;
  using snapshot_t = typename STMCAS::snapshot_t;
  using ownable_t = typename STMCAS::ownable_t;
  template <typename T> using FIELD = typename STMCAS::template sField<T>;

  /// A list node.  It has prev and next pointers, but no key or value.  It's
  /// useful for sentinels, so that K and V don't have to be default
  /// constructable.
  ///
  /// NB: we do not need a `valid` bit, because any operation that would clear
  ///     it would also acquire this node's orec, and thus any node that would
  ///     encounter a cleared valid bit would also detect an orec inconsistency.
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
    ///
    /// NB: Could we use `prev` to indicated `closed`
    FIELD<bool> closed; // Has it been rehashed?

    /// Construct a sentinel_t
    sentinel_t() : node_t(), closed(false) {}

    /// Destructor is a no-op, but it needs to be virtual because of inheritance
    virtual ~sentinel_t() {}
  };

  /// A list node that also has a key and value.  Note that keys are const, and
  /// values are only accessed while the node is locked, so neither is a
  /// tm_field.
  struct data_t : node_t {
    const K key; // The key of this key/value pair
    V val;       // The value of this key/value pair

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
  /// NB: to avoid indirection, the array is in-lined into the tbl_t.  To make
  ///     this compatible with SMR, tbl_t must be ownable.
  class tbl_t : public ownable_t {
    using bucket_t = FIELD<sentinel_t *>;

    /// Construct a table
    ///
    /// @param `_size` The desired size of the table
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
    static tbl_t *make(uint64_t size, WSTEP &tx) {
      tbl_t *tbl = (tbl_t *)malloc(sizeof(tbl_t) + size * sizeof(bucket_t));
      auto ret = new (tbl) tbl_t(size);
      for (size_t i = 0; i < size; ++i)
        ret->tbl[i].set(nullptr, tx);
      return ret;
    }
  };

  ownable_t *tbl_orec;    // An orec for protecting `active` and `frozen`
  FIELD<tbl_t *> active;  // The active table
  FIELD<tbl_t *> frozen;  // The frozen table
  std::hash<K> _pre_hash; // A weak hash function for converting keys to ints
  const uint64_t RESIZE_THRESHOLD; // Max bucket size before resizing

  /// A pair consisting of a pointer and an orec version.
  struct node_ver_t {
    node_t *_obj = nullptr; // The start of a bucket
    uint64_t _ver = 0;      // NB: _ver may not be related to _obj
  };

  /// Result of trying to resize a bucket
  enum resize_result_t {
    CANNOT_ACQUIRE,  // Couldn't get orec... retry
    ALREADY_RESIZED, // Already resized by another thread
    RESIZE_OK        // Bucket successfully resized
  };

  /// Given a key, determine the bucket into which it should go.  As in the Liu
  /// hash, we do not change the hash function when we resize, we just change
  /// the number of bits to use
  ///
  /// @param key  The key to hash
  /// @param size The size of the table into which this should be hashed
  ///
  /// @return An integer between 0 and size
  uint64_t table_hash(STMCAS *me, const K &key, const uint64_t size) const {
    return me->hash(_pre_hash(key)) % size;
  }

public:
  /// Default construct a map as having a valid active table.
  ///
  /// NB: This constructor calls std::terminate if the provided size is not a
  ///     power of 2.
  ///
  /// @param me  The operation that is creating this umap
  /// @param cfg A config object with `buckets` and `resize_threshold`
  dlist_carumap(STMCAS *me, auto *cfg)
      : tbl_orec(new ownable_t()), RESIZE_THRESHOLD(cfg->resize_threshold) {
    // Enforce power-of-2 initial size
    if (std::popcount(cfg->buckets) != 1)
      throw("cfg->buckets should be power of 2");

    // Create an initial active table in which all of the buckets are
    // initialized but empty (null <- head <-> tail -> null).
    WSTEP tx(me);
    active.set(tbl_t::make(cfg->buckets, tx), tx);
    for (size_t i = 0; i < cfg->buckets; ++i)
      active.get(tx)->tbl[i].set(create_list(tx), tx);
    // NB: since all buckets are initialized, nobody will ever go to the
    //     frozen table, so we can leave it as null
    frozen.set(nullptr, tx);
  }

private:
  /// Create a dlist with head and tail sentinels
  ///
  /// @param tx A writing TM context.  Even though this code can't fail, we need
  ///           the context in order to use tm_field correctly.
  ///
  /// @return A pointer to the head sentinel of the list
  sentinel_t *create_list(WSTEP &tx) {
    // NB: By default, a node's prev and next will be nullptr, which is what we
    //     want for head->prev and tail->next.
    auto head = new sentinel_t();
    auto tail = new node_t();
    head->next.set(tail, tx);
    tail->prev.set(head, tx);
    return head;
  }

  /// `resize()` is an internal method for changing the size of the active
  /// table. Strictly speaking, it should be called `expand`, because for now we
  /// only support expansion, not contraction.  When `insert()` discovers that
  /// it has made a bucket "too big", it will continue to do its insertion and
  /// then, after linearizing, it will call `resize()`.  `remove()` does not
  /// currently call `resize()`.
  ///
  /// At a high level, `resize()` is supposed to be open-nested and not to incur
  /// any blocking, except due to orec conflicts.  We accomplish this through
  /// laziness and stealing.  resize() finishes the /last/ resize, moves the
  /// `active` table to `frozen`, and installs a new `active` table.  Subsequent
  /// operations will do most of the migrating.
  ///
  /// @param me    The calling thread's descriptor
  /// @param a_ver The version of `active` when the resize was triggered
  void resize(STMCAS *me, uint64_t a_ver) {
    // Get the current active and frozen tables, and the frozen table size
    tbl_t *ft = nullptr, *at = nullptr;
    {
      RSTEP tx(me);
      ft = frozen.get(tx);
      at = active.get(tx);
      if (!tx.check_continuation(tbl_orec, a_ver))
        return; // someone else must be starting a resize, so we can quit
    }

    // If ft is null, then there's no frozen table, so things will be easy
    if (ft == nullptr) {
      WSTEP tx(me);

      // Make and initialize the table *before* acquiring orecs, to minimize the
      // critical section.  The table is 2x as big.
      auto new_tbl = tbl_t::make(at->size * 2, tx);

      // Lock the table, move it from `active` to `frozen`, then install the new
      // table.
      if (!tx.acquire_continuation(tbl_orec, a_ver)) {
        // NB: new_tbl is private.  We don't need SMR
        delete new_tbl;
        return; // Someone else is resizing, and that's good enough for `me`
      }
      frozen.set(at, tx);
      active.set(new_tbl, tx);
      return;
    }

    // Migrate everything out of frozen, remove the frozen table, and retry
    //
    // NB: prepare_resize removes the frozen table.  That will change a_ver, so
    //     we need to capture the new a_ver value so that our next attempt won't
    //     fail erroneously.
    a_ver = prepare_resize(me, a_ver, ft, at);
    if (a_ver == 0)
      return; // Someone else finished resizing for `me`

    resize(me, a_ver); // Try again now that it's clean
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
  /// @param a_ver  The active table version when this was called
  /// @param f_tbl  The "frozen table", really the "source" table
  /// @param a_tbl  The "active table", really the "destination" table
  ///
  /// @return {0}       if another thread stole the job of nulling `frozen`.
  ///                   When this happens, there must be a concurrent resize,
  ///                   and since both are trying to do the same thing (expand),
  ///                   the one who receives {0} can just get out of the other's
  ///                   way
  ///         {integer} the new orec version of `active`
  uint64_t prepare_resize(STMCAS *me, uint64_t a_ver, tbl_t *f_tbl,
                          tbl_t *a_tbl) {
    // NB: Right now, next_index == completed.  If we randomized the start
    //     point, concurrent calls to prepare_resize() would contend less
    uint64_t next_index = 0; // Next bucket to migrate
    uint64_t completed = 0;  // Number of buckets migrated

    // Migrate all data from `frozen` to `active`
    while (completed != f_tbl->size) {
      WSTEP tx(me);

      // Try to rehash the next bucket
      auto bucket = f_tbl->tbl[next_index].get(tx);
      auto res =
          rehash_expand_bucket(me, bucket, next_index, f_tbl->size, a_tbl, tx);
      // If we can't acquire all nodes in this bucket, try again, because it
      // might just mean someone else was doing an operation in the bucket.
      if (res == CANNOT_ACQUIRE) {
        tx.unwind();
        continue;
      }
      // If this bucket is already rehashed by others, there is a chance that
      // the current resize phase is finished, so check
      if (res == ALREADY_RESIZED) {
        // check if the active table version changed since resize() was
        // called, if so, we know resize is finished, return
        if (!tx.check_continuation(tbl_orec, a_ver)) {
          tx.unwind();
          return 0;
        }
      }

      // Move to the next bucket
      ++next_index;
      ++completed;
    }

    // Uninstall the `frozen` table, since it has been emptied.  Save the commit
    // time, so we can validate tbl_orec later.
    tbl_t *old;
    {
      WSTEP tx(me);
      if (tx.acquire_continuation(tbl_orec, a_ver)) {
        old = f_tbl;
        frozen.set(nullptr, tx);
      } else
        return 0;
    }
    auto last_commit_time = me->get_last_wo_end_time();

    // Reclaim `old`'s buckets, then `old` itself
    {
      WSTEP tx(me);
      for (size_t i = 0; i < f_tbl->size; i++) {
        // use singleton_reclaim to reclaim head and tail of each bucket
        auto head = old->tbl[i].get(tx);
        auto tail = head->next.get(tx);
        tx.reclaim(head);
        tx.reclaim(tail);
      }
      tx.reclaim(old);
    }
    return last_commit_time;
  }

  /// Get a pointer to the bucket in the active table that holds `key`.  This
  /// may cause some rehashing to happen.
  ///
  /// NB: The pattern here is unconventional.  get_bucket() is the first step in
  ///     WSTEP transactions.  If it doesn't rehash, then the caller WSTEP
  ///     continues its operation.  If it does rehash, then the caller WSTEP
  ///     commits and restarts, which is a poor-man's open-nested transaction.
  ///     If it encounters an inconsistency, the caller WSTEP should "abort" by
  ///     unwinding and restarting. In the third case, this returns *while
  ///     holding an orec*
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key whose bucket is sought
  /// @param tx  An active WSTEP transaction
  ///
  /// @return On success, a pointer to the head of a bucket, along with
  ///         `tbl_orec`'s value.  {nullptr, 0} on any rehash or inconsistency
  node_ver_t get_bucket(STMCAS *me, const K &key, WSTEP &tx) {
    // Get the head of the appropriate bucket in the active table
    //
    // NB: Validate or else a_tbl[a_idx] could be out of bounds
    auto a_tbl = active.get(tx);
    uint64_t a_ver = tx.check_orec(tbl_orec);
    if (a_ver == STMCAS::END_OF_TIME)
      return {nullptr, a_ver};
    auto a_idx = table_hash(me, key, a_tbl->size);
    auto a_bucket = a_tbl->tbl[a_idx].get(tx); // NB: caller will validate
    if (a_bucket)
      return {a_bucket, a_ver}; // not null --> no resize needed

    // Find the bucket in the frozen table that needs rehashing
    auto f_tbl = frozen.get(tx);
    if (tx.check_orec(tbl_orec) == STMCAS::END_OF_TIME)
      return {nullptr, 0}; // this op delayed, rehash finished by someone else!
    auto f_idx = table_hash(me, key, f_tbl->size);
    auto f_bucket = f_tbl->tbl[f_idx].get(tx);
    if (!tx.acquire_consistent(f_bucket))
      return {nullptr, 0}; // someone else is using the old bucket

    // Rehash it, tell caller to commit so the rehash appears to be open nested
    //
    // NB: if the rehash fails, it's due to someone else rehashing, which is OK
    rehash_expand_bucket(me, f_bucket, f_idx, f_tbl->size, a_tbl, tx);
    return {nullptr, 0};
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
  ///         CANNOT_ACQUIRE  - The operation could not acquire all orecs
  resize_result_t rehash_expand_bucket(STMCAS *me, sentinel_t *f_list,
                                       uint64_t f_idx, uint64_t f_size,
                                       tbl_t *a_tbl, WSTEP &tx) {
    // Stop if this bucket is already rehashed
    if (f_list->closed.get(tx)) // true is effectively const, skip validation
      return ALREADY_RESIZED;
    // Fail if we cannot acquire all nodes in f_list
    if (!list_acquire_all(f_list, tx))
      return CANNOT_ACQUIRE;

    // Shuffle nodes from f_list into two new lists that will go into `a_tbl`
    auto l1 = create_list(tx), l2 = create_list(tx);
    auto curr = f_list->next.get(tx);
    while (curr->next.get(tx) != nullptr) {
      auto next = curr->next.get(tx);
      auto data = static_cast<data_t *>(curr);
      auto dest = table_hash(me, data->key, a_tbl->size) == f_idx ? l1 : l2;
      auto succ = dest->next.get(tx);
      dest->next.set(data, tx);
      data->next.set(succ, tx);
      data->prev.set(dest, tx);
      succ->prev.set(data, tx);
      curr = next;
    }
    // curr is tail, set head->tail
    f_list->next.set(curr, tx);
    // put the lists into the active table, close the frozen bucket
    a_tbl->tbl[f_idx].set(l1, tx);
    a_tbl->tbl[f_idx + f_size].set(l2, tx);
    f_list->closed.set(true, tx);
    return RESIZE_OK;
  }

  /// Acquire all of the nodes in the list starting at `head`, including the
  /// head and tail sentinels
  ///
  /// @param head The head of the list whose nodes should be acquired
  /// @param tail The calling WSTEP transaction
  ///
  /// @return true if all nodes are acquired, false otherwise
  bool list_acquire_all(node_t *head, WSTEP &tx) {
    node_t *curr = head;
    while (curr) {
      if (!tx.acquire_consistent(curr))
        return false;
      curr = curr->next.get(tx);
    }
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
  /// @return {nullptr, 0}  if the transaction discovered an inconsistency
  ///         {head, count} if the key was not found
  ///         {node, 0}     if the key was found at `node`
  std::pair<node_t *, uint64_t> list_get_or_head(const K &key, sentinel_t *head,
                                                 WSTEP &tx) {
    // Get the head's successor; on any inconsistency, return.
    auto curr = head->next.get(tx);
    uint64_t head_orec = tx.check_orec(head);
    if (head_orec == STMCAS::END_OF_TIME)
      return {nullptr, 0};

    uint64_t count = 0; // Number of nodes encountered during the loop

    while (true) {
      // if we reached the tail, return the head
      if (curr->next.get(tx) == nullptr)
        return {head, count}; // No validation: tail's next is effectively const

      // return curr if it has a matching key
      if (static_cast<data_t *>(curr)->key == key)
        return {curr, 0};

      // read `next` consistently
      //
      // NB: We could skip this, and just validate before `return {curr, 0}`
      auto next = curr->next.get(tx);
      if (tx.check_orec(curr) == STMCAS::END_OF_TIME)
        return {nullptr, 0};
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
  bool get(STMCAS *me, const K &key, V &val) {
    while (true) {
      WSTEP tx(me);
      // Get the bucket in `active` where `key` should be.  "Abort" and retry on
      // any inconsistency; commit and retry if `get_bucket` resized
      auto [bucket, _] = get_bucket(me, key, tx);
      if (!bucket)
        continue;

      // Find the node in `bucket` that matches `key`.  If it can't be found,
      // we'll get the head node.
      auto [node, __] =
          list_get_or_head(key, static_cast<sentinel_t *>(bucket), tx);

      // If we got back null, there was an inconsistency, so retry
      if (!node) {
        tx.unwind();
        continue;
      }

      // If we got back the head, return false
      if (node == bucket) {
        tx.unwind(); // because we didn't update shared memory
        return false;
      }

      if (std::is_scalar<V>::value) {
        data_t *dn = static_cast<data_t *>(node);
        V val_copy = reinterpret_cast<std::atomic<V> *>(&dn->val)->load(
            std::memory_order_acquire);
        if (tx.check_orec(node) == STMCAS::END_OF_TIME) {
          tx.unwind();
          continue;
        }
        val = val_copy;
        return true;
      } else {
        // Acquire, read, unwind (because no writes!)
        if (!tx.acquire_consistent(node)) {
          tx.unwind();
          continue;
        }
        val = static_cast<data_t *>(node)->val;
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
    // If we discover that a bucket becomes too full, we'll insert, linearize,
    // and then resize in a new transaction before returning.  Tracking
    // `active`'s version prevents double-resizing under concurrency.
    uint64_t a_ver = 0;
    while (true) {
      WSTEP tx(me);
      auto [bucket, a_version] = get_bucket(me, key, tx);
      if (!bucket)
        continue;
      a_ver = a_version;

      // Find the node in `bucket` that matches `key`.  If it can't be found,
      // we'll get the head node.
      auto [node, count] =
          list_get_or_head(key, static_cast<sentinel_t *>(bucket), tx);

      // If we got back null, there was an inconsistency, so retry
      if (!node) {
        tx.unwind();
        continue;
      }

      // If we didn't get the head, the key already exists, so return false
      if (node != bucket) {
        tx.unwind();
        return false;
      }

      // Lock the node and its successor
      if (!tx.acquire_consistent(node)) {
        tx.unwind();
        continue;
      }
      auto next = node->next.get(tx);
      if (!tx.acquire_aggressive(next)) {
        tx.unwind();
        continue;
      }

      // Stitch in a new node
      data_t *new_dn = new data_t(key, val);
      new_dn->next.set(next, tx);
      new_dn->prev.set(node, tx);
      node->next.set(new_dn, tx);
      next->prev.set(new_dn, tx);
      if (count >= RESIZE_THRESHOLD)
        break; // need to resize!
      return true;
    }

    resize(me, a_ver);
    return true;
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(STMCAS *me, const K &key) {
    while (true) {
      WSTEP tx(me);
      // Get the bucket in `active` where `key` should be.  Abort and retry on
      // any inconsistency; commit and retry if `get_bucket` resized
      auto [bucket, _] = get_bucket(me, key, tx);
      if (!bucket)
        continue;

      // Find the node in `bucket` that matches `key`.  If it can't be found,
      // we'll get the head node.
      //
      // NB: While `bucket` has not been reclaimed, `active.tbl` may have
      //     changed.  Fortunately, list_get_or_head will validate it.
      auto [node, __] =
          list_get_or_head(key, static_cast<sentinel_t *>(bucket), tx);

      // If we got back the head, return false
      if (node == bucket) {
        tx.unwind(); // because we didn't update shared memory
        return false;
      }

      // If the `node` is null, list_get_or_head failed and we need to retry
      // Otherwise, it's unowned and the keys match, so lock `node` and its
      // neighbors, else retry
      if (!node || !tx.acquire_consistent(node) ||
          !tx.acquire_aggressive(node->prev.get(tx)) ||
          !tx.acquire_aggressive(node->next.get(tx))) {
        tx.unwind();
        continue;
      }

      // unstitch it
      auto pred = node->prev.get(tx), succ = node->next.get(tx);
      pred->next.set(succ, tx);
      succ->prev.set(pred, tx);
      tx.reclaim(node);
      return true;
    }
  }
};
