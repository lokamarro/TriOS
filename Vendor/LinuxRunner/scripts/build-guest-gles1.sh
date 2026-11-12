#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${GPU_ROOTFS:?Disposable Debian ARM64 fixture required}"
: "${GPU_EXCHANGE:?Absolute exchange path required}"
cp graphics/{guest_gl_subset.c,guest_gl_subset.h,guest_gles1.inc,guest_gles2.inc,guest_typed.inc,guest_typed.h,guest_textures.inc,guest_framebuffers.inc,guest_glx.inc,guest_blend.inc,guest_depth.inc,guest_mapping.inc,guest_mapping.h,guest_storage.inc,guest_uniforms.inc,guest_uniforms.h,shader_compiler.h,guest_gpu.h} "$GPU_EXCHANGE/"
cp tests/linuxrunner/gpu/{boolean_block_contract.c,blender_shader_contract.c,storage_buffer_contract.c,uniform_array_contract.c,glx_context.c,guest_triangle.c,gles1_contract.c,texture_contract.c,framebuffer_contract.c,glx_metal_contract.c,uniform_block_contract.c,typed_shader_contract.c,blend_contract.c,instanced_contract.c,indexed_contract.c,divisor_contract.c,depth_framebuffer_contract.c,nested_uniform_contract.c,uniform_block_benchmark.c,framebuffer_copy_benchmark.c,resize_gears.c} "$GPU_EXCHANGE/"
cp tests/linuxrunner/gpu/mesa-demos/{gears.c,es2gears.c,glxheads.c,eglut.c,eglut.h,eglutint.h,eglut_x11.c} "$GPU_EXCHANGE/"
export ISH_BIND_MOUNTS="/home/mobile/Downloads=$GPU_EXCHANGE"
build/host/ish -f "$GPU_ROOTFS" /bin/sh -ec '
 cd /home/mobile/Downloads
 test -f /usr/include/GLES/gl.h && test -f /usr/include/EGL/egl.h || { echo "Install libgles-dev libegl-dev in this disposable fixture"; exit 2; }
 gcc -O2 -fPIC -shared guest_gl_subset.c -lm -pthread -lX11 -o liblinuxrunner-gles-subset.so
 gcc -O2 -fPIC -shared -DLR_GLX_PROVIDER guest_gl_subset.c -lm -pthread -lX11 -o liblinuxrunner-glx-metal.so
 gcc -O2 glx_metal_contract.c -L. -llinuxrunner-glx-metal -lX11 -pthread -o gpu-glx-metal-contract
 gcc -O2 boolean_block_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-boolean-block-contract
 gcc -O2 blender_shader_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-blender-shader-contract
 gcc -O2 storage_buffer_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-storage-buffer-contract
 gcc -O2 uniform_array_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-uniform-array-contract
 gcc -O2 typed_shader_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-typed-shader-contract
 gcc -O2 uniform_block_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-uniform-block-contract
 gcc -O2 blend_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-blend-contract
 gcc -O2 instanced_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-instanced-contract
 gcc -O2 divisor_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-divisor-contract
 gcc -O2 indexed_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-indexed-contract
 gcc -O2 depth_framebuffer_contract.c -L. -llinuxrunner-glx-metal -lX11 -o gpu-depth-framebuffer-contract
 gcc -O2 nested_uniform_contract.c -lGL -lX11 -o gpu-nested-uniform-contract
 gcc -O2 framebuffer_copy_benchmark.c -L. -llinuxrunner-gles-subset -lX11 -o gpu-framebuffer-copy-benchmark
 gcc -O2 glx_context.c -lGL -lX11 -ldl -o gpu-mesa-contract
 gcc -O2 glxheads.c -lGL -lX11 -o linuxrunner-mesa-glxheads
 gcc -O2 guest_triangle.c -L. -llinuxrunner-gles-subset -lX11 -o linuxrunner-gpu-demo
 gcc -O2 es2gears.c eglut.c eglut_x11.c -lEGL -lGLESv2 -lX11 -lm -o linuxrunner-mesa-es2-gears
 gcc -O2 gears.c eglut.c eglut_x11.c -lEGL -lGLESv1_CM -lX11 -lm -o linuxrunner-mesa-es1-gears
 gcc -O2 framebuffer_contract.c -L. -llinuxrunner-gles-subset -lX11 -o gpu-framebuffer-contract
 gcc -O2 texture_contract.c -L. -llinuxrunner-gles-subset -lX11 -o gpu-texture-contract
 gcc -O2 gles1_contract.c -L. -llinuxrunner-gles-subset -lX11 -o gpu-gles1-contract
 gcc -O2 resize_gears.c -lX11 -o gpu-resize-gears
'
if [[ ${GPU_PACKAGE_ASSETS:-0} == 1 ]]; then
 cp "$GPU_EXCHANGE/"{liblinuxrunner-gles-subset.so,liblinuxrunner-glx-metal.so,gpu-mesa-contract,gpu-boolean-block-contract,gpu-blender-shader-contract,gpu-storage-buffer-contract,gpu-uniform-array-contract,gpu-glx-metal-contract,gpu-typed-shader-contract,gpu-uniform-block-contract,gpu-blend-contract,gpu-instanced-contract,gpu-indexed-contract,gpu-divisor-contract,gpu-depth-framebuffer-contract,gpu-nested-uniform-contract,linuxrunner-mesa-glxheads,linuxrunner-gpu-demo,linuxrunner-mesa-es1-gears,linuxrunner-mesa-es2-gears,gpu-texture-contract,gpu-framebuffer-contract} app/LinuxRunner/GUISupport.bundle/
 python3 scripts/generate-gui-support-manifest.py
fi
