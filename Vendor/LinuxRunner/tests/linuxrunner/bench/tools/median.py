#!/usr/bin/env python3
# median.py LABEL CMD... — run CMD 5 times, print median ns_per_iter per workload and check consistency.
import os, subprocess, sys, statistics, re, json
label, cmd = sys.argv[1], sys.argv[2:]
runs = {}
checks = {}
for r in range(5):
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    for m in re.finditer(r'BENCH (\S+)\s+iters=(\d+)\s+ms=\s*([\d.]+) ns_per_iter=\s*([\d.]+) check=(\w+)', out):
        runs.setdefault(m[1], []).append(float(m[4])); checks.setdefault(m[1], set()).add(m[5])
    for m in re.finditer(r'MACRO (\S+) ms=(\d+) check=(\w+)', out):   # macro workloads: value is ms
        runs.setdefault(m[1], []).append(float(m[2])); checks.setdefault(m[1], set()).add(m[3])
res = {k: {'median_ns': statistics.median(v), 'min_ns': min(v), 'max_ns': max(v), 'n': len(v), 'checks': sorted(checks[k])} for k, v in runs.items()}
json.dump(res, open(os.path.join(os.environ.get('LR_BENCH_DIR', '.'), f'results-{label}.json'), 'w'), indent=1)
for k, v in res.items():
    print(f"{label:12} {k:9} median={v['median_ns']:10.2f} ns  min={v['min_ns']:10.2f} max={v['max_ns']:10.2f}  checks={len(v['checks'])}")
