#!/bin/bash
# Real ARM64 Linux GLX draw dispatch to Metal; Mac fixture only.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${GPU_ROOTFS:?Disposable Debian ARM64 rootfs required}"
: "${GPU_EXCHANGE:?Absolute exchange path required}"
: "${GPU_DRIVER:?Metal-enabled host driver built by test-guest-gpu.sh required}"
out=${GPU_OUTPUT:-build/glx-metal-check}
[[ ! -e $out ]] || { echo 'Use a fresh output directory'; exit 2; }
mkdir -p "$out"
GPU_PACKAGE_ASSETS=0 scripts/build-guest-gles1.sh > "$out/guest-build.log" 2>&1
clang -O2 -fobjc-arc -I. tests/linuxrunner/gpu/glx_metal_probe.m app/LinuxRunner/RFBClient.m app/LinuxRunner/RFBProtocol.c -framework Foundation -framework ImageIO -framework CoreGraphics -o "$out/probe"
cat > "$out/command.sh" <<'GUEST'
#!/bin/sh
cd /home/mobile/Downloads
mkdir -p metal-glx
ln -sf ../liblinuxrunner-glx-metal.so metal-glx/libGL.so.1
export LD_LIBRARY_PATH=$PWD/metal-glx:$PWD
./gpu-boolean-block-contract || exit $?
./gpu-storage-buffer-contract || exit $?
./gpu-uniform-array-contract || exit $?
./gpu-typed-shader-contract || exit $?
./gpu-nested-uniform-contract || exit $?
./gpu-depth-framebuffer-contract || exit $?
./gpu-instanced-contract || exit $?
./gpu-indexed-contract || exit $?
./gpu-divisor-contract || exit $?
./gpu-uniform-block-contract || exit $?
./gpu-blend-contract || exit $?
./gpu-glx-metal-contract || exit $?
exec ./linuxrunner-mesa-glxheads
GUEST
LR_FIXTURE_GLX=software python3 tests/linuxrunner/gui/run_guest_session.py --rootfs "$GPU_ROOTFS" --exchange "$GPU_EXCHANGE" --runtime "$GPU_DRIVER" --probe "$out/probe" --mode custom --script "$out/command.sh" --geometry 960x540 --output "$out/session" --timeout 55 > "$out/session.log" 2>&1
# Reporting must not turn a successful guest suite into failure when rg is absent.
if command -v rg >/dev/null 2>&1; then
    rg 'UNIFORM_BLOCK_CONTRACT_PASS|BLEND_CONTRACT_PASS|GLX_METAL_CONTRACT_PASS|METAL_COUNTERS' "$out/session/application.log"
else
    grep -E 'UNIFORM_BLOCK_CONTRACT_PASS|BLEND_CONTRACT_PASS|GLX_METAL_CONTRACT_PASS|METAL_COUNTERS' "$out/session/application.log"
fi
cat "$out/session/glx-metal-result.json"
