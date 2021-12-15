from run import gem5Run
import os
import sys
from uuid import UUID
from itertools import starmap
from itertools import product
import multiprocessing as mp
import argparse

def worker(run):
    run.run()
    json = run.dumpsJson()
    print(json)

if __name__ == "__main__":

    parser = argparse.ArgumentParser()
    parser.add_argument('N', action="store",
                      default=1, type=int,
                      help = """Number of cores used for simulation""")
    args  = parser.parse_args()

    cpu_types = ['Minor4'] # 'Simple', 
    mem_types = ['Slow']
    l1_cachesizes =  ["32kB"] # ,"8kB","32kB","64kB"
    l2_cachesizes = ["1MB"] # "256kB", "512kB", "1MB"
                        # for l2size in l2_cachesizes:
                        # for clockspeed in clockspeeds:

    clockspeeds = ["1GHz"] # , "2GHz"

    dram_models = ["DDR3_2133_8x8"]

    prefetchers = ["StridePrefetcher", "TaggedPrefetcher", "STeMSPrefetcher", "SMSPrefetcher", "ChenBaerPrefetcher"] # SMSPrefetcher

    bm_list = []

    # iterate through files in microbench dir to
    # create a list of all microbenchmarks

    for filename in os.listdir('microbenchmark'):
        if os.path.isdir(f'microbenchmark/{filename}') and filename != '.git':
                bm_list.append(filename)

    jobs = []
    for bm in bm_list:
        for cpu in cpu_types:
            for mem in mem_types:
                for clockspeed in clockspeeds:
                    for dram_model in dram_models:
                        for prefetcher in prefetchers:
                            run = gem5Run.createSERun(
                                'microbench_tests',
                                os.getenv('M5_PATH')+'/build/X86/gem5.opt',
                                'gem5-config/run_micro.py',
                                'results/X86/run_micro/{}/{}/{}/{}/{}/{}/'.format(bm,cpu,mem, dram_model, clockspeed, prefetcher),
                                cpu,mem,os.path.join('microbenchmark',bm,'bench.X86'),dram_model, prefetcher, "--clock=" + clockspeed, )
                            jobs.append(run)

    with mp.Pool(args.N) as pool:
        pool.map(worker,jobs)

