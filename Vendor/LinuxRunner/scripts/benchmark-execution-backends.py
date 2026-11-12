#!/usr/bin/env python3
"""Optional macOS JIT differential benchmark; never proof of iOS acceleration.

Use one jit-enabled host driver for both modes, plus a native reference for the
portable compute program. The rootfs and guest executable must already exist.
No capability is inferred from attachment or successful process launch.
"""
import argparse, json, os, pathlib, platform, re, statistics, subprocess, time

p = argparse.ArgumentParser()
p.add_argument('--runtime', required=True)
p.add_argument('--rootfs', required=True)
p.add_argument('--compute', required=True, help='guest path to compiled benchmark_compute.c')
p.add_argument('--native-reference', required=True)
p.add_argument('--runs', type=int, default=5)
p.add_argument('--output', required=True)
a = p.parse_args()
if not 1 <= a.runs <= 20:
    p.error('runs must be between 1 and 20')
reference = subprocess.check_output([a.native_reference], timeout=30).decode().strip()
workloads = {
    'command': (['/bin/true'], ''),
    'shell-compute': (['/bin/sh', '-ec', 'i=0; while [ "$i" -lt 500 ]; do i=$((i+1)); done; echo "$i"'], '500'),
    'C-compute': ([a.compute], reference),
    'python-compute': (['/usr/bin/python3', '-c', 'print(sum(i*i for i in range(10000)))'], '333283335000'),
}
report = {'platform': platform.platform(), 'physical_iOS': 'NOT TESTED',
          'timing': 'host wall time including runtime creation, no CPU/RSS/GUI claim',
          'runtime': str(pathlib.Path(a.runtime).resolve()), 'runs': a.runs,
          'compute_native_reference': reference, 'workloads': {}}
for name, (command, expected) in workloads.items():
    modes = {}
    for mode in ('interpreter', 'macOS-MAP_JIT'):
        samples, emitted, statuses = [], [], []
        for _ in range(a.runs):
            env = dict(os.environ, ISH_JIT='1' if mode == 'macOS-MAP_JIT' else '0', ISH_JIT_STATS='1')
            start = time.perf_counter()
            try:
                run = subprocess.run([a.runtime, '-f', a.rootfs, *command], env=env,
                                     capture_output=True, text=True, timeout=90)
            except subprocess.TimeoutExpired:
                statuses.append('TIMEOUT'); break
            samples.append(time.perf_counter() - start)
            statuses.append('PASS' if run.returncode == 0 and run.stdout.strip() == expected else 'FAIL')
            emitted.append(bool(re.search(r'JIT\(MAP_JIT\): blocks [1-9][0-9]*,', run.stderr)))
            if statuses[-1] != 'PASS':
                modes[mode] = {'stdout': run.stdout, 'stderr': run.stderr[-4000:]}; break
        modes.setdefault(mode, {}).update(samples=samples, results=statuses,
            median_seconds=statistics.median(samples) if samples else None,
            generated_code_observed=all(emitted) if emitted else False)
    report['workloads'][name] = modes
out = pathlib.Path(a.output); out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(report, indent=2))
print(out)
raise SystemExit(0 if all(len(v['results']) == a.runs and all(s == 'PASS' for s in v['results'])
                         for m in report['workloads'].values() for v in m.values()) else 1)
