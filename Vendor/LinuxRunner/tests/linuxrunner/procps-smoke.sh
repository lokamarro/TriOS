#!/bin/sh
set -eu
printf 'PROCPS_VERSION: '; ps --version | head -1
ps -ef
ps aux
pgrep -x sh
pidof sh
readlink /proc/self
cat /proc/self/comm /proc/self/status /proc/self/stat /proc/loadavg
ls /proc/self/task
cat /proc/self/fdinfo/0
# Batch top should parse the same CPU and boot-time data without an interactive tty.
top -b -n 1 | head -20
sleep 30 & child=$!
kill "$child"
wait "$child" || :
printf 'PROCPS_SMOKE: PASS\n'
