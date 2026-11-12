#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
unset LD_LIBRARY_PATH LD_PRELOAD MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
export LR_MESA_METAL_MODE=verify LR_MESA_METAL_MAX_DRAWS=128
[ -r "$base/liblinuxrunner-mesa-metal.so" ] || { echo 'BLENDER_HYBRID: interoperation provider missing'; exit 127; }
command -v blender >/dev/null || { echo 'BLENDER_HYBRID: Blender is not installed'; exit 127; }
echo 'BLENDER_HYBRID: genuine Mesa core context; eligible original draws rendered by Metal and pixel-verified; unsupported draws remain software. Not full Metal OpenGL4.3.'
blender --version
cd "${HOME:-/root}"
exec env LD_PRELOAD="$base/liblinuxrunner-mesa-metal.so" blender --debug-gpu
