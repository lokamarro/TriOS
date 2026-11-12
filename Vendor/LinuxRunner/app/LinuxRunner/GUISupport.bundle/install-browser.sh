#!/bin/sh
set -eu
. "$(dirname "$0")/support-common.sh"
verify_resources
sh ./install-qt.sh
[ -f browser.cpp ] || support_fail BROWSER_INSTALL browser-source-missing
if command -v apk >/dev/null; then
    packages='qt6-qtwebengine firefox mesa-dri-gallium font-dejavu'
    if ! apk info -e $packages >/dev/null 2>&1; then apk add --no-cache $packages; fi
elif command -v apt-get >/dev/null; then
    apt-get install -y --no-install-recommends libqt6webenginewidgets6 libqt6webenginecore6-bin firefox-esr libgl1-mesa-dri fonts-dejavu-core
elif command -v pacman >/dev/null; then
    pacman -Syu --needed --noconfirm qt6-webengine firefox mesa ttf-dejavu
else support_fail BROWSER_INSTALL package-manager-unsupported; fi
# Firefox is independently installed and checked before the Qt build.
/bin/sh ./browser-preflight.sh firefox
required=yes
if browser_ready; then required=no; fi
echo "BROWSER_INSTALL: support_dir=$SUPPORT_DIR version=$SUPPORT_VERSION source=yes compiler=$(command -v g++) build_required=$required"
if [ "$required" = yes ]; then
    # Use a temporary build group; runtime launches need no WebEngine headers.
    if command -v apk >/dev/null; then
        apk add --no-cache --virtual .linuxrunner-browser-build qt6-qtwebengine-dev
    elif command -v pacman >/dev/null; then
        pacman -S --needed --noconfirm qt6-webengine
    else
        # APT dev packages may already be user-owned. Do not autoremove them.
        apt-get install -y --no-install-recommends qt6-webengine-dev
    fi
    pkg-config --exists Qt6Widgets || support_fail BROWSER_INSTALL Qt6Widgets-pkgconfig-missing
    pkg-config --exists Qt6WebEngineWidgets || support_fail BROWSER_INSTALL Qt6WebEngineWidgets-pkgconfig-missing
    echo "BROWSER_INSTALL: qt_widgets=yes qt_webengine=yes qt_version=$(pkg-config --modversion Qt6Widgets) webengine_version=$(pkg-config --modversion Qt6WebEngineWidgets)"
    echo "BROWSER_INSTALL: compiling browser.cpp with guest g++; keep LinuxRunner in foreground"
    g++ browser.cpp -std=c++17 -O0 -Wall -Wextra $(pkg-config --cflags --libs Qt6WebEngineWidgets Qt6Widgets) -o browser.new
    chmod 755 browser.new
    verify_elf BROWSER_INSTALL "$SUPPORT_DIR/browser.new"
    ./browser.new --versions
    # Commit only a completely verified binary; preserve the old one on failure.
    mv browser.new browser
    {
        echo "version=$SUPPORT_VERSION"
        echo "fingerprint=$(browser_key)"
        echo "source_sha256=$(sha256sum browser.cpp | awk '{print $1}')"
        echo "binary_sha256=$(sha256sum browser | awk '{print $1}')"
        echo "qt_widgets_version=$(pkg-config --modversion Qt6Widgets)"
        echo "qt_webengine_version=$(pkg-config --modversion Qt6WebEngineWidgets)"
    } > browser-support.meta.new
    mv browser-support.meta.new browser-support.meta
    if command -v apk >/dev/null; then apk del .linuxrunner-browser-build; fi
fi
/bin/sh ./browser-preflight.sh chromium
echo 'BROWSER_INSTALL: browser_elf=aarch64 browser_executable=yes firefox_executable=yes result=PASS'
