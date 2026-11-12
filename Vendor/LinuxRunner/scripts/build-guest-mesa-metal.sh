#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${GPU_ROOTFS:?Disposable Debian fixture required}"
: "${GPU_EXCHANGE:?Absolute exchange required}"
cp graphics/mesa_metal_dispatch.c graphics/guest_gpu.h graphics/shader_compiler.h tests/linuxrunner/gpu/mesa_metal_contract.c tests/linuxrunner/gpu/interop_transfer_contract.c tests/linuxrunner/gpu/stencil_contract.c "$GPU_EXCHANGE/"
export ISH_BIND_MOUNTS="/home/mobile/Downloads=$GPU_EXCHANGE"
build/host/ish -f "$GPU_ROOTFS" /bin/sh -ec '
 cd /home/mobile/Downloads
 gcc -O2 -fPIC -shared mesa_metal_dispatch.c -lGL -lX11 -ldl -lm -lz -pthread -o liblinuxrunner-mesa-metal.so
 gcc -O2 stencil_contract.c -o gpu-stencil-contract
 gcc -O2 interop_transfer_contract.c -o gpu-interop-transfer-contract
 gcc -O2 mesa_metal_contract.c -lGL -lX11 -ldl -o gpu-mesa-metal-contract
'
if [[ ${GPU_PACKAGE_ASSETS:-0} == 1 ]]; then
 cp "$GPU_EXCHANGE/"{liblinuxrunner-mesa-metal.so,gpu-mesa-metal-contract,gpu-interop-transfer-contract,gpu-stencil-contract} app/LinuxRunner/GUISupport.bundle/
 python3 scripts/generate-gui-support-manifest.py
fi
