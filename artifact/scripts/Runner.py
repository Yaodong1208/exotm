import Targets
import GetData
import MakeChart

dataFolder = "./data/"          # Destination folder for data files
chartFolder = "./charts/"       # Destination folder for charts w/o error bars
varianceFolder = "./variance/"  # Destination folder for charts w/ error bars
ubenchFolder = "../ubench/"     # Source folder for benchmark executables

# Now that the paths are set, we can make all the charts
for c in Targets.all_targets:
    GetData.getData(c, dataFolder, ubenchFolder)
    MakeChart.makeChart(c, dataFolder, chartFolder, False)
    MakeChart.makeChart(c, dataFolder, varianceFolder, True)
