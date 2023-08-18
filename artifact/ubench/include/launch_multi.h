#pragma once

/// A standardized main() function for use with all of our integer map
/// benchmarks
int main(int argc, char **argv) {
  // Parse and print the command-line options.  If it throws, terminate
  config_t *cfg = new config_t(argc, argv);
  cfg->report();

  // Create a bst and fill it
  auto me = new descriptor();
  map *ds1 = new map(me, cfg);
  map *ds2 = new map(me, cfg);
  fill_even<map, descriptor, K2VAL>(ds1, ds2, cfg);

  // Launch the test
  // intmap_test<map, descriptor, K2VAL>(ds, cfg);
}
