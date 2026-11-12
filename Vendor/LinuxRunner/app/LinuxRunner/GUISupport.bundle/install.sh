#!/bin/sh
set -eu
. "$(dirname "$0")/support-common.sh"
verify_resources
[ "$(uname -m)" = aarch64 ] || support_fail GUI_SUPPORT architecture
if command -v apk >/dev/null; then
    packages='xvfb x11vnc xclock libx11-dev libxcb-dev build-base pkgconf binutils xterm font-misc-misc font-dejavu fontconfig xauth setxkbmap xmodmap'
    if ! apk info -e $packages >/dev/null 2>&1; then apk add --no-cache $packages; fi
elif command -v apt-get >/dev/null; then
    /usr/lib/linuxrunner/initialize-debian.sh
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends busybox procps xvfb x11vnc xauth x11-xserver-utils x11-utils x11-apps libx11-dev libxcb1-dev build-essential pkg-config binutils xterm xfonts-base fonts-dejavu-core fontconfig dbus-daemon dbus-x11 librsvg2-common
    dbus-uuidgen --ensure
elif command -v pacman >/dev/null; then
    /bin/sh ./desktop-profile.sh repair
else support_fail GUI_SUPPORT package-manager-unsupported
fi
for command in Xvfb x11vnc xauth fc-match gcc pkg-config readelf; do
    command -v "$command" >/dev/null || support_fail GUI_SUPPORT "command-missing:$command"
done
pkg-config --exists x11 xcb || support_fail GUI_SUPPORT X11-XCB-development-files
font=$(fc-match -f '%{file}' DejaVuSans)
[ -r "$font" ] || support_fail GUI_SUPPORT fonts-missing
key=$({ cat support-version; sha256sum gui-test.c focus-manager.c; pkg-config --modversion x11 xcb; } | sha256sum | awk '{print $1}')
if [ ! -x gui-test ] || [ ! -x focus-manager ] || [ "$(cat gui-build.key 2>/dev/null || true)" != "$key" ]; then
    echo 'GUI_SUPPORT: building real Linux Xlib probe and focus manager'
    gcc gui-test.c -O2 -Wall -Wextra -lX11 -o gui-test.new
    gcc focus-manager.c -O2 -Wall -Wextra -lX11 -o focus-manager.new
    chmod 755 gui-test.new focus-manager.new
    verify_elf GUI_SUPPORT "$SUPPORT_DIR/gui-test.new"
    verify_elf GUI_SUPPORT "$SUPPORT_DIR/focus-manager.new"
    mv gui-test.new gui-test; mv focus-manager.new focus-manager
    echo "$key" > gui-build.key
fi
/bin/sh ./prepare-window-observer.sh
verify_elf GUI_SUPPORT "$SUPPORT_DIR/gui-test"
# Execute actual Xlib against an owned, temporary software X server.
check=$(mktemp -d "$SUPPORT_DIR/check.XXXXXX")
xpid=
trap '[ -z "$xpid" ] || { kill "$xpid" 2>/dev/null || true; wait "$xpid" 2>/dev/null || true; }; rm -rf "$check"' EXIT
export DISPLAY=:$((9000+$$%1000)) XAUTHORITY="$check/authority"
xauth -f "$XAUTHORITY" add "$DISPLAY" . "$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')"
Xvfb "$DISPLAY" -screen 0 640x480x24 -nolisten tcp -auth "$XAUTHORITY" -extension MIT-SHM -extension GLX > "$check/xvfb.log" 2>&1 & xpid=$!
i=0
until ./gui-test --self-test > "$check/probe.log" 2>&1; do
    kill -0 "$xpid" 2>/dev/null || { cat "$check/xvfb.log"; support_fail GUI_SUPPORT Xvfb-probe-failed; }
    i=$((i+1)); [ "$i" -lt 100 ] || { cat "$check/probe.log"; support_fail GUI_SUPPORT Xlib-probe-timeout; }; sleep .1
done
cat "$check/probe.log"
echo "GUI_SUPPORT: Xvfb=$(command -v Xvfb) x11vnc=$(command -v x11vnc) xauth=$(command -v xauth) X11=yes XCB=yes fonts=$font support_scripts=verified result=PASS"
