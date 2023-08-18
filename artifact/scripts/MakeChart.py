import numpy as np
import matplotlib.ticker as ticker
from matplotlib.ticker import FuncFormatter
import matplotlib
import json
import functools
import matplotlib.pyplot as plt
import Types
import Util


def makeChart(chart: Types.ChartCfg, dataFolder: str, chartFolder: str, errbar: bool):
    """
    Make the chart described by `chart`
    """
    
    matplotlib.rcParams.update({'font.size': 18}) # change font size

    formatnum_n = []
    for i in range(0, 10):
        def make_formatnum(i=i):
            def formatnum(x, pos):
                return '$%.1f$x$10^{%i}$' % (x/(10**i), i)
            return formatnum
        formatnum_n.append(make_formatnum())
    chart_conf = {}
    chart_conf['list_64_wo'] = [FuncFormatter(formatnum_n[7]), (5.6,0.5), 4]
    chart_conf['list_64'] = [FuncFormatter(formatnum_n[7]), (5.6,0.5), 4]
    chart_conf['list_1K_wo'] = [FuncFormatter(formatnum_n[7]), (6,0.5), 4]
    chart_conf['list_1K'] = [FuncFormatter(formatnum_n[7]), (6,0.5), 4]
    chart_conf['sl_64K_wo'] = [FuncFormatter(formatnum_n[8]), (3,0.5), 4]
    chart_conf['sl_64K'] = [FuncFormatter(formatnum_n[8]), (3,0.5), 4]
    chart_conf['sl_1M_wo'] = [FuncFormatter(formatnum_n[8]), (3,0.5), 4]
    chart_conf['sl_1M'] = [FuncFormatter(formatnum_n[8]), (3,0.5), 4]
    chart_conf['umap_1M_wo'] = [FuncFormatter(formatnum_n[8]), (4.5,0.5), 3]
    chart_conf['umap_1M'] = [FuncFormatter(formatnum_n[8]), (4.5,0.5), 3]
    chart_conf['bst_64K_wo'] = [FuncFormatter(formatnum_n[8]), (4.5,0.5), 4]
    chart_conf['bst_64K'] = [FuncFormatter(formatnum_n[8]), (4.5,0.5), 4]
    chart_conf['bst_1M_wo'] = [FuncFormatter(formatnum_n[8]), (4.5,0.5), 4]
    chart_conf['bst_1M'] = [FuncFormatter(formatnum_n[8]), (4.5,0.5), 4]
    chart_conf['bbst_64K_wo'] = [FuncFormatter(formatnum_n[8]), (4.2,0.5), 4]
    chart_conf['bbst_64K'] = [FuncFormatter(formatnum_n[8]), (4.2,0.5), 4]
    chart_conf['bbst_1M_wo'] = [FuncFormatter(formatnum_n[8]), (4.2,0.5), 4]
    chart_conf['bbst_1M'] = [FuncFormatter(formatnum_n[8]), (4.2,0.5), 4]
    # Ensure the chart folder exists
    Util.makeDir(chartFolder)

    # fetch data from files, process it into throughput and variance
    throughput = {}
    variance = {}
    for curve in chart.curves:
        throughput[curve] = []
        variance[curve] = []
        for thread in chart.expCfg.threads:
            throughput_samples = []
            for trial in range(chart.expCfg.trials):
                # open the file, read it
                file_name = Util.makeDataFileName(
                    curve, chart, thread, (trial+1))
                file_path = Util.makeDataFilePath(dataFolder, file_name)
                with open(file_path, 'r') as f:
                    throughput_samples.append(json.load(f))
            throughput[curve].append(np.mean(throughput_samples))
            variance[curve].append(np.std(throughput_samples, ddof=1))

    # Set up the chart object (no curves yet)

    # compute domain and range, set up the axes
    width = chart.expCfg.threads[-1] + 1
    height = functools.reduce(lambda a, b: max(a, b), map(
        lambda x: max(x), list(throughput.values()))) * 1.2
    plt.axis([0, width, 0, height])
    ax = plt.gca()
    ax.grid(True, linestyle='--')
    ax.xaxis.set_major_locator(ticker.AutoLocator())
    ax.xaxis.set_minor_locator(ticker.AutoMinorLocator())

    ax.yaxis.set_major_formatter(chart_conf[chart.name][0])
    ax.yaxis.set_minor_locator(ticker.AutoMinorLocator())
    # plt.ticklabel_format(style='sci', axis='y',
    #                      scilimits=(0, 0), useMathText=True)

    # Start plotting throughput, possibly with error bars
    for curve_throughput in throughput.items():
        # Note that we're pretending that the x axis is 1, 2, 3, ..., and we
        # override the labels with the real thread count.
        if errbar == False:
            plt.plot(
                chart.expCfg.threads, curve_throughput[1],
                color=curve_throughput[0].lineCfg.color,
                linestyle=curve_throughput[0].lineCfg.linestyle,
                marker=curve_throughput[0].lineCfg.marker,
                label=curve_throughput[0].label,
                markersize=6)
        else:
            plt.errorbar(
                chart.expCfg.threads, curve_throughput[1], variance[curve_throughput[0]],
                color=curve_throughput[0].lineCfg.color,
                linestyle=curve_throughput[0].lineCfg.linestyle,
                marker=curve_throughput[0].lineCfg.marker,
                label=curve_throughput[0].label)

    # Set the axis labels
    plt.xlabel(chart.xLabel)
    plt.ylabel(chart.yLabel)
    plt.tight_layout()

    # Save the chart
    if errbar:
        plt.savefig(r"%s%s_variance.png" % (chartFolder, chart.name))
    else:
        plt.savefig(r"%s%s.png" % (chartFolder, chart.name))

    # Add a legend, extract it, and save it to a legend file
    matplotlib.rcParams.update({'font.size': 8})
    fig_leg = plt.figure(figsize = chart_conf[chart.name][1])
    ax_leg = fig_leg.add_subplot(111)
    col_num = chart_conf[chart.name][2]#len(chart.curves)
    # if len(chart.curves) >= 4:
    #     col_num /= 2
    ax_leg.legend(*ax.get_legend_handles_labels(), loc='center', ncol = col_num)
    ax_leg.axis('off')
    if errbar:
        fig_leg.savefig(r"%s%s_variance_legend.png" %
                        (chartFolder, chart.name))
    else:
        fig_leg.savefig(r"%s%s_legend.png" % (chartFolder, chart.name))
    plt.close('all')
