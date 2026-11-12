#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs va_encode_probe through libva against the driver over the in-memory
# encoder node (fake_drv_video), on a host with no Virtio video device. The
# probe checks every frame, key frame, parameter set and stream size itself.
#
# usage: probe_selftest.sh BUILD_DIR
set -u
build=${1:?usage: probe_selftest.sh BUILD_DIR}
build=$(CDPATH= cd -- "$build" && pwd)
export LD_PRELOAD="$build/drm_node_shim.so"
export LIBVA_DRIVER_NAME=fake
export LIBVA_DRIVERS_PATH=$build

cases=(
    "--codec h264 --size 640x360"
    "--codec h264 --size 1920x1080 --profile high --rate vbr --frames 12"
    "--codec h264 --size 320x240 --rate cqp --qp 30 --idr-period 5 --frames 12"
    "--codec h264 --size 320x240 --rate icq"
    "--codec h264 --size 640x360 --bitrate-step 500000 --depth 1"
    "--codec h264 --profile baseline --size 176x144"
    "--codec hevc --size 640x360 --idr-period 10"
    "--codec hevc --size 1920x1080 --frames 6"
    "--sessions 5 --contexts 2 --frames 6"
)
failed=0
for arguments in "${cases[@]}"; do
    # shellcheck disable=SC2086
    if timeout 120 "$build/va_encode_probe" --device /dev/null $arguments </dev/null >/dev/null 2>&1; then
        echo "PASS $arguments"
    else
        echo "FAIL $arguments"
        failed=$((failed + 1))
    fi
done
echo "VA_PROBE_SELFTEST: $([ "$failed" = 0 ] && echo PASS || echo "FAIL ($failed)")"
[ "$failed" = 0 ]
