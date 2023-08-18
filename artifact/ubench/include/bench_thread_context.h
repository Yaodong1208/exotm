#pragma once

#include <random>

/// bench_thread_context_t has per-thread counters for the six intset benchmark
/// events.  It also has a per-thread pseudorandom number generator.
class bench_thread_context_t {
  /// A large prime.  Use to seed Mersenne Twister because similar seeds lead to
  /// similar sequences
  const uint64_t LARGE_PRIME = 2654435761ULL;

public:
  enum EVENTS {
    GET_T,
    GET_F,
    INS_T,
    INS_F,
    RMV_T,
    RMV_F,
    MOD_T,
    MOD_F,
    RNG_T,
    RNG_F,
    TX_T,
    NUM
  };                            // event types
  std::mt19937 mt;              // Per-thread PRNG
  int stats[EVENTS::NUM] = {0}; // Event counters

  /// Construct a thread's context by creating its PRNG
  bench_thread_context_t(int _id) : mt(_id * LARGE_PRIME) {}
};
