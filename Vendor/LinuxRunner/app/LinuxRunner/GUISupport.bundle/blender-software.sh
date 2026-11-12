#!/bin/sh
set -eu
cd "$(dirname "$0")"
unset LD_LIBRARY_PATH LD_PRELOAD MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
chmod 700 ./gpu-mesa-contract
echo 'BLENDER_SOFTWARE: validating real Mesa context before starting installed Blender; existing profile retained'
./gpu-mesa-contract || { echo 'BLENDER_SOFTWARE: context validation failed; see GLX_DIAGNOSTIC_FAIL and GLX_LIBRARY above'; exit 1; }
command -v blender >/dev/null || { echo 'BLENDER_SOFTWARE: Blender is not installed in this distribution'; exit 127; }
blender --version
cd "${HOME:-/root}"
exec blender --debug-gpu
