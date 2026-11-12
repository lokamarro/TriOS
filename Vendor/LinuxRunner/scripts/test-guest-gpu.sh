#!/bin/bash
# macOS host integration test; never claims physical-iPhone validation.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ $(uname -s) != Darwin ]]; then echo 'Metal test requires a macOS Metal device'; exit 77; fi
: "${GPU_ROOTFS:?Set GPU_ROOTFS to a disposable case-sensitive Debian fixture}"
: "${GPU_EXCHANGE:?Set GPU_EXCHANGE to its existing exchange directory}"
out=${GPU_OUTPUT:-build/guest-gpu-test}
mkdir -p "$out"
ninja -C build/host > "$out/kernel-build.log" 2>&1
if [[ ! -f build/shader-compiler/macosx/liblrshader.a ]]; then scripts/build-shader-compiler.sh macosx > "$out/shader-build.log" 2>&1; fi
flags=(-O2 -g -I. -Ibuild/host -DGUEST_ARM64=1 -DENGINE_ASBESTOS=1 -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -DLOG_HANDLER_DPRINTF=1)
clang "${flags[@]}" -Dmain=lr_cli_main -c main.c -o "$out/main.o"
clang++ -std=c++17 -O2 -Ideps/glslang -Ideps/spirv-cross -c graphics/shader_compiler.cpp -o "$out/shader.o"
clang -O2 -g -fobjc-arc -I. tests/linuxrunner/gpu/host_driver.m "$out/main.o" "$out/shader.o" build/shader-compiler/macosx/liblrshader.a -lc++ build/host/libish.a build/host/libish_emu.a build/host/libfakefs.a -lm -ldl -lsqlite3 -framework Foundation -framework Metal -o "$out/ish-metal"
cp graphics/guest_gl_subset.c graphics/guest_gl_subset.h graphics/guest_gpu.h graphics/guest_gles1.inc graphics/guest_gles2.inc graphics/guest_typed.inc graphics/guest_typed.h graphics/guest_textures.inc graphics/guest_framebuffers.inc graphics/guest_blend.inc graphics/guest_depth.inc graphics/guest_mapping.inc graphics/guest_mapping.h graphics/guest_storage.inc graphics/guest_uniforms.inc graphics/guest_uniforms.h graphics/shader_compiler.h tests/linuxrunner/gpu/guest_triangle.c tests/linuxrunner/gpu/extended_abi_contract.c tests/linuxrunner/gpu/stencil_contract.c tests/linuxrunner/gpu/guest_device.c tests/linuxrunner/gpu/surface_quota_contract.c tests/linuxrunner/gpu/guest_concurrency.c tests/linuxrunner/gpu/gles1_contract.c tests/linuxrunner/gpu/gles2_contract.c tests/linuxrunner/gpu/texture_contract.c tests/linuxrunner/gpu/vertex_arena_contract.c tests/linuxrunner/gpu/framebuffer_contract.c "$GPU_EXCHANGE/"
export ISH_BIND_MOUNTS="/home/mobile/Downloads=$GPU_EXCHANGE"
build/host/ish -f "$GPU_ROOTFS" /bin/sh -ec '
 cd /home/mobile/Downloads
 gcc -O2 -fPIC -shared guest_gl_subset.c -lm -pthread -lX11 -o liblinuxrunner-gles-subset.so
 gcc -O2 guest_triangle.c -L. -llinuxrunner-gles-subset -lX11 -o linuxrunner-gpu-demo
 gcc -O2 gles2_contract.c -L. -llinuxrunner-gles-subset -lX11 -o gpu-gles2-contract
 gcc -O2 vertex_arena_contract.c -o gpu-vertex-arena
 gcc -O2 framebuffer_contract.c -L. -llinuxrunner-gles-subset -lX11 -o gpu-framebuffer-contract
 gcc -O2 texture_contract.c -L. -llinuxrunner-gles-subset -lX11 -o gpu-texture-contract
 gcc -O2 gles1_contract.c -L. -llinuxrunner-gles-subset -lX11 -o gpu-gles1-contract
 gcc -O2 surface_quota_contract.c -o gpu-surface-quota-contract
 gcc -O2 extended_abi_contract.c -o gpu-extended-abi-contract
 gcc -O2 stencil_contract.c -o gpu-stencil-contract
 gcc -O2 guest_device.c -o gpu-device-test
 gcc -O2 -pthread guest_concurrency.c -o gpu-concurrency-test
 [ -e /dev/linuxrunner-gpu ] || { mknod /dev/linuxrunner-gpu c 241 0; chmod 600 /dev/linuxrunner-gpu; }
' > "$out/guest-build.log" 2>&1
"$out/ish-metal" -f "$GPU_ROOTFS" /bin/sh -ec '
 cd /home/mobile/Downloads
 LD_LIBRARY_PATH=$PWD ./gpu-gles1-contract
 LD_LIBRARY_PATH=$PWD ./gpu-gles2-contract
 LD_LIBRARY_PATH=$PWD ./gpu-texture-contract
 LD_LIBRARY_PATH=$PWD ./gpu-framebuffer-contract
 ./gpu-vertex-arena
 LD_LIBRARY_PATH=$PWD ./linuxrunner-gpu-demo --headless
 ./gpu-surface-quota-contract
 ./gpu-extended-abi-contract
 ./gpu-stencil-contract
 ./gpu-device-test
 ./gpu-concurrency-test
' > "$out/guest-tests.log" 2>&1
# Reporting must not turn a successful guest suite into failure when rg is absent.
if command -v rg >/dev/null 2>&1; then
    rg 'STENCIL_CONTRACT_PASS|EXTENDED_ABI_CONTRACT_PASS|FRAMEBUFFER_CONTRACT_PASS|VERTEX_ARENA_PASS|TEXTURE_CONTRACT_PASS|GLES[12]_CONTRACT_PASS|GPU_.*PASS|GPU_BRIDGE_COUNTERS' "$out/guest-tests.log"
else
    grep -E 'STENCIL_CONTRACT_PASS|EXTENDED_ABI_CONTRACT_PASS|FRAMEBUFFER_CONTRACT_PASS|VERTEX_ARENA_PASS|TEXTURE_CONTRACT_PASS|GLES[12]_CONTRACT_PASS|GPU_.*PASS|GPU_BRIDGE_COUNTERS' "$out/guest-tests.log"
fi
