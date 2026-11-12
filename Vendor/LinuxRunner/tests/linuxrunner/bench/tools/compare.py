#!/usr/bin/env python3
# compare.py BASE NEW [NATIVE] — speedup table from results-*.json
import os, json, sys
d = lambda n: json.load(open(os.path.join(os.environ.get('LR_BENCH_DIR', '.'), f'results-{n}.json')))
a, b = d(sys.argv[1]), d(sys.argv[2]); nat = d(sys.argv[3]) if len(sys.argv) > 3 else None
print(f"{'workload':10} {sys.argv[1]:>12} {sys.argv[2]:>12} {'speedup':>8}" + (f" {'vs native':>10}" if nat else ""))
for k in a:
    if k not in b: continue
    x, y = a[k]['median_ns'], b[k]['median_ns']
    flag = '' if a[k]['checks'] == b[k]['checks'] else '  CHECK-MISMATCH'
    print(f"{k:10} {x:12.2f} {y:12.2f} {x/y:8.3f}" + (f" {y/nat[k]['median_ns']:9.1f}x" if nat and k in nat else "") + flag)
