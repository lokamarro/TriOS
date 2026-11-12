#!/bin/sh
# runmacro.sh RUNTIME — real-program workloads (macro.sh) inside the Debian fakefs.
ISH_BIND_MOUNTS=/mnt/bench=${LR_BENCH_BIN:?} "$1" -f "${LR_BENCH_ROOTFS:?}" /bin/sh /mnt/bench/macro.sh 2>&1 | grep "^MACRO"
