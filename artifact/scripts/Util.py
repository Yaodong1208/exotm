import os
import errno

from Types import ChartCfg, CurveCfg


def makeDir(d: str):
    """Create a directory with the name given by `d`"""
    if not os.path.exists(os.path.dirname(d)):
        # Races to create the folder are OK, others aren't
        try:
            os.makedirs(os.path.dirname(d))
        except OSError as exc:
            if exc.errno != errno.EEXIST:
                raise


def makeDataFileName(curve: CurveCfg, chart: ChartCfg, thread, trial):
    """Create the name for a data file, given the curve, chart, thread count, and trial number"""
    return r'%s_%s_%s_%i_%i' % (curve.exeCfg.name, curve.dsCfg.name, chart.expCfg.name, thread, trial)


def makeDataFilePath(folder: str, name: str):
    """Create the full path to a data file from a folder and a file name"""
    return r'%s%s' % (folder, name)


def makeExeName(exe_path: str, chart: ChartCfg, curve: CurveCfg, thread: int):
    """Create the full command for executing a benchmark, from its path, the chart config, the curve config, and the thread count"""
    return r'./%s -b %i -c %i -i %i -k %i -r %i -t %i -B %i -o %i -s %i -l %i -T %i -Q' % (
        exe_path, curve.dsCfg.bucketSize, curve.dsCfg.chunkSize, chart.expCfg.seconds, chart.expCfg.keyRange, chart.expCfg.lookupRatio, thread, curve.dsCfg.resizeThresh, chart.expCfg.fillRand, curve.dsCfg.snapshotFreq, curve.dsCfg.maxLevels, chart.expCfg.fillThreads)
