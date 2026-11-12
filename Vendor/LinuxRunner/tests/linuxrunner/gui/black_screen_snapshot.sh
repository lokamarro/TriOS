#!/bin/sh
# Explicit test-only root observer. No environment or memory content capture.
set -u
session=$1
for sample in $(seq 1 72); do
    delay=5
    sleep "$delay"
    test ! -e "$session/stop" || exit 0
    export DISPLAY=":$(cat "$session/display")" XAUTHORITY="$session/authority"
    cat /proc/ish/fds >> "$session/fd-trends.log"
    /home/mobile/Downloads/runtime-snapshot "$session/runtime-snapshots.log" || exit 1
    # Opt-in runtime IPC ring (ISH_IPC_TRACE); a disabled runtime only reports that.
    case "$sample" in 6|18|30|42|54|66) cat /proc/ish/ipctrace >> "$session/ipc-trace-status.log" 2>&1 || true;; esac
done
