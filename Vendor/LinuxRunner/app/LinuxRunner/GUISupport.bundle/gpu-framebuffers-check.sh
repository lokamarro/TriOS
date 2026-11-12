#!/bin/sh
# On-device pixel contract through real guest GLES2 and the Metal bridge.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if ! grep -q '^ID=debian$' /etc/os-release; then echo 'GPU_FRAMEBUFFERS: Debian ARM64 glibc required'; exit 1; fi
command -v xterm >/dev/null || { echo 'GPU_FRAMEBUFFERS: install xterm in this Debian profile'; exit 1; }
if [ ! -e /dev/linuxrunner-gpu ]; then mknod /dev/linuxrunner-gpu c 241 0; chmod 600 /dev/linuxrunner-gpu; fi
chmod 700 "$base/gpu-framebuffer-contract"
export LD_LIBRARY_PATH="$base${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec xterm -hold -T 'LinuxRunner GLES2 offscreen target pixel checks' -e "$base/gpu-framebuffer-contract"
