#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
: "${CPU_ROOTFS:?Use a disposable Debian fixture}"
: "${CPU_EXCHANGE:?Use an isolated exchange}"
out=${CPU_OUTPUT:-build/scalar-shift-narrow}
mkdir -p "$out" "$CPU_EXCHANGE"
test "$(uname -m)" = arm64
clang -O2 -Wall -Wextra -Werror tests/arm64/simd-saturating/shift-narrow.c -o "$out/native"
"$out/native" > "$out/native.log"
cp tests/arm64/simd-saturating/shift-narrow{,-invalid}.c "$CPU_EXCHANGE/"
export ISH_BIND_MOUNTS="/home/mobile/Downloads=$CPU_EXCHANGE"
export ISH_ARM64_MEMORY_PC_FUSION=0
python3 - "$CPU_ROOTFS" "$out" <<'PY'
import pathlib,subprocess,sys
root,out=sys.argv[1:];out=pathlib.Path(out)
with (out/'build.log').open('w') as log:
 subprocess.run(['build/host/ish','-f',root,'/bin/sh','-ec',
  'cd /home/mobile/Downloads; gcc -O2 -Wall -Wextra shift-narrow.c -o shift-narrow; gcc -O2 -Wall -Wextra shift-narrow-invalid.c -o shift-narrow-invalid'],stdout=log,stderr=subprocess.STDOUT,timeout=90,check=True)
for name in ['shift-narrow','shift-narrow-invalid']:
 with (out/(name+'.log')).open('w') as log:
  subprocess.run(['build/host/ish','-f',root,'/home/mobile/Downloads/'+name],stdout=log,stderr=subprocess.STDOUT,timeout=30,check=True)
actual=[s for s in (out/'shift-narrow.log').read_text().splitlines() if s.startswith('SCALAR_SHIFT_NARROW_PASS')]
assert actual==(out/'native.log').read_text().splitlines(),'Native/guest result differs'
assert 'SCALAR_SHIFT_RESERVED_PASS' in (out/'shift-narrow-invalid.log').read_text()
print(actual[0]);print('SCALAR_SHIFT_RESERVED_PASS precise_SIGILL=PASS')
PY
