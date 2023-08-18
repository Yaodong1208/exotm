#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// STM Non-resizable Hash Table

/// A straightforward non-resizable hashtable. This map supports
/// get(), insert(), remove(), and size() operations.
///
/// @param K      The type of the keys stored in this map
/// @param V      The type of the values stored in this map
/// @param STMCAS The STMCAS implementation (PO or PS)
/// @param OMAP   An ordered map type to use as each bucket
///
/// NB: OMAP must be templated on <K, V, STMCAS>
template <typename K, typename V, class STMCAS, class OMAP>
class ca_umap_list_adapter_t {
  OMAP **buckets;             // The OMAPs that act as the buckets in the table.
  const uint64_t num_buckets; // The number of buckets in the table.

public:
  /// Create a non-resizable hash table with the specified number of buckets.
  ///
  /// @param me  The operation that is constructing the table.
  /// @param cfg A configuration object with a `buckets` field
  ca_umap_list_adapter_t(STMCAS *me, auto *cfg) : num_buckets(cfg->buckets) {
    buckets = (OMAP **)malloc(num_buckets * sizeof(OMAP *));

    // Fill the "buckets" vector with singly-linked lists.
    for (unsigned int i = 0; i < num_buckets; ++i)
      buckets[i] = new OMAP(me, cfg);
  }

private:
  std::hash<K> pre_hash;

  /// Get the index of the bucket where the provided key belongs
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key to hash.
  ///
  /// @return The hashed value of the key, modded by the number of buckets
  int hash(STMCAS *me, const K key) {
    return me->hash(pre_hash(key)) % num_buckets;
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
    return buckets[hash(me, key)]->get(me, key, val);
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
    return buckets[hash(me, key)]->insert(me, key, val);
  }

  /// Clear the mapping involving the provided `key`.
  ///
  /// @param me  The calling thread's descriptor
  /// @param key The key for the mapping to eliminate
  ///
  /// @return True if the key was found and removed, false otherwise
  bool remove(STMCAS *me, const K &key) {
    return buckets[hash(me, key)]->remove(me, key);
  }
};
