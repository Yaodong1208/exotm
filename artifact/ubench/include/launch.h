#pragma once

/// A standardized main() function for use with all of our integer map
/// benchmarks
int main(int argc, char **argv) {
  // Parse and print the command-line options.  If it throws, terminate
  config_t *cfg = new config_t(argc, argv);
  cfg->report();

  // Create a bst and fill it
  auto me = new descriptor();
  auto ds = new map(me, cfg);
  fill_even<map, descriptor, K2VAL>(ds, cfg);

  // Launch the test
  intmap_test<map, descriptor, K2VAL>(ds, cfg);
}
