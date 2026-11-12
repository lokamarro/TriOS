#!/bin/sh
# Developer/test helper only. No authentication arguments or account inspection.
set -u
export QT_QPA_PLATFORM=xcb QT_OPENGL=software QT_QUICK_BACKEND=software LIBGL_ALWAYS_SOFTWARE=1
application_path=$(command -v telegram-desktop || command -v Telegram) || exit 127
printf 'TELEGRAM_IDENTITY: executable=%s architecture=%s\n' "$application_path" "$(uname -m)"
sha256sum "$application_path" || exit 2
if command -v dpkg-query >/dev/null; then
    dpkg-query -W telegram-desktop libqt5core5t64 2>/dev/null || true
elif command -v apk >/dev/null; then
    apk info -v telegram-desktop 2>/dev/null || true
fi
if command -v readelf >/dev/null; then
    readelf -l "$application_path" | sed -n '/Requesting program interpreter/p'
fi
printf 'TELEGRAM_IDENTITY: DISPLAY=%s QPA=%s; interpreter baseline; no credentials logged\n' "${DISPLAY:-unset}" "$QT_QPA_PLATFORM"
if [ -n "${LR_TELEGRAM_TEST_WORKDIR:-}" ]; then
    # Explicit isolated cold-profile test. Never erase or replace an old profile.
    test ! -e "$LR_TELEGRAM_TEST_WORKDIR/tdata" || { echo 'TELEGRAM_TEST: profile is not cold'; exit 2; }
    mkdir -p "$LR_TELEGRAM_TEST_WORKDIR" || exit 2
    "$application_path" -workdir "$LR_TELEGRAM_TEST_WORKDIR" -- &
else
    "$application_path" -- &
fi
application=$!
(sleep 10; echo TELEGRAM_PROGRESS_10; cat "/proc/$application/maps" /proc/ish/tasks;
 sleep 20; echo TELEGRAM_PROGRESS_30; cat /proc/ish/tasks;
 sleep 30; echo TELEGRAM_PROGRESS_60; cat /proc/ish/tasks /proc/ish/pi-futex;
 sleep 60; echo TELEGRAM_PROGRESS_120; cat /proc/ish/tasks) >&2 &
sampler=$!
trap 'kill "$application" "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null' EXIT INT TERM
status=0
wait "$application" || status=$?
kill "$sampler" 2>/dev/null; wait "$sampler" 2>/dev/null
printf 'TELEGRAM_EXIT: status=%s\n' "$status"
exit "$status"
