#!/bin/sh
# Explicit per-process backend selection; system Mesa and profiles stay intact.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! grep -q '^ID=debian$' /etc/os-release; then echo 'GPU_GEARS: Debian glibc required'; exit 1; fi
if [ ! -e /dev/linuxrunner-gpu ]; then mknod /dev/linuxrunner-gpu c 241 0; chmod 600 /dev/linuxrunner-gpu; fi
chmod 700 "$base/linuxrunner-mesa-es2-gears"
# Only names inside the application-managed support directory are selected.
ln -sf liblinuxrunner-gles-subset.so "$base/libEGL.so.1"
ln -sf liblinuxrunner-gles-subset.so "$base/libGLESv2.so.1"
export LD_LIBRARY_PATH="$base${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
echo 'GPU_GEARS: upstream Mesa GLES2 gears; limited Metal backend; not full GLES conformance'
exec "$base/linuxrunner-mesa-es2-gears" "$@"
