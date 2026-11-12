#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Runs FFmpeg's VA-API encoders through libva against the driver over the
# in-memory encoder node (fake_drv_video), as a second client beside
# va_encode_probe. The stream the fake writes is not decodable, so this checks
# that FFmpeg's whole submission is accepted and every frame comes back, not
# the picture.
#
# usage: ffmpeg_selftest.sh BUILD_DIR
set -u
build=${1:?usage: ffmpeg_selftest.sh BUILD_DIR}
build=$(CDPATH= cd -- "$build" && pwd)
command -v ffmpeg >/dev/null 2>&1 || { echo "FFMPEG_SELFTEST: SKIP (no ffmpeg)"; exit 0; }
export LIBVA_DRIVER_NAME=fake
export LIBVA_DRIVERS_PATH=$build
output=$(mktemp)
trap 'rm -f -- "$output"' EXIT

failed=0
run()
{
    size=$1
    codec=$2
    shift 2
    if LD_PRELOAD="$build/drm_node_shim.so" timeout 120 ffmpeg -hide_banner -loglevel error \
        -vaapi_device /dev/null -f lavfi -i "testsrc=size=$size:rate=30" -frames:v 20 \
        -vf format=nv12,hwupload -c:v "$codec" "$@" -y "$output" </dev/null >/dev/null 2>&1 &&
        [ -s "$output" ]; then
        echo "PASS $codec $size $*"
    else
        echo "FAIL $codec $size $*"
        failed=$((failed + 1))
    fi
}
run 1920x1080 h264_vaapi -f h264
run 1920x1080 h264_vaapi -rc_mode CQP -qp 28 -f h264
run 640x360 h264_vaapi -profile:v main -rc_mode CBR -b:v 1M -g 15 -f h264
run 320x240 h264_vaapi -profile:v constrained_baseline -f h264
run 1280x720 hevc_vaapi -f hevc
run 640x360 hevc_vaapi -f hevc
run 1920x1080 hevc_vaapi -f hevc
run 854x480 h264_vaapi -f h264
run 1280x720 hevc_vaapi -rc_mode CQP -qp 30 -f hevc
echo "FFMPEG_SELFTEST: $([ "$failed" = 0 ] && echo PASS || echo "FAIL ($failed)")"
[ "$failed" = 0 ]
