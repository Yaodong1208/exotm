#pragma once

/// iht_umap is an HANDSTM implementation of the interlocked hash table.  There
/// is one significant simplification:
///
/// - It *does not* employ the max-depth trick for ensuring constant time
///   access.  The worst-case asymptotic complexity is thus O(log(log(N))).
///   That's probably small enough that nobody will ever care.
///
/// - Note that TM makes it very easy to use the trick where the type of a node
///   is embedded in the type, rather than in the pointer to the type.  This is
///   more like the original IHT, less like our baseline version.
template <typename K, typename V, class HANDSTM> class iht_carumap {
  using WOSTM = typename HANDSTM::WOSTM;
  using ROSTM = typename HANDSTM::ROSTM;
  using STM = typename HANDSTM::STM;
  using ownable_t = typename HANDSTM::ownable_t;
  template <typename T> using FIELD = typename HANDSTM::template xField<T>;

  /// Common parent for EList and PList types.  It uses a bool as a proxy for
  /// RTTI for distinguishing between PLists and ELists
  ///
  /// NB: In golang, the lock would go in Base.
  struct Base : ownable_t {
    const bool isEList; // Is this an EList (true) or a PList (false)

    // Construct the base type by setting its `isElist` field
    Base(bool _isEList) : isEList(_isEList) {}
  };

  /// EList (ElementList) stores a bunch of K/V pairs
  ///
  /// NB: We construct with a factory, so the pairs can be a C-style variable
  ///     length array field.
  struct EList : Base {
    /// The key/value pair.  We don't structure split, so that we can have the
    /// array as a field.
    struct pair_t {
      FIELD<K> key; // A key
      FIELD<V> val; // A value
    };

    FIELD<size_t> count; // # live elements
    pair_t pairs[];      // The K/V pairs stored in this EList

  private:
    /// Force construction via the make_elist factory
    EList() : Base(true), count(0) {}

  public:
    /// Construct a EList that can hold up to `size` elements
    static EList *make(WOSTM &wo, size_t size) {
      EList *e = new (wo.LOG_NEW(
          (ownable_t *)malloc(sizeof(EList) + size * sizeof(pair_t)))) EList();
      return e;
    }

    /// Insert into an EList, without checking if there is enough room
    void unchecked_insert(WOSTM &wo, const K &key, const V &val) {
      auto c = count.get(wo, this);
      pairs[c].key.set(wo, this, key);
      pairs[c].val.set(wo, this, val);
      count.set(wo, this, c + 1);
    }
  };

  /// PList (PointerList) stores a bunch of pointers and their associated locks
  ///
  /// NB: We construct with a factory, so the pairs can be a C-style variable
  ///     length array field.  This means that `depth` and `count` can't be
  ///     const, but that's OK.
  struct PList : Base {
    /// A wrapper around a pointer to a Base object
    struct bucket_t {
      FIELD<Base *> base; // pointer to P/E List
    };

    bucket_t buckets[]; // The pointers stored in this PList

  private:
    /// Force construction via the make_plist factory
    PList() : Base(false) {}

  public:
    /// Construct a PList at depth `depth` that can hold up to `size` elements
    static PList *make(WOSTM &wo, size_t size) {
      PList *p = new (wo.LOG_NEW((ownable_t *)malloc(
          sizeof(PList) + size * sizeof(bucket_t)))) PList();
      for (size_t i = 0; i < size; ++i)
        p->buckets[i].base.set(wo, p, nullptr);
      return p;
    }
  };

  const size_t elist_size; // The size of all ELists
  const size_t plist_size; // The size of the root PList
  PList *root;             // The root PList
  std::hash<K> pre_hash;   // A low-quality hash function from K to size_t

  /// For the time being, we re-hash a key at each level, xor-ing in the level
  /// so that keys are unlikely to collide repeatedly.
  ///
  /// TODO: This shouldn't be too expensive, but we can probably do better.
  uint64_t level_hash(HANDSTM *me, const K &key, size_t level) {
    return me->hash(level ^ pre_hash(key));
  }

  /// Given a PList where the `index`th bucket is a full EList, create a new
  /// PList that is twice the size of `parent` and hash the full EList's
  /// elements into it.  This only takes O(1) time.
  ///
  /// @param parent The PList whose bucket needs rehashing
  /// @param pcount The number of elements in `parent`
  /// @param pdepth The depth of `parent`
  /// @param pidx   The index in `parent` of the bucket to rehash
  PList *rehash(HANDSTM *me, WOSTM &wo, PList *parent, size_t pcount,
                size_t pdepth, size_t pidx) {
    // Make a new PList that is twice as big, with all locks set to E_UNLOCKED
    auto p = PList::make(wo, pcount * 2);

    // hash everything from the full EList into it
    auto source =
        static_cast<EList *>(parent->buckets[pidx].base.get(wo, parent));
    auto c = source->count.get(wo, source);
    for (size_t i = 0; i < c; ++i) {
      auto k = source->pairs[i].key.get(wo, source);
      auto b = level_hash(me, k, pdepth + 1) % pcount;
      auto base = p->buckets[b].base.get(wo, p);
      if (base == nullptr) {
        base = EList::make(wo, elist_size);
        p->buckets[b].base.set(wo, p, base);
      }
      EList *dest = static_cast<EList *>(base);
      dest->unchecked_insert(wo, k, source->pairs[i].val.get(wo, source));
    }

    // The caller locked the pointer to the EList, so we can reclaim the EList
    wo.reclaim(source);
    return p;
  }

public:
  /// Construct an IHT by configuring the constants and building the root PList
  ///
  /// @param me  Unused thread descriptor
  /// @param cfg A configuration object with `chunksize` and `buckets` fields,
  ///            for setting the EList size and root PList size.
  iht_carumap(HANDSTM *me, auto *cfg)
      : elist_size(cfg->chunksize), plist_size(cfg->buckets) {
    BEGIN_WO(me);
    root = PList::make(wo, plist_size);
  }

  /// Search for a key in the map.  If found, return `true` and set the ref
  /// parameter `val` to the associated value.  Otherwise return `false`.
  ///
  /// @param me  Thread context
  /// @param key The key to search for
  /// @param val The value (pass-by-ref) that was found
  bool get(HANDSTM *me, const K &key, V &val) {
    BEGIN_RO(me);
    auto curr = root; // Start at the root PList
    size_t depth = 1, count = plist_size;
    while (true) {
      auto bucket = level_hash(me, key, depth) % count;
      // If it's null, fail
      auto b = curr->buckets[bucket].base.get(ro, curr);
      if (b == nullptr)
        return false;

      // If it's a PList, keep traversing
      if (!b->isEList) {
        curr = static_cast<PList *>(b);
        ++depth;
        count *= 2;
        continue;
      }

      // If it's not null, do a linear search of the keys
      auto e = static_cast<EList *>(b);
      auto c = e->count.get(ro, e);
      for (size_t i = 0; i < c; ++i) {
        if (e->pairs[i].key.get(ro, e) == key) {
          val = e->pairs[i].val.get(ro, e);
          return true;
        }
      }
      // Not found
      return false;
    }
  }

  /// Search for a key in the map.  If found, remove it and its associated value
  /// and return `true`.  Otherwise return `false`.
  ///
  /// @param me  Thread context
  /// @param key The key to search for
  bool remove(HANDSTM *me, const K &key) {
    BEGIN_WO(me);
    auto curr = root; // Start at the root PList
    size_t depth = 1, count = plist_size;
    while (true) {
      auto bucket = level_hash(me, key, depth) % count;
      // If it's null, fail
      auto b = curr->buckets[bucket].base.get(wo, curr);
      if (b == nullptr)
        return false;

      // If it's a PList, keep traversing
      if (!b->isEList) {
        curr = static_cast<PList *>(b);
        ++depth;
        count *= 2;
        continue;
      }

      // If it's not null, do a linear search of the keys
      auto e = static_cast<EList *>(b);
      auto c = e->count.get(wo, e);
      for (size_t i = 0; i < c; ++i) {
        if (e->pairs[i].key.get(wo, e) == key) {
          // remove the K/V pair by overwriting, but only if there's >1 key
          if (c > 1) {
            e->pairs[i].key.set(wo, e, e->pairs[c - 1].key.get(wo, e));
            e->pairs[i].val.set(wo, e, e->pairs[c - 1].val.get(wo, e));
          }
          e->count.set(wo, e, c - 1);
          return true;
        }
      }

      // Not found
      return false;
    }
  }

  /// Insert a new key/value pair into the map, but only if the key is not
  /// already present.  Return `true` if a mapping was added, `false` otherwise.
  ///
  /// @param me  Thread context
  /// @param key The key to try to insert
  /// @param val The value to try to insert
  bool insert(HANDSTM *me, const K key, const V val) {
    BEGIN_WO(me);
    auto curr = root; // Start at the root PList
    size_t depth = 1, count = plist_size;
    while (true) {
      auto bucket = level_hash(me, key, depth) % count;
      // If it's null, make a new EList, insert, and we're done
      auto b = curr->buckets[bucket].base.get(wo, curr);
      if (b == nullptr) {
        auto e = EList::make(wo, elist_size);
        e->unchecked_insert(wo, key, val);
        curr->buckets[bucket].base.set(wo, curr, e);
        return true;
      }

      // If it's a PList, keep traversing
      if (!b->isEList) {
        curr = static_cast<PList *>(b);
        ++depth;
        count *= 2;
        continue;
      }

      // If It's not null, do a linear search of the keys, return false if found
      auto e = static_cast<EList *>(b);
      auto c = e->count.get(wo, e);
      for (size_t i = 0; i < c; ++i) {
        if (e->pairs[i].key.get(wo, e) == key)
          return false;
      }

      // Not found: insert if room
      if (c < elist_size) {
        e->unchecked_insert(wo, key, val);
        return true;
      }

      // Otherwise expand and keep traversing, because pathological hash
      // collisions are always possible.
      curr->buckets[bucket].base.set(
          wo, curr, rehash(me, wo, curr, count, depth, bucket));
    }
  }
};
