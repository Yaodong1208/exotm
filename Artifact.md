# Artifact: exoTM/STMCAS Mechanisms, Policies, and Data Structures

## Abstract

This artifact consists of synchronization libraries and data structures for
evaluating the performance of the exoTM synchronization mechanism and STMCAS
synchronization policy.  It consists of synchronization libraries, data
structure implementations, and microbenchmarks for stress-testing those data
structures.  The code requires an Intel CPU with support for the `rdtscp`
instruction, which has been available on most Intel CPUs for more than 10 years.
For the most meaningful evaluation, a system with a large number of cores is
recommended.  The provided Dockerfile handles all of the necessary software
dependencies.

## Description

This repository consists of the following components:

* Synchronization Policies (`artifact/policies`)
* Data Structures (`artifact/ds`)
* Microbenchmarks (`artifact/ubench`)
* Evaluation Scripts (`artifact/scripts`)
* Build Environment (`Docker`)

### Synchronization Policies

This artifact considers five synchronization policies

* Compiler-based STM (xSTM)
* Hand-instrumented STM (handSTM)
* Software Transactional Multiword Compare and Swap (STMCAS)
* handSTM+STMCAS (hybrid)
* Traditional blocking/nonblocking approaches (baseline)

Each synchronization policy can be found in a subfolder of `artifact/policies`.
Most policies are "header-only" C++ files, which do not require special
compilation.  The exception is xSTM, for which we provide a version of the
llvm-transmem TM plugin for C++.

### Data Structures

This artifact includes several data structures implemented with STMCAS
(doubly-linked list, skip list, singly-linked list, closed addressing resizable
unordered map, binary search tree, red/black tree).  As appropriate, these data
structures are also provided for other synchronization policies.  The `ds`
folder holds all data structures.  The subfolders of `ds` correspond to the
different synchronization policies.

### Microbechmarks

The artifact's microbenchmark harness runs a stress test microbenchmark.  The
microbenchmark has a variety of configuration options, some related to the data
structure's configuration (e.g., initial size of the unordered map), others
related to the experiment's configuration (e.g., operation mix, number of
threads).

### Build Environment

The easiest way to set up an appropriate build environment is to build a Docker
container.  The included `Dockerfile` has instructions for building an
appropriate container.  The dependencies are relatively minimal:

* Ubuntu 22.04
* Clang++ 15
* CMake (only needed for xSTM)
* Standard Linux build tools
* Standard Python3 charting tools

## Hardware Dependencies

This artifact has been tested on a system with 192 GB of RAM and two Intel Xeon
Platinum 8160 CPUs (48 threads / 96 cores), running Ubuntu 22.04.  In general,
any modern x86 CPU should work.  The exoTM/STMCAS codes do not require many
advanced x86 features.  The most noteworthy is the `rdtscp` instruction, which
has been available in most Intel processors for over a decade.

Please note that the baseline data structures based on the PathCAS
synchronization methodology require support for Intel TSX.  If you do not have a
machine with TSX support, you will need to comment out lines 112/113 and 138/139
in `artifact/scripts/Targets.py`.  Otherwise the automated testing/charting
scripts will fail.

## Software Dependencies

This artifact was developed and tested on Linux systems, running a variety of
kernel versions.  The xSTM policy that we compare against requires Clang 15, so
we have opted to use Clang throughout the artifact.  Our build configuration
uses the `-std=c++20` flag, but we do not require any particularly advanced
features (e.g., no concepts or coroutines).  For exoTM/STMCAS, any modern C++
compiler should be satisfactory.

## Data Sets

The artifact does not require any special data sets.

## Instructions for Repeating the Experiments in the Paper

If you wish to repeat the experiments from our paper, follow these instructions:

1. Check out this repository (`git clone git@github.com:Yaodong1208/exotm.git`)
2. Build the Docker image (`cd Docker && sudo docker build -t exotm_ae . && cd ..`)
3. Launch a container (`sudo docker run --privileged --rm -v $(pwd):/root -it exotm_ae`)
4. Build and run (`make`)

Please note that the Docker image will require roughly 1.7 GB of disk space.  To
check out and build the source code will require another 60 MB.

Also note that you will probably want to run a parallel make command in step 4
(e.g., `make -j 16`).

### Experiment Workflow

The top-level Makefile first builds all necessary executable files.  Please see
the README.md files in subfolders for more details.  In general, each data
structure will produce its own executable.

Once all executables are built, the Makefile will invoke `scripts/Runner.py` to
collect data and plot charts.  For the charts in the paper, this script took
about 6 hours to run, and required about 1GB of space to store the charts and
data files.

When the script completes, the `scripts/data` folder will hold all results.  The
charts can be found in the `scripts/charts` folder.  A second set of charts,
with error bars, can be found in `scripts/variance`.

Note that typing `make clean` will remove all build artifacts and also all
experiment results and charts.

## Instructions for Reusing the Artifact (Adding New Data Structures)

Below we discuss the process one can use to add new data structures.

1. Create a new `.h` file with the implementation of the data structure.  This
   should go in the appropriate sub-folder of `artifact/ds`, based on the
   synchronization policy used by the data structure.
2. Create a new `.cc` file in the appropriate sub-folder of `artifact/ubench`,
   depending on the synchronization policy used by the data structure.  Note
   that these files are typically quite small (~7 lines), as they only include
   other files, define some types, and invoke a policy's initializer.
3. In the same folder as the `.cc` file, add the `.cc` file's name (without an
   extension) to the `DS` variable in the `common.mk` file.  Typing `make`
   should now build a version of the microbenchmark for testing the new data
   structure.  Under rare circumstances, the Makefile might issue a warning
   about duplicate rules in the generated `rules.mk` file.  Should this happen,
   type `make clean` and then `make` (or `make -j 16`, for a parallel build).
4. To integrate the new data structure into the test scripts for an existing
   chart, first add it to the `exeNames` listing in
   `artifact/scripts/ExpCfg.py`.  Then locate the chart(s) to augment in
   `artifact/scripts/Targets.py` and add a new `Curve` with a matching
   `exeName`.

