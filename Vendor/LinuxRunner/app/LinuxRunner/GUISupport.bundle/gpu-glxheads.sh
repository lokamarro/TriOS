#!/bin/sh
# Unmodified upstream GLX application, explicitly selected experimental backend.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! grep -q '^ID=debian$' /etc/os-release; then echo 'GPU_GLX: Debian ARM64 glibc required'; exit 1; fi
if [ ! -e /dev/linuxrunner-gpu ]; then mknod /dev/linuxrunner-gpu c 241 0; chmod 600 /dev/linuxrunner-gpu; fi
mkdir -p "$base/metal-glx"
ln -sf ../liblinuxrunner-glx-metal.so "$base/metal-glx/libGL.so.1"
chmod 700 "$base/linuxrunner-mesa-glxheads"
export LD_LIBRARY_PATH="$base/metal-glx:$base"
echo 'GPU_GLX: actual Metal draw dispatch; limited desktop GL subset, not OpenGL 4.3 or Blender acceleration'
exec "$base/linuxrunner-mesa-glxheads" "$@"
