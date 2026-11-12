#!/usr/bin/env python3
# interleave.py ROUNDS LABEL=RUNTIME ... — alternate runtimes each round (micro + macro), medians per label.
import os, subprocess, sys, re, json, statistics
rounds = int(sys.argv[1]); specs = [a.split('=', 1) for a in sys.argv[2:]]
B = os.environ.get('LR_BENCH_DIR', '.')
data = {lab: {} for lab, _ in specs}; checks = {lab: {} for lab, _ in specs}
for r in range(rounds):
    for lab, rt in specs:
        here = os.path.dirname(os.path.abspath(__file__))
        for cmd in ([f'{here}/run.sh', rt, '1'], [f'{here}/runmacro.sh', rt]):
            out = subprocess.run(cmd, capture_output=True, text=True).stdout
            for m in re.finditer(r'BENCH (\S+)\s+iters=\d+\s+ms=\s*[\d.]+ ns_per_iter=\s*([\d.]+) check=(\w+)', out):
                data[lab].setdefault(m[1], []).append(float(m[2])); checks[lab].setdefault(m[1], set()).add(m[3])
            for m in re.finditer(r'MACRO (\S+) ms=(\d+) check=(\w+)', out):
                data[lab].setdefault(m[1], []).append(float(m[2])); checks[lab].setdefault(m[1], set()).add(m[3])
for lab, _ in specs:
    res = {k: {'median_ns': statistics.median(v), 'min_ns': min(v), 'max_ns': max(v), 'n': len(v), 'checks': sorted(checks[lab][k])} for k, v in data[lab].items()}
    json.dump(res, open(f'{B}/results-{lab}.json', 'w'), indent=1)
print('done', [lab for lab, _ in specs])
