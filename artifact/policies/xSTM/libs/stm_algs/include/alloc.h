/// alloc.h provides a set of allocation managers that can be used by a TM
/// implementation.  These allocation managers all provide the same public
/// interface, so that they are interchangeable in TM algorithms.

#pragma once

#include <cstdlib>

#include "../../../../include/minivector.h"

/// A mechanism that allows a transaction to log its allocations and frees, and
/// to finalize or undo them if the transaction commits or aborts.  It also
/// supports the "capture" optimization, which tracks the most recent allocation
/// and suggests to the TM that accesses in that allocation shouldn't be
/// instrumented.
class BasicAllocationManager {
protected:
  minivector<void *> mallocs; // the transaction's not-yet-committed allocations
  minivector<void *> frees;   // the transaction's not-yet-committed reclaims
  bool active = false;        // track if allocation management is active
  void *lastAlloc;            // address of last allocation
  size_t lastSize;            // size of last allocation

public:
  /// Indicate that logging should begin
  void onBegin() { active = true; }

  /// When a transaction commits, finalize its mallocs and frees.  Note that
  /// this should be called *after* privatization is ensured.
  void onCommit() {
    mallocs.clear();
    for (auto a : frees) {
      free(a);
    }
    frees.clear();
    active = false;
    lastAlloc = nullptr;
    lastSize = 0;
  }

  /// When a transaction aborts, drop its frees and reclaim its mallocs
  void onAbort() {
    frees.clear();
    for (auto p : mallocs) {
      free(p);
    }
    mallocs.clear();
    active = false;
    lastAlloc = nullptr;
    lastSize = 0;
  }

  /// To allocate memory, we must also log it, so we can reclaim it if the
  /// transaction aborts
  void *alloc(size_t size) {
    void *res = malloc(size);
    if (active) {
      mallocs.push_back(res);
      lastAlloc = res;
      lastSize = size;
    }
    return res;
  }

  /// Allocate memory that is aligned on a byte boundary as specified by A
  void *alignAlloc(size_t A, size_t size) {
    void *res = aligned_alloc(A, size);
    if (active) {
      mallocs.push_back(res);
      lastAlloc = res;
      lastSize = size;
    }
    return res;
  }

  /// To free memory, we simply wait until the transaction has committed, and
  /// then we free.
  void reclaim(void *addr) {
    if (active) {
      frees.push_back(addr);
    } else {
      free(addr);
    }
  }

  /// Return true if the given address is within the range returned by the most
  /// recent allocation
  bool checkCaptured(void *addr) {
    uintptr_t lstart = (uintptr_t)lastAlloc;
    uintptr_t lend = lstart + lastSize;
    uintptr_t a = (uintptr_t)addr;
    return a >= lstart && a < lend;
  }
};