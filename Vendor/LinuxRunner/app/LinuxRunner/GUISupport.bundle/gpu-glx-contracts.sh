#!/bin/sh
# A scoped client library: system Mesa, rootfs and application data are preserved.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$base"
export LD_LIBRARY_PATH="$base/metal-glx:$base"
printf '%s\n' 'GPU_GLX_CHECK: real guest draw commands through Metal; not final-framebuffer presentation'
for test in gpu-boolean-block-contract gpu-storage-buffer-contract gpu-uniform-array-contract gpu-typed-shader-contract gpu-uniform-block-contract gpu-nested-uniform-contract gpu-indexed-contract gpu-divisor-contract gpu-instanced-contract gpu-blend-contract gpu-depth-framebuffer-contract gpu-glx-metal-contract; do
  chmod 700 "$base/$test"
  "$base/$test"
done
printf '%s\n' 'GPU_GLX_CHECK_PASS: supported subset only; OpenGL 4.3 / Blender Metal remains unsupported'
