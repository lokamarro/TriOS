#!/bin/sh
# Explicit client provider; never replaces system Mesa or user configuration.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! grep -q '^ID=debian$' /etc/os-release; then echo 'GPU_GLX: Debian ARM64 glibc required'; exit 1; fi
command -v xterm >/dev/null || { echo 'GPU_GLX: install xterm in this profile'; exit 1; }
if [ ! -e /dev/linuxrunner-gpu ]; then mknod /dev/linuxrunner-gpu c 241 0; chmod 600 /dev/linuxrunner-gpu; fi
mkdir -p "$base/metal-glx"
ln -sf ../liblinuxrunner-glx-metal.so "$base/metal-glx/libGL.so.1"
chmod 700 "$base/gpu-glx-contracts.sh"
exec xterm -hold -T 'LinuxRunner GLX Metal rendering / resource checks' -e /bin/sh "$base/gpu-glx-contracts.sh"
