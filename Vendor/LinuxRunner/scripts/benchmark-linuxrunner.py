#!/usr/bin/env python3
"""Rerunnable host CLI baselines; no iOS performance claim."""
import argparse, json, pathlib, platform, statistics, subprocess, time
p=argparse.ArgumentParser(); p.add_argument('--runtime',required=True); p.add_argument('--rootfs',required=True); p.add_argument('--runs',type=int,default=10); p.add_argument('--output',default='build/tests/benchmark.json'); a=p.parse_args()
workloads={'command-startup':['/bin/true'], 'shell-startup':['/bin/sh','-c','true'], 'filesystem':['/bin/sh','-ec','dd if=/dev/zero of=/tmp/lr-bench bs=65536 count=16; cp /tmp/lr-bench /tmp/lr-bench-copy; rm /tmp/lr-bench /tmp/lr-bench-copy']}
result={'platform':platform.platform(),'machine':platform.machine(),'runs':a.runs,'timing':'host elapsed seconds including runtime process creation','workloads':{}}
for name,command in workloads.items():
    samples=[]
    for _ in range(a.runs):
        begin=time.perf_counter(); subprocess.run([a.runtime,'-f',a.rootfs,*command],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=60); samples.append(time.perf_counter()-begin)
    result['workloads'][name]={'median':statistics.median(samples),'min':min(samples),'max':max(samples),'samples':samples}
path=pathlib.Path(a.output); path.parent.mkdir(parents=True,exist_ok=True); path.write_text(json.dumps(result,indent=2)); print(path)
