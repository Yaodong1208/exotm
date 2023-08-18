import os
import json

import Types
import Util


def getData(chart: Types.ChartCfg, dataFolder: str, ubenchFolder: str):
    """
    Get the data needed in order to produce `chart`
    """
    # Ensure the data folder exists
    Util.makeDir(dataFolder)

    print("collecting data for chart " + chart.name)

    # `chart` consists of a bunch of curves, each of which consists of averages
    # of multiple data points for a {executable, config} pair.  For each curve,
    # we need to make sure our data is fresher than the executable's last
    # modified time.
    for curve in chart.curves:
        # Fail if the executable cannot be found, otherwise log its time
        exe_path = r'%s%s' % (ubenchFolder, curve.exeCfg.exePath)
        if not os.path.exists(exe_path):
            print('%s doesn\'t exist, make it before retry' % exe_path)
            exit()
        exe_time = os.path.getmtime(exe_path)

        # For each trial, we're going to need to run the experiment for each
        # thread count
        for trial in range(chart.expCfg.trials):
            for thread in chart.expCfg.threads:
                print(".", end="", flush=True)
                print("exe_name: %s, thread: %d, trial: %d" % (curve.exeCfg.name, thread, trial))
                # We produce one file for each run of the executable, and we use all
                # of the mnemonics to produce the file name.  Make sure the trial
                # number is 1-based.
                file_name = Util.makeDataFileName(
                    curve, chart, thread, (trial+1))
                # If the file exists and is newer than the exe time, don't re-run the experiment
                file_path = Util.makeDataFilePath(dataFolder, file_name)
                if not os.path.exists(file_path) or os.path.getmtime(file_path) < exe_time:
                    cmd = Util.makeExeName(exe_path, chart, curve, thread)
                    # Given the `-Q` flag, the entire output of a successful run
                    # will be a single number.  Get it and put it in the file.
                    result = os.popen(cmd).readline()
                    with open(file_path, 'w') as f:
                        json.dump(float(result), f)
    print("")
