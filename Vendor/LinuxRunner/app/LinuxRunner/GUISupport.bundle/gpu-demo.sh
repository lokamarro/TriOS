#!/bin/sh
# Opt-in experimental Linux EGL/GLES -> Metal test. No Mesa/profile changes.
set -eu
if ! grep -q "^ID=debian$" /etc/os-release; then
    echo "GPU_BRIDGE: bundled prototype requires Debian ARM64 glibc; existing graphics unchanged"
    exit 1
fi
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ ! -e /dev/linuxrunner-gpu ]; then
    mknod /dev/linuxrunner-gpu c 241 0 || { echo 'GPU_BRIDGE: cannot create experimental device'; exit 1; }
    chmod 600 /dev/linuxrunner-gpu
fi
# Deployment deliberately gives data assets mode 0600; only this guest demo
# is made executable on explicit launch, inside the managed exchange folder.
chmod 700 "$base/linuxrunner-gpu-demo"
export LD_LIBRARY_PATH="$base${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$base/linuxrunner-gpu-demo" "$@"
