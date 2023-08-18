#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string.h>

#include "hash.h"

/// redolog_nocast_t combines a hash-based index with a vector, so that we can
/// quickly find elements, and still iterate through the collection efficiently.
///
/// This version assumes the programmer does not have unions or casting that can
/// lead to the same location being part of different writes/reads at different
/// sizes.  Note that it still stores chunks of bytes to update, because that's
/// still more efficient.  But those chunks aren't guaranteed to match 1:1 with
/// orecs.
///
/// NB: CHUNKSIZE must be <= 64, and a power of 2.  It should be at least as
///     large as the largest scalar datatype supported by the language.  16, 32,
///     and 64 are probably the only reasonable values.
///
/// NB: CHUNKSIZE dictates the alignment that the compiler must obey.  E.g.,
///     when it is 8, a scalar variable cannot cross an 8-byte boundary, or we
///     will not be able to log it correctly.
///
/// NB: redolog_t cannot handle scalars with a size greater than 8 bytes
template <int CHUNKSIZE = 32> class redolog_nocast_t {
  /// MASK is for isolating/clearing the low bits of an address.
  static const uintptr_t MASK = ((uintptr_t)CHUNKSIZE) - 1;

  /// SPILL_FACTOR determines how full `ht` can get before resizing. It must be
  /// >1.  The value 3 means that we resize when `vec` gets full enough that 1/3
  /// of `ht`'s entries are valid.
  static const int SPILL_FACTOR = 3;

  /// Hash index entry type.
  ///
  /// NB: If vVer != ht.ver, treat the entry as "cleared" or "invalid"
  struct index_t {
    size_t vVer;     // last valid version number
    uintptr_t cAddr; // (aligned) address where this chunk starts
    size_t vIdx;     // index into the vector of chunks

    /// Constructor: Note that version 0 is not allowed in the RedoLog
    index_t() : vVer(0), cAddr(0), vIdx(0) {}
  };

  /// Vector entry type
  struct writeback_chunk_t {
    uintptr_t bAddr;         // aligned base address
    uint64_t vBytes;         // Bitmask for which bytes are valid
    uint8_t data[CHUNKSIZE]; // The actual data
  };

  /// The hash index structure
  struct {
    index_t *tbl; // The "hashtable" of the Redo Log
    size_t len;   // Size of hashtable
    size_t ver;   // For fast-clearing of the hash table
    size_t shift; // used by the hash function
  } ht;

  /// The vector of chunks
  struct {
    writeback_chunk_t *chunks; // The "vector" of the Redo Log
    size_t cap;                // Capacity of the vector
    size_t count;              // Current # elements in vector
  } vec;

  /// This hash function is straight from CLRS (that's where the magic constant
  /// comes from).
  size_t hash(uintptr_t const key) const {
    uint64_t r = mix13_hash(key);
    return (size_t)((r & 0xFFFFFFFF) >> ht.shift);
  }

  /// Double the size of the index.  This *does not* allocate or free memory.
  /// Callers should delete[] the index table, increment the table size, and
  /// then reallocate it.
  size_t doubleIndexLength() {
    assert(ht.shift != 0 &&
           "ERROR: redolog_t doesn't support an index this large");
    ht.shift -= 1;
    ht.len = 1 << (8 * sizeof(uint32_t) - ht.shift);
    return ht.len;
  }

  /// Increase the size of the hash and rehash everything
  __attribute__((noinline)) void rebuild() {
    assert(ht.ver != 0 && "ERROR: the version should *never* be 0");

    // double the index size
    delete[] ht.tbl;
    ht.tbl = new index_t[doubleIndexLength()];

    // rehash the elements
    for (size_t i = 0; i < vec.count; ++i) {
      size_t h = hash(vec.chunks[i].bAddr);

      // search for the next available slot
      while (ht.tbl[h].vVer == ht.ver)
        h = (h + 1) % ht.len;

      ht.tbl[h].cAddr = vec.chunks[i].bAddr;
      ht.tbl[h].vVer = ht.ver;
      ht.tbl[h].vIdx = i;
    }
  }

  /// Double the size of the vector if/when it becomes full
  __attribute__((noinline)) void resize() {
    writeback_chunk_t *temp = vec.chunks;
    vec.cap *= 2;
    vec.chunks =
        (writeback_chunk_t *)malloc(vec.cap * sizeof(writeback_chunk_t));
    memcpy(vec.chunks, temp, sizeof(writeback_chunk_t) * vec.count);
    free(temp);
  }

  /// zero the hash on version# overflow... highly unlikely
  __attribute__((noinline)) void reset_internal() {
    memset(ht.tbl, 0, sizeof(index_t) * ht.len);
    ht.ver = 1;
  }

public:
  /// Construct a RedoLog by providing an initial capacity (default 64)
  redolog_nocast_t(const size_t initial_capacity = 64)
      : ht({nullptr, 0, 1, 8 * sizeof(uint32_t)}),
        vec({nullptr, initial_capacity, 0}) {
    // Find a good index length for the initial capacity of the list.
    while (ht.len < SPILL_FACTOR * initial_capacity)
      doubleIndexLength();
    ht.tbl = new index_t[ht.len];
    vec.chunks =
        (writeback_chunk_t *)malloc(vec.cap * sizeof(writeback_chunk_t));
  }

  /// Reclaim the dynamically allocated parts of a RedoLog when we destroy it
  ~redolog_nocast_t() {
    delete[] ht.tbl;
    free(vec.chunks);
  }

  /// Use linear probing to find the vector index of the chunk containing `key`,
  /// or -1 on failure
  int lookup(uintptr_t key) {
    size_t h = hash(key);
    while (ht.tbl[h].vVer == ht.ver) { // Chunk only valid if versions match
      if (ht.tbl[h].cAddr == key)
        return ht.tbl[h].vIdx;
      /// NB: given SPILL_FACTOR, the search is guaranteed to halt
      h = (h + 1) % ht.len;
      continue;
    }
    return -1;
  }

  /// Fast check if the RedoLog is empty
  bool isEmpty() const { return vec.count == 0; }

  /// reserve is effectively the first half of an "upsert".  It finds the vector
  /// entry into which a key should go, or makes that vector entry
  ///
  /// NB: we expect key's low bits to be masked to zero
  size_t reserve(uintptr_t key) {
    //  Find the slot that this address should hash to. If it is valid,
    //  return the index. If we find an unused slot then it's a new
    //  insertion.
    size_t h = hash(key);
    while (ht.tbl[h].vVer == ht.ver) {
      if (ht.tbl[h].cAddr == key)
        return ht.tbl[h].vIdx;
      // keep probing... as in lookup, the search is guaranteed to halt
      h = (h + 1) % ht.len;
    }

    // at this point, h is an unused position in `ht`
    ht.tbl[h].cAddr = key;
    ht.tbl[h].vVer = ht.ver;
    size_t res = ht.tbl[h].vIdx = vec.count;

    // initialize the chunk in `vec`, advance the count
    vec.chunks[vec.count].bAddr = key;
    vec.chunks[vec.count].vBytes = 0LL;
    ++vec.count;

    // resize vec if there's only one spot left
    if (__builtin_expect(vec.count == vec.cap, false))
      resize();

    // if we reach our load-factor, rebuild the ht
    if (__builtin_expect((vec.count * SPILL_FACTOR) >= ht.len, false))
      rebuild();

    return res;
  }

  /// fast-clear the hash by bumping the version number
  void clear() {
    vec.count = 0;
    ht.ver += 1;
    // if there is version number overflow, we'll need to do a heavyweight reset
    // of the index
    if (ht.ver != 0)
      return;
    reset_internal();
  }

  /// write the chunks of `vec` back to main memory in a manner that is atomic
  /// WRT the C++ memory model.
  void writeback() {
    auto const rlx = std::memory_order_relaxed;
    // We don't think that the order of the writes matters, so just go through
    // the vector from first to last chunk
    for (size_t i = 0; i < vec.count; ++i) {
      // We must be extremely careful when writing a chunk back to memory,
      // because we want these writes to be compatible with a concurrent
      // operation that is speculatively reading (and maybe not validating). We
      // can trust that the compiler isn't mis-aligning scalars, but we need to
      // try to write in 8-byte chunks, then 4-byte chunks then 2-byte chunks,
      // then 1-byte chunks.  If we don't do it with all that complexity, then
      // we risk someone reading the result of a partial write.
      int bytes = 0;
      while (bytes < CHUNKSIZE) {
        // Shift the mask, so that the current bytes' validity is in the low
        // bits of m.
        int m = vec.chunks[i].vBytes >> bytes;
        // Do we have 64 valid bits to write, aligned at a 64-bit boundary?
        if (((m & 0xFF) == 0xFF) && ((bytes % 8) == 0)) {
          uint64_t *addr = (uint64_t *)(vec.chunks[i].bAddr + bytes);
          uint64_t *data = (uint64_t *)(vec.chunks[i].data + bytes);
          std::atomic_ref<uint64_t>(*addr).store(*data, rlx);
          bytes += 8;
        }
        // Can we skip 8 bytes?
        else if ((m & 0xFF) == 0) {
          bytes += 8;
        }
        // Do we have 32 valid bits to write, aligned at a 32-bit boundary?
        else if (((m & 0xF) == 0xF) && ((bytes % 4) == 0)) {
          uint32_t *addr = (uint32_t *)(vec.chunks[i].bAddr + bytes);
          uint32_t *data = (uint32_t *)(vec.chunks[i].data + bytes);
          std::atomic_ref<uint32_t>(*addr).store(*data, rlx);
          bytes += 4;
        }
        // Can we skip 4 bytes?
        else if ((m & 0xF) == 0) {
          bytes += 4;
        }
        // Do we have 16 valid bits to write, aligned at a 16-bit boundary?
        else if (((m & 0x3) == 0x3) && ((bytes % 2) == 0)) {
          uint16_t *addr = (uint16_t *)(vec.chunks[i].bAddr + bytes);
          uint16_t *data = (uint16_t *)(vec.chunks[i].data + bytes);
          std::atomic_ref<uint16_t>(*addr).store(*data, rlx);
          bytes += 2;
        }
        // Can we skip 2 bytes?
        else if ((m & 0x3) == 0) {
          bytes += 2;
        }
        // Do we have 8 valid bits to write, aligned at an 8-bit boundary?
        else if ((m & 0x1) == 0x1) {
          uint8_t *addr = (uint8_t *)(vec.chunks[i].bAddr + bytes);
          uint8_t *data = (uint8_t *)(vec.chunks[i].data + bytes);
          std::atomic_ref<uint8_t>(*addr).store(*data, rlx);
          bytes += 1;
        }
        // skip these 8 bits, because the mask says they are not valid
        else {
          bytes += 1;
        }
      }
    }
  }

  /// type-specialized code for inserting an address/value pair into the RedoLog
  template <typename T> void insert(T *addr, T val) {
    // Align `addr` to CHUNKSIZE, look it up in `ht` to get a chunk in `vec`
    uintptr_t chunk_addr = (uintptr_t)addr & ~MASK;
    int vec_idx = reserve(chunk_addr);

    // compute the address within the chunk where we're going to put `val`
    uint64_t offset = (uintptr_t)addr & MASK;
    uint8_t *dataptr = vec.chunks[vec_idx].data;
    dataptr += offset;

    // do a type-preserving write, and update the chunk's mask of valid bytes
    T *tgt = (T *)dataptr;
    *tgt = val;
    uint64_t mask = (1UL << sizeof(T)) - 1;
    vec.chunks[vec_idx].vBytes |= (mask << offset);
  }

  /// type-specialized code for looking up a value from the RedoLog
  template <typename T> bool get(T *addr, T &val) {
    // Quick exit if the log is empty
    //
    // TODO: Revisit this after specializing tm_field::get to RO vs WO
    if (vec.count == 0)
      return false;
    // Align `addr` to CHUNKSIZE, and see if there is a chunk for it
    uintptr_t key = (uintptr_t)addr & ~MASK;
    int idx = lookup(key);
    if (idx == -1)
      return false; // no chunk == not found

    // Check if the relevant bits are set in the chunk's mask
    uint64_t offset = (uintptr_t)addr & MASK;
    uint64_t nodemask = vec.chunks[idx].vBytes >> offset;
    uint64_t mask = (1UL << sizeof(T)) - 1;
    uint32_t livebits = mask & nodemask;
    if (!livebits)
      return false; // no bits are set, so this part of the chunk isn't valid

    // Read the data to `val` and return true
    uint8_t *dataptr = vec.chunks[idx].data;
    dataptr += offset;
    T *tgt = (T *)dataptr;
    val = *tgt;
    return true;
  }

  /// Perform a speculative read from memory, by casting `addr` to an atomic_ref
  template <typename T> static T safe_read(T *addr) {
    return std::atomic_ref<T>(*addr).load(std::memory_order_acquire);
  }

  /// Report the number of elements stored in the redo log
  size_t size() { return vec.count; }
};
