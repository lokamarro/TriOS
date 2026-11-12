#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the user RFLAGS contract of the x86_64-userspace slice.

Cross-builds the freestanding rflags_probe.cpp for x86-64 and i386, boots an
x86-64 image on an emulator built with the macos-x86_64 preset, and runs the
probes there. As on Linux x86, execve() starts a task with RFLAGS 0x202, a
tracer changes only ptrace's FLAG_MASK bits, and sigreturn restores only
FIX_EFLAGS; IOPL, VM, VIF, VIP and ID stay the task's, and IF stays set. RF
from IRET or sigreturn is gone by the next instruction's system call, fault
frames and a REP string interrupted between elements show RF while trap frames
do not, and TF set by IRET traps after the instruction IRET returns to. POPF
at CPL 3 loads only the flags a task may change, IRET those and RF, and PUSHF
stores no RF; IRET without REX.W pops a 32-bit frame. RDRAND and RDSEED,
which CPUID advertises, succeed and leave only CF of the status flags set.
The REP case signals from another CPU, which the guest's ten vCPUs
provide.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
# python3 -I leaves the script's own directory off sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from guest import Guest  # noqa: E402
from probe_runner import (  # noqa: E402
    GUEST_DIRECTORY, I386, X86_64, build_probes, parse_arguments)


PROBE = pathlib.Path(__file__).with_name("rflags_probe.cpp")
# The x86-64 probe reads the flags with pushfq below its stack pointer.
PROBES = {
    "rflags-probe": ("x86-64", X86_64),
    "rflags-probe32": ("i386", I386),
}
PROBE_TIMEOUT_SECONDS = 120


def main() -> int:
    arguments = parse_arguments(__doc__)

    payloads = build_probes(arguments.compiler, PROBE, PROBES)
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "512M",
        "tmpfs",
        kernel_arguments=("ncpus=10",),
        init_shell=True,
        engine=arguments.engine,
        payloads=payloads,
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        result = guest.run_command(
            f"{GUEST_DIRECTORY}/rflags-probe {GUEST_DIRECTORY}/rflags-probe32",
            PROBE_TIMEOUT_SECONDS,
        )
        print(result.output.strip())
        if result.return_code != 0:
            raise RuntimeError(
                f"the RFLAGS probe reported {result.return_code} failure(s)"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
