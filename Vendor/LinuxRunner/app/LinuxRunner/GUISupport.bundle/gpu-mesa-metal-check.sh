#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
unset LD_LIBRARY_PATH LD_PRELOAD MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe LR_MESA_METAL_MODE=verify LR_MESA_METAL_MAX_DRAWS=64
chmod 700 "$base/gpu-mesa-metal-contract" "$base/gpu-interop-transfer-contract" "$base/gpu-stencil-contract"
"$base/gpu-stencil-contract"
"$base/gpu-interop-transfer-contract"
echo 'MESA_METAL_CHECK: genuine Mesa GLX core context + actual Metal draw replacement. Mesa renderer strings retained; software fallback is independently counted.'
exec env LD_PRELOAD="$base/liblinuxrunner-mesa-metal.so" "$base/gpu-mesa-metal-contract"
