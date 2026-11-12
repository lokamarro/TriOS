#!/bin/sh
set -eu
. "$(dirname "$0")/support-common.sh"
sh ./install.sh
if command -v apk >/dev/null; then
    packages='qt6-qtbase-dev qt6-qtbase-x11 font-dejavu'
    if ! apk info -e $packages >/dev/null 2>&1; then apk add --no-cache $packages; fi
elif command -v pacman >/dev/null; then
    pacman -Syu --needed --noconfirm qt6-base ttf-dejavu
else
    apt-get install -y --no-install-recommends qt6-base-dev qt6-qpa-plugins fonts-dejavu-core
fi
pkg-config --exists Qt6Widgets || support_fail QT_SUPPORT Qt6Widgets-missing
key=$({ cat support-version; sha256sum qt-test.cpp; pkg-config --modversion Qt6Widgets; } | sha256sum | awk '{print $1}')
if [ ! -x qt-test ] || [ "$(cat qt-build.key 2>/dev/null || true)" != "$key" ]; then
    g++ qt-test.cpp -std=c++17 -O2 -Wall -Wextra $(pkg-config --cflags --libs Qt6Widgets) -o qt-test.new
    chmod 755 qt-test.new; verify_elf QT_SUPPORT "$SUPPORT_DIR/qt-test.new"
    mv qt-test.new qt-test; echo "$key" > qt-build.key
fi
verify_elf QT_SUPPORT "$SUPPORT_DIR/qt-test"
echo "QT_SUPPORT: Qt=$(pkg-config --modversion Qt6Widgets) QPA=xcb result=PASS"
