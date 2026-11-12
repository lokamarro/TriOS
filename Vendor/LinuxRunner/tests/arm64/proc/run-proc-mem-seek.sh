#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"

CC="${CC:-clang}"
ISH_BIN="${ISH_BIN:-$PROJECT_DIR/build-arm64-linux/ish}"
ROOTFS="${ROOTFS:-$PROJECT_DIR/debian-arm64-fakefs}"
TIMEOUT_S="${TIMEOUT_S:-120}"
GUEST_WORK="/tmp/arm64-proc-mem-seek"
HOST_TMP="$(mktemp -d)"
TEST_BIN="$HOST_TMP/proc-mem-seek"

cleanup() {
    rm -rf "$HOST_TMP"
}
trap cleanup EXIT

test "$(uname -m)" = aarch64 || {
    echo "native ARM64 host required for the proc-mem seek oracle" >&2
    exit 1
}
command -v "$CC" >/dev/null || { echo "missing C compiler: $CC" >&2; exit 1; }
test -x "$ISH_BIN" || { echo "missing ish binary: $ISH_BIN" >&2; exit 1; }
test -d "$ROOTFS" || { echo "missing rootfs: $ROOTFS" >&2; exit 1; }

# Build one static ARM64 fixture and run identical bytes natively and under
# iSH. This catches the old NULL show callback crash and compares the full
# seek matrix against the host kernel rather than relying on guest tooling.
"$CC" -O2 -static -Wall -Wextra -Werror "$SCRIPT_DIR/proc-mem-seek.c" -o "$TEST_BIN"
timeout "$TIMEOUT_S" "$TEST_BIN" | grep -qx 'proc-mem-seek-ok'

tar -C "$HOST_TMP" -cf - proc-mem-seek |
    timeout "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" /bin/sh -c \
        "rm -rf '$GUEST_WORK' && mkdir -p '$GUEST_WORK' && tar -xf - -C '$GUEST_WORK'"

timeout "$TIMEOUT_S" "$ISH_BIN" -f "$ROOTFS" "$GUEST_WORK/proc-mem-seek" |
    grep -qx 'proc-mem-seek-ok'

echo 'proc-mem-seek-gate-ok'
