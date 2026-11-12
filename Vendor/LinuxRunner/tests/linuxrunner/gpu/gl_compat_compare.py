#!/usr/bin/env python3
"""Compare two PPM images from gl_compat_test (Mesa reference vs bridge).
  gl_compat_compare.py REFERENCE.ppm CANDIDATE.ppm [max-channel-diff] [max-bad-percent]
A pixel is bad when any channel differs by more than max-channel-diff (default 12,
covering filtering and rasterization-rule differences). PASS when the bad-pixel
share is at most max-bad-percent (default 2.0) and the images are not blank."""
import sys

def load(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

ref_w, ref_h, ref = load(sys.argv[1])
w, h, cand = load(sys.argv[2])
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 12
allowed = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0
assert (w, h) == (ref_w, ref_h), "size differs"
bad = sum(1 for i in range(0, len(ref), 3) if max(abs(ref[i + k] - cand[i + k]) for k in range(3)) > limit)
distinct = len({ref[i:i + 3] for i in range(0, len(ref), 3 * 7)})
share = 100.0 * bad / (w * h)
ok = share <= allowed and distinct > 3
print(f"GL_COMPAT_COMPARE {'PASS' if ok else 'FAIL'} bad_pixels={bad} ({share:.2f}%) reference_colours={distinct} tolerance={limit}")
sys.exit(0 if ok else 1)
