# Scripts

This folder contains Python scripts for running a standard set of microbenchmark
experiments and then producing charts to summarize the results.

The scripts are mostly declarative.  See below for details.

## Files

The first category of files are those that declare the experiments to run. These
culminate in `Targets.py`.  If one wishes to produce different charts, then it
will be necessary to change some of the declarations in these files.

* `Types.py`: A set of types that we use to describe the various components of
  an experiment / chart, such as an executable file, a data structure, an
  experiment's command-line arguments, how a line should look, chart-wide
  formatting, a chart curves, and a chart's components.
* `ExpCfg.py`: Instances of various `Types` that define the executables to test,
  the data structure configurations to test, and how to conduct each trial.
* `ChartCfg.py`: Instances of various `Types` that define the formatting and
  appearance of chart lines and axes.
* `Targets.py`: Combinations of the above instances into an object that
  describes all of the experiments to run and charts to build.

The second category of files are those that define the functions for running
experiments and producing charts.

* `GetData.py`: A function for getting all the data needed for a chart.  Each
  experiment result goes in its own file.  The function only runs experiments
  for which either (a) there is no data file, or (b) the data file is older than
  the executable.
* `MakeChart.py`: A function for making a chart file.  It is parameterized based
  on whether we want error bars or not.

Finally, there is a script called `Runner.py`, which uses the above functions
with the `all_targets` object from `Targets.py` to conduct experiments and build
charts.  Note that at the present time, `Runner.py` hard-codes paths to
executables and to output folders.  This means it must be run from the `scripts`
folder (i.e., `python3 Runner.py`).

Please note that the results of experiments will be saved to files, and
experiments will not be re-run if the corresponding file exists.  This is good
when reformatting charts, but please be sure to `make clean` if you wish to
re-run experiments.