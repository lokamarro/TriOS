#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"

CC="${CC:-clang}"
ISH_BIN="${ISH_BIN:-$PROJECT_DIR/build-arm64-linux/ish}"
ROOTFS="${ROOTFS:-$PROJECT_DIR/alpine-arm64-fakefs}"
TIMEOUT_S="${TIMEOUT_S:-120}"
GUEST_WORK="/tmp/arm64-fcvt-vector"
HOST_TMP="$(mktemp -d)"
TEST_BIN="$HOST_TMP/fcvt-vector"
DECODER_BIN="$HOST_TMP/check-fcvt-decoders"

cleanup() {
    rm -rf "$HOST_TMP"
}
trap cleanup EXIT

test "$(uname -m)" = aarch64 || {
    echo "native ARM64 host required for the FCVT oracle" >&2
    exit 1
}
command -v "$CC" >/dev/null || { echo "missing C compiler: $CC" >&2; exit 1; }
test -x "$ISH_BIN" || { echo "missing ish binary: $ISH_BIN" >&2; exit 1; }
test -d "$ROOTFS" || { echo "missing rootfs: $ROOTFS" >&2; exit 1; }

# Check valid and reserved/other encoding collisions independently from iSH's
# generic undefined-instruction runtime behaviour.
"$CC" -O2 -Wall -Wextra -Werror "$SCRIPT_DIR/check-fcvt-decoders.c" -o "$DECODER_BIN"
"$DECODER_BIN" | grep -qx 'fcvt-decoder-mask-ok'

# Build one static ARM64 binary and use it as both the native hardware oracle
# and the iSH guest fixture. No guest compiler or libc is required.
"$CC" -O0 -static -Wall -Wextra -Werror "$SCRIPT_DIR/fcvt-vector.c" -o "$TEST_BIN"
timeout "$TIMEOUT_S" "$TEST_BIN" | grep -qx 'fcvt-vector-ok'

tar -C "$HOST_TMP" -cf - fcvt-vector |
    timeout "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/sh -c \
        "rm -rf '$GUEST_WORK' && mkdir -p '$GUEST_WORK' && tar -xf - -C '$GUEST_WORK'"

timeout "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" "$GUEST_WORK/fcvt-vector" |
    grep -qx 'fcvt-vector-ok'

echo 'fcvt-vector-gate-ok'
