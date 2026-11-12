# x86-64 LTP rootfs

This producer creates the immutable x86-64 userspace that runs the LTP
campaign on the dedicated 4 KiB x86_64-userspace kernel slice. It is Debian 13
amd64 with the LTP-relevant runtime packages of the arm64 product rootfs, the
toolchain and debugger that LTP command tests exercise, and LTP 20260529 built
from its release tag into `/opt/ltp`. It contains only x86-family ELF payloads
and is not a general-purpose product image.

Run `build.sh --work PATH --output PATH.erofs` as an ordinary user with
passwordless sudo on native x86-64 Debian Linux. The producer publishes the
image with its package inventory, ELF inventory, and checksum only after the
LTP version, package state, and ELF audit pass.

Run the campaign with `tests/integration/ltp/run_guest_ltp.py --guest-arch
x86_64` against an emulator built with the `macos-x86_64` preset; see
[`docs/testing.md`](../../../docs/testing.md#ltp).
