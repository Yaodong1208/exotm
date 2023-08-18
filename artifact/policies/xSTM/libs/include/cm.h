/// cm.h provides a set of contention managers that can be used by a TM
/// implementation.  These contention managers all provide the same public
/// interface, so that they are interchangeable in TM algorithms.

#pragma once

#include "platform.h"

/// NoopCM is a contention manager that does no contention management.
class NoopCM {
public:
  /// NoopCM::Globals is empty, but to simplify the use of this class as a
  /// template parameter to TM algorithms, we need to define a Globals class
  /// anyway.
  class Globals {};

  /// Construct a no-op contention manager
  NoopCM() {}

  /// CM code to run before beginning a transaction
  /// @returns true if the transaction should become irrevocable
  bool beforeBegin(Globals &) { return false; }

  /// CM code to run after a transaction finishes cleaning up from an abort
  void afterAbort(Globals &, uint64_t) {}

  /// CM code to run after a transaction finishes cleaning up from a commit
  void afterCommit(Globals &) {}
};

/// ExpBackoffCM is a contention manager that does randomized exponential
/// backoff on abort
///
/// The backoff threshold can be tuned via MIN and MIX, which are the logarithms
/// of the shortest and longest backoff times.  Backoff times are in # cycles,
/// via the CPU tick counter.
template <int MIN, int MAX> class ExpBackoffCM {
public:
  /// ExpBackoffCM::Globals is empty, but to simplify the use of this class as a
  /// template parameter to TM algorithms, we need to define a Globals class
  /// anyway.
  class Globals {};

private:
  /// The number of consecutive aborts by the current thread
  int consecAborts;

  /// A seed to use for random number generation
  unsigned seed;

public:
  /// Construct an exponential backoff contention manager
  ExpBackoffCM() : consecAborts(0), seed((uintptr_t)(&consecAborts)) {}

  /// CM code to run before beginning a transaction
  /// @returns true if the transaction should become irrevocable
  bool beforeBegin(Globals &) { return false; }

  /// CM code to run after a transaction finishes cleaning up from an abort
  void afterAbort(Globals &, uint64_t) {
    exp_backoff(++consecAborts, seed, MIN, MAX);
  }

  /// CM code to run after a transaction finishes cleaning up from a commit
  void afterCommit(Globals &) { consecAborts = 0; }
};
