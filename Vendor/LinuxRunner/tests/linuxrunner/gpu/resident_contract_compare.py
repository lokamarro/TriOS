#!/usr/bin/env python3
"""Compare resident_contract dumps (RESIDENT_CONTRACT_DUMP) from a Mesa-only run
and a resident-Metal run.

usage: resident_contract_compare.py MESA_PREFIX RESIDENT_PREFIX [--frames 3]

Exact hashes cannot match across rasterizers: Metal converts float colour to
unorm8 with round-to-nearest (0.3 -> 77) where llvmpipe yields 76, and Metal
depth is Depth32Float rather than 24-bit fixed point. Tolerances: 1/255 per
RGBA8 channel, 2/255 for the RGBA16F pass that samples it at 2x, and 4 units of
2^-24 for depth. Anything larger is a pipeline error (missing draw, wrong
target, stale contents) and fails.
"""
import argparse, struct, sys

W = H = 128
p = argparse.ArgumentParser()
p.add_argument('mesa'); p.add_argument('resident'); p.add_argument('--frames', type=int, default=3)
a = p.parse_args()
LIMIT = {'fb_a.rgba': 1, 'window.rgba': 1, 'fb_b.half': 2 / 255 + 1e-3, 'depth.f32': 4}

def values(name, data):
    if name.endswith('rgba'): return list(data)
    if name.endswith('half'): return struct.unpack('<%de' % (len(data) // 2), data)
    return struct.unpack('<%df' % (len(data) // 4), data)

failed = False
for frame in range(a.frames):
    for name, limit in LIMIT.items():
        m = open('%s-%d-%s' % (a.mesa, frame, name), 'rb').read()
        r = open('%s-%d-%s' % (a.resident, frame, name), 'rb').read()
        if len(m) != len(r) or len(m) != W * H * (8 if name.endswith('half') else 4):
            print('RESIDENT_COMPARE frame=%d %s size mismatch' % (frame, name)); failed = True; continue
        mv, rv = values(name, m), values(name, r)
        worst = max(abs(x - y) for x, y in zip(mv, rv))
        differing = sum(1 for x, y in zip(mv, rv) if x != y)
        ok = worst <= limit
        failed |= not ok
        print('RESIDENT_COMPARE frame=%d %s differing=%d max=%g limit=%g %s' % (frame, name, differing, worst, limit, 'PASS' if ok else 'FAIL'))
print('RESIDENT_COMPARE_%s' % ('FAIL' if failed else 'PASS'))
sys.exit(1 if failed else 0)
