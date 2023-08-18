#
# This file defines all the data types that we will use to describe the set of
# experiments to run.
#


from typing import List


class ExeCfg:
    """
    Description of a benchmark executable
      exePath: The full path to the executable
      name:    A name to associate with that executable (used in output files)
    """

    def __init__(self, exePath: str, name: str) -> None:
        self.exePath = exePath
        self.name = name


class DsCfg:
    """
    Description of how to configure a data structure.  We expect this to be the
    same for all data structures presented in a single chart, but we do not
    strictly enforce that behavior, since there are situations where we might
    want to present the same ExeCfg twice on the same chart, using different
    DsCfgs.
      bucketSize:   The initial config of bucket numbers
      chunkSize:    The initial size of chunks
      resizeThresh: The number of elements before a resize should happen
      snapshotFreq: The frequency with which to take snapshots
      maxLevels:    The maximum number of levels
      name:         A name for this configuration (used in output files)
    """

    def __init__(self, bucketSize: int, chunkSize: int, resizeThresh: int, snapshotFreq: int, maxLevels: int, name: str):
        self.bucketSize = bucketSize
        self.chunkSize = chunkSize
        self.resizeThresh = resizeThresh
        self.snapshotFreq = snapshotFreq
        self.maxLevels = maxLevels
        self.name = name


class ExpCfg:
    """
    Description of how to run an experiment.  All data structures presented in a
    single chart should use the same ExpCfg.  In general, we wouldn't expect
    much other than fillRand and fillThreads to differ even across charts.
      seconds:     Number of seconds for which to run
      threads:     Array of thread counts
      fillRand:    Should the prefilling be done randomly (vs reverse order)
      fillThreads: How many threads should do the prefilling?
      trials:      How many trials to run
      keyRange:    The range for keys
      lookupRatio: The percentage of operations that should be lookups
      machine:     declair which machine we use
      name:        A name for this configuration (used in output files)
    """

    def __init__(self, seconds: int,  threads: int,  fillRand: int, fillThreads: int, trials: int, keyRange: int, lookupRatio: int, machine: str, name: str):
        self.seconds = seconds
        self.threads = threads
        self.fillRand = fillRand
        self.fillThreads = fillThreads
        self.trials = trials
        self.keyRange = keyRange
        self.lookupRatio = lookupRatio
        self.machine = machine
        self.name = name + '_' + machine


class LineCfg:
    """
    Description of how a line in a chart should appear
      color:      The color of this line
      linestyle:  The style for line between glyphs
      marker: The style for the marker
    """

    def __init__(self, color: str,  linestyle: str, marker: str):
        self.color = color
        self.linestyle = linestyle
        self.marker = marker


class CurveCfg:
    """
    Description of one of the lines in a chart
      exeCfg:  The executable that is used to generate the data for this line
      dsCfg:   The rules for how to configure the exe's data structure
      lineCfg: The description of how to draw this line
      label:   The label to use for this curve
    """

    def __init__(self, exeCfg: ExeCfg, dsCfg: DsCfg, lineCfg: LineCfg, label: str):
        self.exeCfg = exeCfg
        self.dsCfg = dsCfg
        self.lineCfg = lineCfg
        self.label = label


class ChartCfg:
    """
    All of the information needed to run experiments and produce a single chart
      curves:   An array with benchmarks to run and rules for drawing their results
      expCfg:   Rules for how to run the experiment
      xLabel:   The x axis label
      yLabel:   The y axis label
      name:     The name for this chart (output filename)
    """

    def __init__(self, curves: List, expCfg: ExpCfg, xLabel: str, yLabel: str, name: str):
        self.curves = curves
        self.expCfg = expCfg
        self.xLabel = xLabel
        self.yLabel = yLabel
        self.name = name
