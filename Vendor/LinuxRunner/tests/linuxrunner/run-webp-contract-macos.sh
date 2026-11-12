#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${CPU_ROOTFS:?Use a disposable Debian fixture with libwebp.so.7}"
: "${CPU_EXCHANGE:?Use an isolated host exchange}"
out=${CPU_OUTPUT:-build/webp-contract}
mkdir -p "$out" "$CPU_EXCHANGE"
cp tests/linuxrunner/webp_decode_contract.c "$CPU_EXCHANGE/"
cp tests/linuxrunner/fixtures/webp/* "$CPU_EXCHANGE/"
export ISH_BIND_MOUNTS="/home/mobile/Downloads=$CPU_EXCHANGE"
export ISH_ARM64_MEMORY_PC_FUSION=0
python3 - "$CPU_ROOTFS" "$out" <<'PY'
import pathlib,subprocess,sys
root,out=sys.argv[1:];out=pathlib.Path(out)
with (out/'guest.log').open('w') as log:
 subprocess.run(['build/host/ish','-f',root,'/bin/sh','-ec',
  'cd /home/mobile/Downloads; gcc -O2 -Wall -Wextra webp_decode_contract.c -Wl,-l:libwebp.so.7 -o webp-decode; '
  './webp-decode encoded-64x48.webp expected-64x48.rgba; '
  './webp-decode --encode source-64x48.rgb 64 48 actual-64x48.webp; '
  './webp-decode actual-64x48.webp expected-64x48.rgba'],
  stdout=log,stderr=subprocess.STDOUT,timeout=90,check=True)
lines=[s for s in (out/'guest.log').read_text().splitlines() if s.startswith('WEBP_')]
assert len(lines)==3 and all('PASS' in s for s in lines)
print('\n'.join(lines))
PY
