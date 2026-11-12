#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host threads the guest kernel creates carry their names.

Boots a guest to a shell and lists the emulator's threads with libproc, as
host tools and crash reports see them. The kernel's own threads must be
there by name, each once: a carrier per vCPU (uml-vcpu-N-carrier), the IRQ
notification thread (uml-irq-notify) and the RTC alarm helper
(uml-rtc-alarm). A Darwin kernel starts no window-size helper for a terminal
console (guest-console-session). The contract is in
kernel/common/README.md. macOS only.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import collections
import ctypes
import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


VCPUS = 2
PROC_PIDTHREADINFO = 5
PROC_PIDLISTTHREADS = 6
SINGLE_THREADS = ("uml-irq-notify", "uml-rtc-alarm")


class ThreadInfo(ctypes.Structure):
    """struct proc_threadinfo from <sys/proc_info.h>."""

    _fields_ = [
        ("pth_user_time", ctypes.c_uint64),
        ("pth_system_time", ctypes.c_uint64),
        ("pth_cpu_usage", ctypes.c_int32),
        ("pth_policy", ctypes.c_int32),
        ("pth_run_state", ctypes.c_int32),
        ("pth_flags", ctypes.c_int32),
        ("pth_sleep_time", ctypes.c_int32),
        ("pth_curpri", ctypes.c_int32),
        ("pth_priority", ctypes.c_int32),
        ("pth_maxpriority", ctypes.c_int32),
        ("pth_name", ctypes.c_char * 64),
    ]


def thread_names(process_id: int) -> list[str]:
    """The names of a process's threads, '' for an unnamed one."""
    libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    handles = (ctypes.c_uint64 * 4096)()
    size = libproc.proc_pidinfo(
        process_id, PROC_PIDLISTTHREADS, 0, handles, ctypes.sizeof(handles)
    )
    if size <= 0:
        raise RuntimeError(
            f"cannot list the threads of {process_id}: errno "
            f"{ctypes.get_errno()}"
        )
    names = []
    for handle in handles[: size // ctypes.sizeof(ctypes.c_uint64)]:
        if not handle:
            continue
        info = ThreadInfo()
        if libproc.proc_pidinfo(
            process_id,
            PROC_PIDTHREADINFO,
            ctypes.c_uint64(handle),
            ctypes.byref(info),
            ctypes.sizeof(info),
        ) != ctypes.sizeof(info):
            continue  # the thread ended meanwhile
        names.append(info.pth_name.decode(errors="replace"))
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}",),
        init_shell=True,
        engine=arguments.engine,
    ) as guest:
        guest.boot_to_shell(180)
        names = thread_names(guest.process_id)
        counts = collections.Counter(names)
        print(f"{len(names)} threads: " + ", ".join(
            f"{name or '(unnamed)'} x{count}" for name, count in sorted(counts.items())
        ))
        missing = [
            name
            for name in (
                *(f"uml-vcpu-{cpu}-carrier" for cpu in range(VCPUS)),
                *SINGLE_THREADS,
            )
            if counts[name] != 1
        ]
        if missing:
            raise RuntimeError(
                "kernel threads not found exactly once by name: "
                + ", ".join(f"{name} x{counts[name]}" for name in missing)
            )
        extra_carriers = [
            name
            for name in counts
            if name.startswith("uml-vcpu-")
            and name not in {f"uml-vcpu-{cpu}-carrier" for cpu in range(VCPUS)}
        ]
        if extra_carriers:
            raise RuntimeError(f"unexpected carrier names: {extra_carriers}")
        if not guest.shutdown(60):
            raise RuntimeError("the guest did not power off within 60 s")
        print("every kernel-created thread is named")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
