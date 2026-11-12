#!/bin/bash
# Mac host ARM64 interpreter regressions, using a disposable Linux fixture.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${CPU_ROOTFS:?Set CPU_ROOTFS to a disposable Debian fixture}"
: "${CPU_EXCHANGE:?Set CPU_EXCHANGE to a disposable host exchange}"
out=${CPU_OUTPUT:-build/asbestos-memory-tests}
mkdir -p "$out" "$CPU_EXCHANGE"
ninja -C build/host > "$out/build.log" 2>&1
flags=(-O2 -g -I. -Ibuild/host -DGUEST_ARM64=1 -DENGINE_ASBESTOS=1 -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -DLOG_HANDLER_DPRINTF=1)
clang "${flags[@]}" tests/linuxrunner/code_page_collision.c build/host/libish.a build/host/libish_emu.a build/host/libfakefs.a -lm -ldl -lsqlite3 -o "$out/code-page-collision" > "$out/collision-build.log" 2>&1
"$out/code-page-collision" > "$out/collision.log" 2>&1
clang "${flags[@]}" tests/linuxrunner/tlb_read_dirty.c build/host/libish.a build/host/libish_emu.a build/host/libfakefs.a -lm -ldl -lsqlite3 -o "$out/tlb-read-dirty" > "$out/tlb-read-build.log" 2>&1
"$out/tlb-read-dirty" > "$out/tlb-read.log" 2>&1
cp tests/arm64/loadstore/store-fault-pc.c tests/arm64/loadstore/unsigned-load-fault-pc.c \
   tests/linuxrunner/runtime_code_coherence.c tests/linuxrunner/exec_argument_copy.c \
   tests/linuxrunner/aes-instructions.c tests/arm64/atomics/{clrex-stxr,ldxr-widths,ldxp-stlxp,cas128}.c "$CPU_EXCHANGE/"
export ISH_BIND_MOUNTS="/home/mobile/Downloads=$CPU_EXCHANGE"
build/host/ish -f "$CPU_ROOTFS" /bin/sh -ec '
 cd /home/mobile/Downloads
 for n in store-fault-pc unsigned-load-fault-pc runtime_code_coherence exec_argument_copy; do
  gcc -O2 -Wall -Wextra "$n.c" -o "$n"; "./$n"
 done
 gcc -O2 -march=armv8-a+crypto aes-instructions.c -o aes-instructions; ./aes-instructions
 for n in clrex-stxr ldxr-widths ldxp-stlxp cas128; do
  gcc -O2 -pthread "$n.c" -latomic -o "$n"; "./$n"
 done
' > "$out/guest.log" 2>&1
# Disable only the new fusion at process startup; keep the existing build852
# LDR64 optimization. Never toggle cached gadget stream formats while running.
ISH_ARM64_MEMORY_PC_FUSION=0 build/host/ish -f "$CPU_ROOTFS" /bin/sh -ec '
 cd /home/mobile/Downloads
 ./store-fault-pc; ./unsigned-load-fault-pc; ./runtime_code_coherence
' > "$out/rollback.log" 2>&1
if command -v rg >/dev/null 2>&1; then
    rg 'PASS|ok cases=|clrex-ok|ok64|ok32|ok=|^b=' "$out/collision.log" "$out/tlb-read.log" "$out/guest.log" "$out/rollback.log"
else
    grep -E 'PASS|ok cases=|clrex-ok|ok64|ok32|ok=|^b=' "$out/collision.log" "$out/tlb-read.log" "$out/guest.log" "$out/rollback.log"
fi
