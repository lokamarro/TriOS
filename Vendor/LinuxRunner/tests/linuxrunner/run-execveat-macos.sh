#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
: "${CPU_ROOTFS:?Use a disposable ARM64 Linux fixture}"
: "${CPU_EXCHANGE:?Use an isolated host exchange}"
out=${CPU_OUTPUT:-build/execveat-test}
mkdir -p "$out" "$CPU_EXCHANGE"
cp tests/linuxrunner/execveat_contract.c "$CPU_EXCHANGE/"
export ISH_BIND_MOUNTS="/home/mobile/Downloads=$CPU_EXCHANGE"
export ISH_ARM64_MEMORY_PC_FUSION=0
python3 - "$CPU_ROOTFS" "$out" "${CPU_RUNTIME:-build/host/ish}" <<'PY'
from pathlib import Path
import subprocess,sys
root,out,runtime=sys.argv[1:]
with (Path(out)/'guest.log').open('w') as log:
 subprocess.run([runtime,'-f',root,'/bin/sh','-ec',
  'cd /home/mobile/Downloads; gcc -O2 -Wall -Wextra execveat_contract.c -o execveat-contract; ./execveat-contract'],
  stdout=log,stderr=subprocess.STDOUT,timeout=90,check=True)
for line in (Path(out)/'guest.log').read_text().splitlines():
 if 'EXECVEAT_' in line:print(line)
PY
