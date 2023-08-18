/// constants.h provides some common values to use when instantiating TM
/// algorithm templates.  An instantiation of a TM algorithm is free to ignore
/// these constants and choose others.  These exist primarily to ensure that we
/// have reasonable defaults that are shared across similar instantiations,
/// without hard-coding constants into the instantiations themselves.

#pragma once

#include <cstdint>

/// log_2 of the number of bytes protected by an orec
const int OREC_COVERAGE = 5;

/// Quiescence benefits from a limit on the number of threads.  4096 is safe
const int MAX_THREADS = 4096;

/// The number of orecs in the system
const uint32_t NUM_STRIPES = 1048576;

/// A low threshold for tuning backoff
const uint32_t BACKOFF_MIN = 4;

/// A high threshold for tuning backoff
const uint32_t BACKOFF_MAX = 16;

/// A threshold for the number of consecutive aborts before a transaction should
/// become irrevocable.
const uint32_t ABORTS_THRESHOLD = 100;
