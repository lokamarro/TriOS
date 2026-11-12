#!/usr/bin/env python3
# bottleneck.py TRACE — top-down bottleneck shares (cycle-weighted) from an Instruments "CPU Counters" trace.
import os, subprocess, sys, re, collections, os
env = dict(os.environ, DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
xml = subprocess.run(['xcrun', 'xctrace', 'export', '--input', sys.argv[1], '--xpath',
    '/trace-toc/run[@number="1"]/data/table[@schema="MetricAggregationForProcess"]'], capture_output=True, text=True, env=env).stdout
ids = {}
def val(tag_xml):
    m = re.search(r'id="(\d+)"[^>]*>([^<]*)<', tag_xml)
    if m: ids[m[1]] = m[2]; return m[2]
    m = re.search(r'ref="(\d+)"', tag_xml)
    return ids.get(m[1]) if m else None
groups = collections.defaultdict(dict)
for row in re.findall(r'<row>(.*?)</row>', xml):
    cells = re.findall(r'<(\w[\w-]*)( [^>]*)?(?:/>|>([^<]*)</\1>)', row)
    parts = re.findall(r'<[^>]+?(?:/>|>[^<]*</[^>]+>)', row)
    v = [val(p) for p in parts]
    # start, duration, process, uint64, fixed-decimal, string(kind), boolean(precise sample?)
    start, dur, kind, boolean = v[0], v[1], v[5], v[6]
    if boolean != '1': continue          # per-millisecond samples only
    if kind == 'cycle': groups[start]['cycle'] = int(v[3])
    else: groups[start][kind] = float(v[4])
tot = collections.Counter(); cyc = 0
for g in groups.values():
    c = g.get('cycle', 0); kinds = [k for k in g if k != 'cycle']; s = sum(g[k] for k in kinds)
    if not c or not s: continue
    cyc += c
    for k in kinds: tot[k] += c * g[k] / s
print(f"cycles={cyc}  " + "  ".join(f"{k}={tot[k]/cyc*100:.1f}%" for k in sorted(tot)))
