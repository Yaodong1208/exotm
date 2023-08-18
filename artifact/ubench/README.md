# Stress-Test Data Structure Microbenchmarks

This folder contains stress-test microbenchmarks.

In order to avoid anomalies, each data structure is its own executable file.
All experiments share a common harness and a common set of command-line
configuration options.

This design means that each executable consists of a small C++ file, consisting
only of `#include` statements and a few type aliases.

Note that the build process has some big combinatorial factors.  For xSTM, we
must link each data structure against each STM algorithm.  For
handSTM/STMCAS/hybrid, we must compile for each policy implementation (and for
PO versus PS).  The Makefiles generate temporary Makefiles for this purpose, in
as consistent of a way as possible.

## Parameters

The microbenchmarks use the same command-line configuration object, with the
following arguments:

```text
  -b: # buckets                       (default 128)
  -i: seconds to run, or # ops/thread (default 5)
  -h: print this message              (default false)
  -k: key range                       (default 256)
  -r: lookup ratio                    (default 34%)
  -t: # threads                       (default 1)
  -v: toggle verbose output           (default false)
  -x: toggle 'i' parameter            (default true <timed mode>)
  -B: resize threshold                (default 7)
  -o: random prefill                  (default false)
  -s: snapshot frequency              (default 33)
  -l: max levels                      (default 32)
  -Q: quiet mode                      (default false)
  -T: #warm-up threads                (default 1)
```

Not all of these arguments are relevant to all data structures.  For example,
the number of buckets and the resize threshold are only relevant to unordered
maps.

Of particular interest, the `-x` flag changes the meaning of the `-i` flag.  The
default is that `-i` provides a number of seconds to run.  But when `-x` is
used, then `-i` means the number of operations to run in each thread.

Also, please note that `-o`, which randomizes the pre-filling of the data
structure, is an essential flag for large unbalanced trees, but should not be
used for lists.
