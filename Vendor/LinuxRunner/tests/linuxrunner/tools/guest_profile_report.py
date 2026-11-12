#!/usr/bin/env python3
"""Summarise an ISH_GUEST_PROFILE file: guest CPU samples by process, library
and nearest exported symbol.

usage: guest_profile_report.py PROFILE ROOTFS_DATA_DIR [--state 1] [--top 30] [--pid N]
State 1 = executing guest code (CPU); state 2 = in a system call or interrupt
(includes blocking waits, so it is not CPU time).
"""
import argparse, bisect, collections, os, re, subprocess

NM = '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-nm'
CXXFILT = '/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-cxxfilt'

p = argparse.ArgumentParser()
p.add_argument('profile'); p.add_argument('rootfs')
p.add_argument('--state', type=int, default=1); p.add_argument('--top', type=int, default=30)
p.add_argument('--pid', type=int)
a = p.parse_args()

samples, maps, header = [], collections.defaultdict(list), ''
with open(a.profile, errors='replace') as f:
    lines = f.read().split('\n')
i = 0
while i < len(lines):
    line = lines[i]
    if line.startswith('PROFILE'): header = line
    elif line.startswith('SAMPLE'):
        _, tgid, state, pc, count = line.split()
        samples.append((int(tgid), int(state), int(pc, 16), int(count)))
    elif line.startswith('MAPS'):
        _, tgid, size = line.split()
        size = int(size); text = []; consumed = 0; i += 1
        while i < len(lines) and consumed < size:
            text.append(lines[i]); consumed += len(lines[i]) + 1; i += 1
        for m in text:
            parts = m.split()
            if len(parts) >= 6 and 'x' in parts[1]:
                lo, hi = (int(x, 16) for x in parts[0].split('-'))
                maps[int(tgid)].append((lo, hi, int(parts[2], 16), parts[5]))
        continue
    i += 1

symbols_cache = {}
def symbols(path):
    if path not in symbols_cache:
        host = os.path.join(a.rootfs, path.lstrip('/'))
        syms = []
        try:
            out = subprocess.run([NM, '-D', '--defined-only', host], capture_output=True, text=True).stdout
            out += subprocess.run([NM, '--defined-only', host], capture_output=True, text=True).stdout
            for l in out.splitlines():
                parts = l.split()
                if len(parts) == 3 and parts[1] in 'tTwW': syms.append((int(parts[0], 16), parts[2]))
        except OSError:
            pass
        syms.sort(); symbols_cache[path] = ([s[0] for s in syms], [s[1] for s in syms])
    return symbols_cache[path]

def demangle(name):
    try: return subprocess.run([CXXFILT, name], capture_output=True, text=True).stdout.strip()[:110]
    except OSError: return name

def resolve(tgid, pc):
    for lo, hi, off, path in maps.get(tgid, []):
        if lo <= pc < hi:
            offset = pc - lo + off
            addrs, names = symbols(path)
            k = bisect.bisect_right(addrs, offset) - 1
            return os.path.basename(path), (names[k] if k >= 0 else '?')
    return '[unmapped]', '?'

total = sum(c for t, s, pc, c in samples if s == a.state and (a.pid is None or t == a.pid))
print(header, '| state', a.state, 'samples', total)
by_pid, by_lib, by_sym = collections.Counter(), collections.Counter(), collections.Counter()
for tgid, state, pc, count in samples:
    if state != a.state or (a.pid is not None and tgid != a.pid): continue
    lib, sym = resolve(tgid, pc)
    by_pid[tgid] += count; by_lib[lib] += count; by_sym[(lib, sym)] += count
print('\nBy process:')
for k, v in by_pid.most_common(10): print('%6d %5.1f%%  pid %d' % (v, 100 * v / total, k))
print('\nBy library:')
for k, v in by_lib.most_common(a.top): print('%6d %5.1f%%  %s' % (v, 100 * v / total, k))
print('\nBy symbol:')
for (lib, sym), v in by_sym.most_common(a.top): print('%6d %5.1f%%  %s  %s' % (v, 100 * v / total, lib, demangle(sym)))
