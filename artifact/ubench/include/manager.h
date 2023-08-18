#pragma once

#include <atomic>
#include <chrono>
#include <signal.h>

#include "bench_thread_context.h"

/// experiment_manager keeps track of all data that we measure during an
/// experiment, and any data we use to manage the execution of the experiment
struct experiment_manager_t {
  using event_types = bench_thread_context_t::EVENTS;
  using time_point = std::chrono::high_resolution_clock::time_point;

  std::atomic<uint32_t> barriers[3]; // barriers for coordinating threads
  time_point start_time;             // start time of the experiment
  time_point end_time;               // end time of the experiment
  std::atomic<uint64_t> stats[event_types::NUM]; // global stat counters
  std::atomic<bool> running; // flag for stopping timed experiments

  /// Static reference to singleton instance of this struct... we need this for
  /// the experiment timer
  static experiment_manager_t *instance;

  /// Construct the global context by initializing the barriers and zeroing the
  /// counters
  experiment_manager_t() : running(true) {
    experiment_manager_t::instance = this;
    for (int i = 0; i < 3; ++i)
      barriers[i] = 0;
    for (int i = 0; i < event_types::NUM; ++i)
      stats[i] = 0;
  }

  /// Report the most essential configuration settings and statistics that we
  /// counted as a comma separated line
  void report_csv() {
    using namespace std::chrono;

    // Report throughput, execution time, and operations completed
    uint64_t ops = count_operations();
    auto dur = duration_cast<duration<double>>(end_time - start_time).count();
    std::cout << "(tput, time, ops), " << ops / dur << ", " << dur << ", "
              << ops << ", ";
  }

  /// Only report throughput, nothing else
  void report_tput_only() {
    using namespace std::chrono;

    // Report throughput, execution time, and operations completed
    uint64_t ops = count_operations();
    auto dur = duration_cast<duration<double>>(end_time - start_time).count();
    std::cout << ops / dur;
  }

  /// Report all of the statistics that we counted, in a human-readable form
  void report_verbose() {
    using namespace std::chrono;

    // Report throughput, execution time, and operations completed
    uint64_t ops = count_operations();
    auto dur = duration_cast<duration<double>>(end_time - start_time).count();
    std::cout << "Throughput: " << ops / dur << "\n"
              << "Execution Time: " << dur << "\n"
              << "Operations: " << ops << "\n";

    std::string titles[] = {"lookup hit",  "lookup miss", "insert hit",
                            "insert miss", "remove hit",  "remove miss",
                            "modify hit",  "modify miss", "range hit",
                            "range miss",  "transactions"};
    for (size_t i = 0; i < event_types::NUM; ++i)
      std::cout << "  " << titles[i] << " : " << stats[i] << "\n";
  }

  /// Report all of the statistics that we counted
  void report(config_t *cfg) {
    if (cfg->quiet) {
      report_tput_only();
      return;
    }
    report_csv();
    std::cout << "\n";
    if (cfg->verbose) {
      report_verbose();
    }
  }

  /// Before launching experiments, use this to ensure that the threads start at
  /// the same time.  This uses two barriers internally, with a timer read
  /// between the first and second, so that we don't read the time while threads
  /// are still being configured, but we do ensure we read it before any work is
  /// done
  void sync_before_launch(size_t id, config_t *cfg) {
    // Barrier #1: ensure everyone is initialized
    barrier(0, id, cfg);
    // Now get the time
    if (id == 0) {
      start_time = std::chrono::high_resolution_clock::now();
      if (cfg->timed_mode) {
        signal(SIGALRM, experiment_manager_t::stop_running);
        alarm(cfg->interval);
      }
    }
    // Barrier #2: ensure we have the start time before work begins
    barrier(1, id, cfg);
  }

  /// Method used to stop test execution.
  static void stop_running(int signal) {
    experiment_manager_t::instance->running.store(false);
  }

  /// After threads finish the experiments, use this to have them all wait
  /// before getting the stop time.
  void sync_after_launch(size_t id, config_t *cfg) {
    // wait for all threads
    barrier(2, id, cfg);

    // now get the time
    if (id == 0)
      end_time = std::chrono::high_resolution_clock::now();
  }

  /// Arrive at one of the barriers.
  void barrier(size_t which, size_t id, config_t *cfg) {
    barriers[which]++;
    while (barriers[which] < cfg->nthreads) {
    }
  }

  /// Get a count of the number of operations that were completed.
  /// Note: this is brittle, be sure to update this when introducing a new
  /// operation type.
  uint64_t count_operations() {
    return stats[event_types::GET_T] + stats[event_types::GET_F] +
           stats[event_types::INS_T] + stats[event_types::INS_F] +
           stats[event_types::RMV_T] + stats[event_types::RMV_F];
  }
};

// Provide a definition to go along with the declaration of the singleton
// instance.
//
// NB: this would break if we included this file in more than one .cc file
experiment_manager_t *experiment_manager_t::instance;
