# encoding=utf8

import matplotlib as mpl
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np
import re
import argparse
import sys

######################### Box styles ##############################


def set_box_color(bp, options):
    colors = options["colors"]
    # "|||||||||", "\/\/\/\/\/\/", ""
    patterns = options["patterns"]
    double_counter = 0

    for i, box in enumerate(bp['boxes']):
        plt.setp(bp['boxes'][i], facecolor=colors[i],
                linewidth=0.5, hatch=patterns[i])

        plt.setp(bp['caps'][double_counter], color='black', linewidth=0.5)
        plt.setp(bp['caps'][double_counter + 1], color='black', linewidth=0.5)

        plt.setp(bp['whiskers'][double_counter],
                 color='black', linewidth=0.5, linestyle="--")
        plt.setp(bp['whiskers'][double_counter + 1],
                 color='black', linewidth=0.5, linestyle="--")

        plt.setp(bp['medians'][i], color='black')
        double_counter += 2


def plot(nums, axes, plot_positions, tick, options, line):

    stats = []
    micro_to_milli = 1000
    for num in nums:
        item = {}
        item["med"] = num[3]/micro_to_milli
        item["q1"] = num[2]/micro_to_milli
        item["q3"] = num[4]/micro_to_milli
        item["whislo"] = num[6]/micro_to_milli  # required
        item["whishi"] = num[1]/micro_to_milli  # required
        item["fliers"] = []  # required if showfliers=True
        stats.append(item)

    # print(plot_positions[0], plot_positions[1], stats)
    # axes.set_axis_off()
    box = axes.bxp(stats, positions=plot_positions,
                   widths=0.6, patch_artist=True)
    print(plot_positions)

    if line:
        axes.plot([tick], [stats[0]["med"]], marker='^',
                  markersize=0.2, linewidth=0.5, color="green")
        axes.plot([tick], [stats[1]["med"]], marker='v',
                  markersize=0.2, linewidth=0.5, color="green")
        axes.plot([tick, tick], [stats[0]["med"], stats[1]["med"]],
                  linewidth=0.3, color="green", linestyle="--")

    # print("{}     {}  {} {}".format(int(nums[0][0]), round(stats[0]["med"] / stats[3]["med"], 2), round(stats[1]["med"] / stats[3]["med"], 2), round(stats[2]["med"] / stats[3]["med"], 2)))
    # axes.text(tick, (item["med"] + item2["med"]) / 2, round(item["med"] / item2["med"], 2),
    #             fontsize=2.8, color="red", ha="center", va="center", fontweight="bold", backgroundcolor='white', bbox={'facecolor':'white', 'pad':0.01, 'edgecolor':'none'})

    set_box_color(box, options)


if __name__ == "__main__":

    font = {'family': 'Arial',
            'weight': 'normal',
            'size': 18}
            
    plt.rc('font', **font)

    def custom_list(s): return [int(item) for item in s.split(',')]
    def str_custom_list(s): return s.split(',')

    textFontSize = 9
    parser = argparse.ArgumentParser(
        description='Box and whisker plotting script for experiments.')
    parser.add_argument('--xlabel', default="No. of Procedures per second",
                        type=str, help='X-Axis label for the graph.')
    parser.add_argument('--ylabel', default="PCT (μs)",
                        type=str, help='Y-Axis label for the graph.')
    parser.add_argument('--regions', default=[], type=custom_list,
                        help='Regions after which you want to place delimiting line.')
    parser.add_argument('--legends', required=True, type=str_custom_list,
                        help='Legends for each boxplot comma separated')
    parser.add_argument('--colors', required=True, type=str_custom_list,
                        help='Colors for each boxplot comma separated')
    parser.add_argument('--patterns', required=True, type=str_custom_list,
                        help='Patterns for each boxplot comma separated')
    parser.add_argument('--output_image', default="image.png",
                        type=str, help='Path to output image.')
    parser.add_argument('--w_space', default=5, type=int,
                        help='Space to leave from the max value to the border, this value will be multipled by the max')
    parser.add_argument('-l', action='store_true', help="Flag which indicates whether to show improvement line or not.\
                                                        Default is false and does not work for more than two files.")

    parser.add_argument('paths', nargs='+',
                        help='Path to files.')

    args = parser.parse_args()

    total_files = len(args.paths)

    if args.l and total_files != 2:
        exit("-l option should only be used with two files.")

    options = {}
    options["colors"] = args.colors
    options["patterns"] = args.patterns
    options["legends"] = args.legends

    fig = plt.figure(figsize=(6, 2.5),  facecolor='w')
    axes = fig.add_subplot(111)
    mpl.rcParams['hatch.linewidth'] = 0.1

    ################################ Legends #########################################
    legends = []
    for i in range(total_files):
        legends.append(mpatches.Patch(linewidth=0.5, edgecolor='black',
                                    facecolor=options["colors"][i], alpha=0.6,
                                    hatch=options["patterns"][i], label=options["legends"][i]))

    lines = plt.legend(handles=legends, bbox_to_anchor=(0.18, 0.45, 0.80, 0.3),
                        loc="lower left", borderaxespad=0, ncol=1, fontsize=textFontSize)
                        

    #################################################################################

    x_labels = []

    fds = [open(path, 'r') for path in args.paths]
    plot_positions = [i*0.85 + 0.5 for i in range(total_files)]

    plot_pos_mean = np.mean(plot_positions)
    x_ticks = [plot_pos_mean]
    max_v = 0

    j = 0
    for files in list(zip(*fds)):

        nums = []
        for f in files:
            nums.append(np.array(re.split(r"\s+", f.strip())).astype(np.float))

        plot(nums, axes, plot_positions, x_ticks[-1], options, args.l)

        for i in range(len(plot_positions)):
            plot_positions[i] += (total_files)

        x_ticks.append(x_ticks[-1] + total_files)

        if j == 0:
            x_labels.append("Normal")
        if j == 1:
            x_labels.append("T-Straggler")
        if j == 2:
            x_labels.append("P-Straggler") 
        
        j += 1
        for num in nums:
            max_v = np.amax(np.array([max_v, num[6]]))

    axes.set_yscale('log')
    plt.grid(True, which="both", ls="-", linewidth=0.1)


    plt.xlim(0, 1)
    x_ticks[-1] -= (plot_pos_mean)
    x_ticks[-1] -= 0.75

    for xcoord in args.regions:
        plt.axvline(x=np.mean(np.array([x_ticks[xcoord - 1], x_ticks[xcoord]])), c='red', linewidth=0.5, linestyle="--")

    x_labels.append("")
    axes.set_xticks(x_ticks)
    axes.set_xticklabels(x_labels)
    axes.xaxis.set_tick_params(width=0.4)
    axes.yaxis.set_tick_params(width=0.4)
    axes.tick_params(labelsize=textFontSize)
    plt.xlabel(args.xlabel, fontsize=textFontSize)
    plt.ylabel(args.ylabel, fontsize=textFontSize)
    plt.ylim(0.1, 70000)



    plt.savefig(args.output_image, dpi=300, bbox_inches='tight', pad_inches = 0.05)
