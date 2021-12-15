# Set the absolute path in 

import numpy as np
import pandas as pd
from matplotlib import pyplot as plt
import matplotlib.patches as mpatches
import os

if not os.getenv("LAB_PATH"):
    print("Set Lab Path\n")
    exit(1)


datadir = os.getenv("LAB_PATH") + '/results/X86/run_micro'

def gem5GetStat(filename, stat):
    filename = os.path.join(datadir, '', filename, 'stats.txt').replace('\\','/')
    with open(filename) as f:
        r = f.read()
        if len(r) < 10: return 0.0
        if (r.find(stat) != -1) :
            start = r.find(stat) + len(stat) + 1
            end = r.find('#', start)
            #print(r[start:end])
            return float(r[start:end])
        else:
            return float(0.0)
all_arch = ['X86']
plt_arch = ['X86']


# all_memory_models = ['SingleCycle', 'Inf', 'Slow']
# plt_memory_models = ['Slow']


# all_gem5_cpus = ['Simple','DefaultO3','Minor4', 'O3_W256', 'O3_W2K']
# plt_gem5_cpus = ['Simple', 'Minor4']

benchmarks = ['CCa',   'CCl',   'DP1f',  'ED1',  'EI', 'MI', "STREAM"]

# l1_cachesizes =  ["4kB","8kB","32kB","64kB"]
# l2_cachesizes = ["128kB", "256kB", "512kB", "1MB"]

# clockspeeds = ["1GHz", "2GHz"]

# dram_models = ["DDR3_2133_8x8"]

cpu_types = ['Minor4'] # 'Simple', 
mem_types = ['Slow']
l1_cachesizes =  ["32kB"] # ,"8kB","32kB","64kB"
l2_cachesizes = ["1MB"] # "256kB", "512kB", "1MB"
                    # for l2size in l2_cachesizes:
                    # for clockspeed in clockspeeds:

clockspeeds = ["1GHz"] # , "2GHz"

dram_models = ["DDR3_2133_8x8"]

prefetchers = ["StridePrefetcher", "TaggedPrefetcher", "STeMSPrefetcher", "SMSPrefetcher"] # SMSPrefetcher

rows = []
for bm in benchmarks: 
    for cpu in cpu_types:
        for mem in mem_types:
                for clockspeed in clockspeeds:
                    for dram_model in dram_models:
                        for prefetcher in prefetchers:
                            rows.append([bm,cpu,mem,dram_model,clockspeed, prefetcher,            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/", 'sim_ticks')/333,
                            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/", 'sim_insts'),
                            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/", 'sim_ops'),
                            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/", 'sim_ticks')/1e9,
                            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/", 'host_op_rate'),
                            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/",'system.mem_ctrl.dram.avgMemAccLat'),
                            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/",'system.mem_ctrl.dram.busUtil'),
                            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/",'system.mem_ctrl.dram.bw_total::total'),
                            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/",'system.mem_ctrl.dram.totBusLat'),
                                                                            #memory with store
                            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/",'system.mem_ctrl.dram.avgWrBW'),
                            gem5GetStat(datadir+"/"+bm+"/"+cpu+"/"+mem+"/"+dram_model+"/"+clockspeed+"/"+prefetcher+"/",'system.cpu.ipc')
                            ])

df = pd.DataFrame(rows, columns=['benchmark','cpu', 'mem', 'dram_model', 'clockspeed', 'prefetcher', 'cycles','instructions', 'Ops', 'Ticks','Host', 'avgmemaccesslatency','busutilit','bandwidthtotal','totalbuslatency',                                       'averagewritebandwidth', 'cpuGem5IPC'])
df['ipc'] = df['instructions']/df['cycles']
df['cpi']= 1/df['ipc']
print(df)
df.to_csv("prefetchstats.csv")

# def draw_vertical_line(ax, xpos, ypos):
#     line = plt.Line2D([xpos, xpos], [ypos + .1, ypos],
#                       transform=ax.transAxes, color='black', lw = 1)
#     line.set_clip_on(False)
#     ax.add_line(line)

# def doplot_benchmarks(benchmarks,stat,norm=True):
#     fig = plt.figure()
#     ax = fig.add_subplot(1,1,1)
#     i = 0
#     seenSystems = []
#     handles = []
#     for bm in benchmarks:
#         base = df[(df['benchmark']==bm)][stat].iloc[0] if norm else 1
#         for j,sys in enumerate(mem_types):
#             d = df[(df['mem']==sys) & (df['benchmark']==bm)]
#             print(d)
#             ax.bar(i, d[stat].iloc[0]/base, color='C'+str(j))
#             i += 1
#             if not sys in seenSystems:
#                 handles.append(mpatches.Patch(color='C'+str(j), label=sys))
#                 seenSystems.append(sys)
#         i += 1
#     for i,sys in enumerate(plt_gem5_cpus):
#         plt.bar(0,0,color='C'+str(i), label=sys)
#     new_names = benchmarks 
#     # Arranging ticks on the X axis
#     plt.xticks(np.arange(len(new_names))*(len(plt_gem5_cpus)+3.4)+i/2, new_names, rotation=40, ha='right')
#     draw_vertical_line(ax, 0, -0.1)
#     draw_vertical_line(ax, 0.347, -0.1)
#     draw_vertical_line(ax, 0.66, -0.1)
#     draw_vertical_line(ax, 1, -0.1)
#     ax.legend(loc="upper right", handles=handles)
#     return handles
# #    ax.text(0.25, -0.25, size[0], ha ='center', transform=ax.transAxes)
# #    ax.text(0.5, -0.25, size[1], ha ='center', transform=ax.transAxes)
# #    ax.text(0.75, -0.25, size[2], ha ='center', transform=ax.transAxes)




# fig_size = plt.rcParams["figure.figsize"]
# fig_size[0] = 10
# fig_size[1] = 5
# plt.rcParams["figure.figsize"] = fig_size
# handles = doplot_benchmarks(benchmarks,"ipc",norm=False)
# plt.ylabel('IPC')
# plt.legend(loc=2, prop={'size': 8}, handles=handles)
# plt.title('1C vs Inf vs Slow (CPU: O3_W256)')
# plt.tight_layout()
# plt.savefig('CACHE_O3_W256.png', format='png', dpi=600)



