#pragma once

#include <cstdint>

/// A simple, high-quality hash function
///
/// NB: This hash is based on "mix 13" from
///     http://zimbry.blogspot.com/2011/09/better-bit-mixing-improving-on.html
///
/// @param x A 64-bit value to hash
///
/// @return The hash of the provided value
uint64_t mix13_hash(uint64_t x) {
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  x = x ^ (x >> 31);
  return x;
}
