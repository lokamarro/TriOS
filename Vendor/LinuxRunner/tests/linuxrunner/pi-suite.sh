#!/bin/sh
# Compile inside the selected guest; copy the four C sources to its exchange.
set -eu
cd /home/mobile/Downloads
for name in atomic contended edges stress; do
 gcc regress_futex_pi_$name.c -O2 -pthread -o regress_futex_pi_$name
 ./regress_futex_pi_$name
done
cat /proc/ish/pi-futex
echo PI_SUITE_PASS_4
