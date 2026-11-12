#!/bin/sh
# run.sh RUNTIME SCALE [filter] — interp_bench inside a Debian fakefs.
#   LR_BENCH_ROOTFS  Debian fakefs (needs gcc for the first build)   LR_BENCH_BIN  dir with interp_bench_arm64
ISH_BIND_MOUNTS=/mnt/bench=${LR_BENCH_BIN:?} "$1" -f "${LR_BENCH_ROOTFS:?}" /mnt/bench/interp_bench_arm64 ${2:-1} $3 2>&1 | grep "^BENCH"
