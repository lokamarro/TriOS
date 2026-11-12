#!/bin/bash
# Read-only prerequisite checks. Installs nothing and never uses sudo.
set -uo pipefail
cd "$(dirname "$0")/.."
missing=0
if [[ $(uname -s) != Darwin ]]; then echo 'macOS is required for iOS builds.' >&2; exit 1; fi
if [[ -z ${DEVELOPER_DIR:-} && -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
for tool in git meson ninja cmake python3 curl tar file shasum ditto ld.lld; do
    if command -v "$tool" >/dev/null 2>&1; then
        printf 'OK %-10s %s\n' "$tool" "$(command -v "$tool")"
    else
        printf 'MISSING %s\n' "$tool" >&2; missing=1
    fi
done
if ! xcodebuild -version; then echo 'Install full Xcode or set DEVELOPER_DIR to its Contents/Developer directory.' >&2; missing=1; fi
if ! xcrun --sdk iphoneos --show-sdk-path; then echo 'The iPhoneOS SDK is unavailable.' >&2; missing=1; fi
if git submodule status --recursive -- deps/libapps deps/libarchive deps/glslang deps/spirv-cross | python3 -c 'import sys; sys.exit(any(line.startswith(("-", "+", "U")) for line in sys.stdin))'; then
    echo 'OK submodules are initialized at pinned revisions.'
else
    echo 'Initialize pinned submodules: git submodule update --init --recursive --checkout -- deps/libapps deps/libarchive deps/glslang deps/spirv-cross' >&2; missing=1
fi
if (( missing )); then
    echo 'Install missing Meson/Ninja with brew install meson ninja, or a project-local Python venv; ld.lld with brew install lld (builds the guest vDSO). Put them on PATH before building.' >&2
    exit 1
fi
printf 'Ready. Signing is checked separately by build-ipa.sh.\n'
