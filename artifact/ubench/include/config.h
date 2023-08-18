#pragma once

#include <iostream>
#include <libgen.h>
#include <unistd.h>

/// config_t encapsulates all of the configuration behaviors that we require of
/// our benchmarks.  It standardizes the format of command-line arguments,
/// parsing of command-line arguments, and reporting of command-line arguments.
/// Note that config_t is a superset of everything that our individual
/// benchmarks need.
///
/// The purpose of config_t is just to reduce boilerplate code.  We aren't
/// concerned about good object-oriented design, so everything is public.
struct config_t {
  size_t interval = 1;    // # seconds to run for, or # operations per thread
  bool timed_mode = true; // is `interval` a time (true) or # transactions
  size_t chunksize = 2;   // size of chunks, for chunked data structures
  size_t iChunksize = 2;  // size of index chunks, for chunked data structures
  size_t key_range = 256; // The range for keys in maps or for elements in sets
  size_t nthreads = 1;    // Number of threads that should run the benchmark
  size_t lookup = 34;     // % lookups.  inserts/removes evenly split the rest
  size_t buckets = 1048576; // # buckets for closed addressing unordered maps
  bool verbose = false;     // Print verbose output?
  size_t resize_threshold = 65536; // resize threshold of the buckets
  bool prefill_rand = false; // 0 to pre-fill in descending order, 1 for random
  std::string program_name;  // The name of the executable
  size_t snapshot_freq = 33; // The frequency with which to take snapshots
  size_t max_levels = 32;    // Max number of index levels in skiplists
  float merge_threshold = 1; // Merge threshold for chunked data structures
  size_t wthreads = 1;       // Number of warm-up threads
  bool quiet = false;        // Skip all output except the throughput?
  size_t bulk = 1;           // maxium number of opeartions in one transaction
  size_t orec_size = 65536;
  /// Initialize the program's configuration by setting the strings that are not
  /// dependent on the command-line
  config_t() {}
  config_t(int argc, char **argv) : program_name(basename(argv[0])) {
    long opt;
    while ((opt = getopt(argc, argv, "b:c:hi:l:k:or:s:t:vxB:QT:m:I:K:")) !=
           -1) {
      switch (opt) {
      case 'b':
        buckets = atoi(optarg);
        break;
      case 'c':
        chunksize = atoi(optarg);
        break;
      case 'I':
        iChunksize = atoi(optarg);
        break;
      case 'i':
        interval = atoi(optarg);
        break;
      case 'k':
        key_range = atoi(optarg);
        break;
      case 'o':
        prefill_rand = true;
        break;
      case 'h':
        usage();
        break;
      case 'l':
        max_levels = atoi(optarg);
        break;
      case 'm':
        merge_threshold = atof(optarg);
        break;
      case 'r':
        lookup = atoi(optarg);
        break;
      case 's':
        snapshot_freq = atoi(optarg);
        break;
      case 't':
        nthreads = atoi(optarg);
        break;
      case 'v':
        verbose = !verbose;
        break;
      case 'x':
        timed_mode = !timed_mode;
        break;
      case 'B':
        resize_threshold = atoi(optarg);
        break;
      case 'Q':
        quiet = true;
        break;
      case 'T':
        wthreads = atoi(optarg);
        break;
      case 'K':
        bulk = atoi(optarg);
        break;
      default:
        throw "Invalid configuration flag " + std::to_string(opt);
      }
    }
  }

  /// Usage() reports on the command-line options for the benchmark
  void usage() {
    std::cout
        << program_name << "\n"
        << "  -b: # buckets                       (default 128)\n"
        << "  -c: (data) chunk size               (default 8)\n"
        << "  -i: seconds to run, or # ops/thread (default 5)\n"
        << "  -h: print this message              (default false)\n"
        << "  -k: key range                       (default 256)\n"
        << "  -r: lookup ratio                    (default 34%)\n"
        << "  -t: # threads                       (default 1)\n"
        << "  -v: toggle verbose output           (default false)\n"
        << "  -x: toggle 'i' parameter            (default true <timed mode>)\n"
        << "  -B: resize threshold                (default 7)\n"
        << "  -o: random prefill                  (default false)\n"
        << "  -s: snapshot frequency              (default 33)\n"
        << "  -l: max levels                      (default 32)\n"
        << "  -m: merge threshold                 (default 1.0)\n"
        << "  -Q: quiet mode                      (default false)\n"
        << "  -T: #warm-up threads                (default 1)\n"
        << "  -I: (index) chunk size              (default 8)\n"
        << "  -K: number of #ops per transaction  (default 1)\n";
  }

  /// Report the current values of the configuration object as a CSV line
  void report() {
    if (quiet)
      return;
    std::cout << program_name << ", (bcikrtxBoslmTIK), " << buckets << ", "
              << chunksize << ", " << interval << ", " << key_range << ", "
              << lookup << ", " << nthreads << ", " << timed_mode << ", "
              << resize_threshold << ", " << prefill_rand << ", "
              << snapshot_freq << ", " << max_levels << ", " << merge_threshold
              << ", " << wthreads << ", " << iChunksize << ", " << bulk << ", ";
  }
};
