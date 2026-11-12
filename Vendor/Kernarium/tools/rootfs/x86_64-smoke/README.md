# x86-64 smoke rootfs

This producer creates the smallest immutable x86-64 userspace used to qualify
the dedicated 4 KiB Darwin UML x86_64-userspace kernel slice. Kernel-mode
execution remains arm64, but the slice does not load arm64 userspace. It is
intentionally separate from the accepted general-purpose rootfs.

Run `build.sh OUTPUT.erofs` on an x86-64 Linux builder with GNU binutils,
`busybox-static`, and `erofs-utils`. The image contains only x86-64 ELF
executables. Its PID 1 checks write syscalls, FS-base TLS addressing, signal
delivery, the native x86 XSAVE signal-frame layout, and x87/XMM/YMM restoration
through `rt_sigreturn`, then prints one success line and powers the guest off.
BusyBox is present for follow-on instruction-coverage work but is not part of
the initial boot path.

Validate the resulting image with
`tests/integration/x86_64/run_guest_x86_64_smoke.py`. The runner requires a
4 KiB x86_64-slice kernel artifact and the emulator built with
`cmake --preset macos-x86_64` plus `cmake --build --preset macos-x86_64`.
It selects the gadget engine, boots `/init`, and accepts the run only after the
guest reports its syscall, TLS, and signal marker and powers itself off cleanly.
