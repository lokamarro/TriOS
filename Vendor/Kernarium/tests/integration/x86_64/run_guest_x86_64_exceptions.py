#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check how the x86_64-userspace slice delivers x86 exceptions and interrupts.

Cross-builds the freestanding exception_probe.cpp for x86-64 and i386, boots an
x86-64 image on an emulator built with the macos-x86_64 preset, and runs the
probes there. As on Linux x86, a 1 ms interval timer keeps firing while a task
runs long repeated string instructions; ICEBP, BOUND, INT 4, INTO, INT n, UD2,
DIV and IDIV, page faults, non-canonical addresses, pushes through a bad
stack pointer again and again on one CPU, misaligned accesses under RFLAGS.AC
and unmasked SSE and x87 exceptions deliver Linux's signals, codes, trap
numbers and error codes; system calls with a bad stack pointer return;
VMASKMOV and VPMASKMOV access only their enabled lanes; AVX2 gathers complete
the elements before a fault; a faulting store or load leaves the state its
instruction found; masked x87 exceptions set
sticky status flags and FPREM, FSIN and FSINCOS keep a zero's sign; an x87
stack survives a signal; a signal taken between two instructions leaves the
accumulator alone; and a tracer of either ABI reads and writes registers, FP
state, debug registers and TLS, steps a repeated string instruction one
iteration at a time, and changes or skips a system call through orig_ax. The
kernel must relay every such signal without complaint.
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


PROBE = pathlib.Path(__file__).with_name("exception_probe.cpp")
PROBES = {
    "exception-probe": ("x86-64", X86_64),
    "exception-probe32": ("i386", I386),
}
PROBE_TIMEOUT_SECONDS = 300
# relay_signal() prints this for a host signal it cannot classify.
RELAY_COMPLAINT = "Attempted to relay unknown signal"


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
            f"{GUEST_DIRECTORY}/exception-probe "
            f"{GUEST_DIRECTORY}/exception-probe32",
            PROBE_TIMEOUT_SECONDS,
        )
        print(result.output.strip())
        complaints = [
            line.strip()
            for line in guest.output().splitlines()
            if RELAY_COMPLAINT in line
        ]
    for line in complaints:
        print(f"FAIL kernel: {line}")
    if result.return_code != 0 or complaints:
        raise RuntimeError(
            f"the exception probe reported {result.return_code} failure(s)"
            f" and the kernel {len(complaints)} relay complaint(s)"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
