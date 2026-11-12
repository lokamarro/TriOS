#!/bin/sh
set -eu
cd "$(dirname "$0")"
key=$(sha256sum window-observer.c | awk '{print $1}')
if [ -x window-observer ] && [ "$(cat window-observer.key 2>/dev/null || true)" = "$key" ]; then exit 0; fi
echo 'X11_STARTUP: updating read-only window observer'
gcc window-observer.c -O2 -Wall -Wextra -lX11 -o window-observer.new
chmod 755 window-observer.new
mv window-observer.new window-observer
printf '%s\n' "$key" > window-observer.key
