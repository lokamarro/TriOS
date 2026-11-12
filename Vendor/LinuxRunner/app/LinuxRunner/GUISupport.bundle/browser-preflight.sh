#!/bin/sh
set -eu
. "$(dirname "$0")/support-common.sh"
target=${1:-}
case "$target" in
 chromium)
    verify_shm_namespace BROWSER_PREFLIGHT
    echo "BROWSER_PREFLIGHT: environment=${LINUXRUNNER_ENVIRONMENT:-current} support_version=$SUPPORT_VERSION support_dir=$SUPPORT_DIR DISPLAY=${DISPLAY:-unset}"
    [ -f browser.cpp ] || support_fail BROWSER_PREFLIGHT browser-source-missing
    verify_resources
    [ -f browser ] || { [ ! -f install-result ] || echo "BROWSER_PREFLIGHT: previous_install_exit=$(cat install-result) log=$SUPPORT_DIR/install-browser.sh.log"; support_fail BROWSER_PREFLIGHT browser-binary-missing 127; }
    verify_elf BROWSER_PREFLIGHT "$SUPPORT_DIR/browser"
    [ -f browser-support.meta ] || support_fail BROWSER_PREFLIGHT browser-metadata-missing
    [ "$(metadata_value browser-support.meta version)" = "$SUPPORT_VERSION" ] || support_fail BROWSER_PREFLIGHT support-version-mismatch
    [ "$(metadata_value browser-support.meta fingerprint)" = "$(browser_key)" ] || support_fail BROWSER_PREFLIGHT source-or-Qt-version-changed
    [ "$(metadata_value browser-support.meta binary_sha256)" = "$(sha256sum browser | awk '{print $1}')" ] || support_fail BROWSER_PREFLIGHT binary-checksum-mismatch
    ./browser --versions || support_fail BROWSER_PREFLIGHT Qt-runtime-load-failed
    echo "BROWSER_PREFLIGHT: qt_version=$(metadata_value browser-support.meta qt_widgets_version) qt_webengine_version=$(metadata_value browser-support.meta qt_webengine_version) result=PASS"
    ;;
 firefox)
    verify_shm_namespace FIREFOX_PREFLIGHT
    launcher=$(command -v firefox || command -v firefox-esr) || support_fail FIREFOX_PREFLIGHT firefox-package-missing 127
    [ -x "$launcher" ] || support_fail FIREFOX_PREFLIGHT firefox-launcher-not-executable 126
    binary=$launcher
    # Alpine legitimately packages a shell launcher. Resolve its actual ELF
    # using the package file list, while keeping the packaged launcher for exec.
    if ! readelf -h "$binary" >/dev/null 2>&1; then
        binary=
        if command -v apk >/dev/null; then files=$(apk info -L firefox)
        elif command -v dpkg-query >/dev/null; then files=$(dpkg-query -L firefox-esr 2>/dev/null || dpkg-query -L firefox 2>/dev/null)
        else support_fail FIREFOX_PREFLIGHT package-manager-unsupported; fi
        for path in $files; do
            case "$path" in */firefox|*/firefox-bin|*/firefox-esr)
                if readelf -h "/$path" >/dev/null 2>&1; then binary=/$path; break; fi;;
            esac
        done
        [ -n "$binary" ] || support_fail FIREFOX_PREFLIGHT firefox-package-ELF-missing 126
    fi
    verify_elf FIREFOX_PREFLIGHT "$binary"
    "$launcher" --version || support_fail FIREFOX_PREFLIGHT firefox-version-failed
    echo "FIREFOX_PREFLIGHT: launcher=$launcher executable=$binary architecture=aarch64 DISPLAY=${DISPLAY:-unset} profile_dir=$HOME/.local/share/linuxrunner-firefox result=PASS"
    ;;
 *) echo 'Expected chromium or firefox'; exit 2;;
esac
