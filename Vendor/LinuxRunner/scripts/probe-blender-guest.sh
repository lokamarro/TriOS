#!/bin/bash
# Disposable fixture only: download/extract an official ARM64 package without
# installing it or changing the package database. Missing dependencies are
# deliberately reported; this is not a Blender graphics acceptance test.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${BLENDER_PROBE_ROOTFS:?Set a disposable Debian rootfs}"
: "${BLENDER_PROBE_EXCHANGE:?Set its absolute exchange directory}"
export ISH_BIND_MOUNTS="/home/mobile/Downloads=$BLENDER_PROBE_EXCHANGE"
exec "${BLENDER_PROBE_RUNTIME:-build/host/ish}" -f "$BLENDER_PROBE_ROOTFS" /bin/sh -ec '
 apt-cache policy blender
 apt-get -s --no-install-recommends install blender
 mkdir -p /home/mobile/Downloads/blender-probe-853
 cd /home/mobile/Downloads/blender-probe-853
 apt-get download blender
 set -- ./blender_*_arm64.deb
 test "$#" -eq 1
 test "$(dpkg-deb -f "$1" Architecture)" = arm64
 dpkg-deb -f "$1" Package Version Architecture
 dpkg-deb -x "$1" extracted
 sha256sum extracted/usr/bin/blender
 ldd extracted/usr/bin/blender
 exec extracted/usr/bin/blender --version
'
