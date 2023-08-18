#pragma once

#include <cstdint>
#include <x86intrin.h>

/// A good-faith reimplementation of Fraser's PRNG.  We use the same magic
/// constants, and we seed the PRNG with the result of rdtsc.
class rdtsc_rand_t {
  uint64_t seed; // The seed... should be 64 bits, even though we return 32 bits

public:
  /// Construct the PRNG by setting the seed to the value of rdtsc
  rdtsc_rand_t() : seed(__rdtsc()) {}

  /// Generate a random number and update the seed
  uint32_t rand() { return (seed = (seed * 1103515245) + 12345); }
};
