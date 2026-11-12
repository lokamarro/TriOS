#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${CPU_ROOTFS:?Use a disposable ARM64 Linux fixture}"
: "${CPU_EXCHANGE:?Use an isolated host exchange}"
out=${CPU_OUTPUT:-build/sysv-shm-test}
mkdir -p "$out" "$CPU_EXCHANGE"
cp tests/linuxrunner/sysv_shm_lifetime.c "$CPU_EXCHANGE/"
export ISH_BIND_MOUNTS="/home/mobile/Downloads=$CPU_EXCHANGE"
export ISH_ARM64_MEMORY_PC_FUSION=0
python3 - "$CPU_ROOTFS" "$out" <<'PY'
from pathlib import Path
import subprocess,sys
root,out=sys.argv[1:]
with (Path(out)/'guest.log').open('w') as log:
 subprocess.run(['build/host/ish','-f',root,'/bin/sh','-ec',
  'cd /home/mobile/Downloads; gcc -O2 -pthread sysv_shm_lifetime.c -o sysv-shm-lifetime; ./sysv-shm-lifetime'],
  stdout=log,stderr=subprocess.STDOUT,timeout=60,check=True)
for line in (Path(out)/'guest.log').read_text().splitlines():
 if 'SYSV_' in line:print(line)
PY
