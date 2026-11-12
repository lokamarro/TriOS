#!/bin/sh
set -eu
cd "$(dirname "$0")"
# Explicit reference renderer; neither scoped Metal libraries nor capability
# overrides may contaminate the software diagnostic. System packages stay put.
unset LD_LIBRARY_PATH LD_PRELOAD MESA_GL_VERSION_OVERRIDE MESA_GLSL_VERSION_OVERRIDE
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
chmod 700 ./gpu-mesa-contract
echo 'GPU_MESA: real system GLX 4.3 context and shader/pixel test; software rendering'
exec ./gpu-mesa-contract
