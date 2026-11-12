#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the AArch32 engine probe on one execution engine.

The probe (engine_probe/) is a freestanding armhf program that boots as the
guest's /init and checks, from user mode, what an AArch32 task sees of the
host engine: system calls and errno, A32/T32 interworking, signal frames with
VFP state, every kuser helper, TPIDRURO and TPIDRURW, cacheflush with
self-modifying code, alignment and memory faults, UNDEFINED instructions,
ptrace and single steps through an IT block, exclusives and kuser cmpxchg
under contention across threads and processes, and then the VFP, Advanced
SIMD and Armv8 extensions, each extension where the HWCAP words show it.

A group that needs an extension the HWCAP words leave out is skipped inside
the probe, and the extension's instructions must raise SIGILL instead.

The runner boots the probe aligned to 64 KiB and the one aligned to 4 KiB
only, and fails on any failed check, any SIGILL the probe did not expect (a
guest illegal instruction is a blocker), any crash of the guest or the
emulator, and an emulator that does not power off with status 0. The
emulator's diagnostics come on a stream of their own, and the probe's lines
are read with the kernel's console lines taken out, since one can reach the
host inside a probe line (check_boot). --groups selects probe groups by hand
(A32_PROBE_GROUPS in the guest), and A32_PROBE_VERBOSE=1 makes the runner
print every line of the guest's output, passes included.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import sys
import time

# The harness's directory.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))

from guest import (  # noqa: E402
    KERNEL_LINE, ConsoleProcess, emulator_arguments, require_host_fast_paths)


SUMMARY = re.compile(
    r"^a32-engine-probe: (?P<failures>\d+) failure\(s\) in (?P<checks>\d+) "
    r"check\(s\), sigill expected (?P<expected>\d+) unexpected "
    r"(?P<unexpected>\d+)$", re.MULTILINE
)
# The probe's own lines are not host crashes, whatever they name.
PROBE_LINE = re.compile(r"^(PASS|FAIL|SKIP|NOTE) |^a32-engine-probe: ")
HOST_CRASH = re.compile(
    r"SIGILL|[Ii]llegal instruction|host fault|Kernel panic|"
    r"Attempted to kill init|segfault at")
# A kernel line can land between any two bytes of a probe line (guest.py says
# why), so kernel lines are found wherever they start.
KERNEL_RECORD = re.compile(KERNEL_LINE.decode())
# How long the probe has to power the guest off once it has reported.
POWER_OFF_SECONDS = 60
# How much of the console's end a boot without a summary prints.
SUMMARY_CONTEXT_LINES = 20


def console_parts(console: str) -> tuple[list[str], list[str]]:
    """The kernel's lines in the guest's console output, and the probe's own
    lines, rejoined where a kernel line had split one."""
    console += "\n"
    records = [record.rstrip("\r\n")
               for record in KERNEL_RECORD.findall(console)]
    return records, KERNEL_RECORD.sub("", console).splitlines()


def crashes(console: str, host: str) -> list[str]:
    """Every line that reports a crash of the guest or the emulator: any
    diagnostic of the emulator's, any kernel line, and any other line of the
    console that is not the probe's own, which names one."""
    records, lines = console_parts(console)
    return ([line for line in host.splitlines() if HOST_CRASH.search(line)]
            + [record for record in records if HOST_CRASH.search(record)]
            + [line for line in lines
               if HOST_CRASH.search(line) and not PROBE_LINE.match(line)])


def check_boot(label: str, console: str, host: str, status: int | None,
               verbose: bool = False) -> int:
    """Judge one boot from the guest's console output, the emulator's own
    output and its exit status (None if it never exited), and return the
    failures and unexpected SIGILLs the probe reported."""
    require_host_fast_paths(host)
    _, lines = console_parts(console)
    for line in lines:
        # Failures and skips are the news; the passes are a count, unless
        # A32_PROBE_VERBOSE asks for every line of the guest's output.
        if verbose or line.startswith(
                ("FAIL ", "SKIP ", "NOTE ", "a32-engine-probe: ")):
            print(line)
    found = crashes(console, host)
    if found:
        print("\n".join(found))
        raise RuntimeError(f"{label}: the guest or the emulator crashed")
    if status != 0:
        raise RuntimeError(
            f"{label}: the emulator did not power off cleanly "
            f"(status {status})")
    match = SUMMARY.search("\n".join(lines))
    if match is None:
        print("\n".join(lines[-SUMMARY_CONTEXT_LINES:]))
        raise RuntimeError(f"{label}: the probe reported no summary")
    if int(match.group("checks")) == 0:
        raise RuntimeError(f"{label}: the probe ran no checks")
    failures = int(match.group("failures"))
    unexpected = int(match.group("unexpected"))
    print(f"{label}: {match.group('checks')} check(s), "
          f"{failures} failure(s), SIGILLs expected "
          f"{match.group('expected')} unexpected {unexpected}",
          flush=True)
    return failures + unexpected


def run_probe(arguments: argparse.Namespace, label: str,
              elf: pathlib.Path) -> int:
    """Boot `elf` as /init and return the failures it reports."""
    kernel = [f"ncpus={arguments.ncpus}"]
    if arguments.groups:
        kernel.append(f"A32_PROBE_GROUPS={arguments.groups}")
    command = emulator_arguments(
        arguments.emulator, arguments.rootfs, "512M",
        (*kernel, "console=tty0", "console=ttyS0", *arguments.kernel_argument),
        engine=arguments.engine,
    )
    # The launcher's per-run initramfs takes the probe as its /init.
    command.insert(command.index(str(arguments.emulator)) + 1,
                   f"--guest-init={elf}")
    print(f"== {arguments.engine}: {label}", flush=True)
    with ConsoleProcess(command, separate_host_output=True) as guest:
        deadline = time.monotonic() + arguments.timeout
        while not guest.wait_for_exit(1):
            if SUMMARY.search("\n".join(console_parts(guest.output())[1])):
                break
            if time.monotonic() > deadline:
                raise TimeoutError(
                    f"{label}: the probe did not report in "
                    f"{arguments.timeout}s")
        # The probe powers the guest off once it has reported.
        guest.wait_for_exit(POWER_OFF_SECONDS)
        console, host = guest.output(), guest.host_output()
        status = guest.return_code
    return check_boot(label, console, host, status,
                      bool(os.environ.get("A32_PROBE_VERBOSE")))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), required=True)
    parser.add_argument("--probe", required=True, type=pathlib.Path,
                        help="the probe aligned to 64 KiB")
    parser.add_argument("--probe-4k", required=True, type=pathlib.Path,
                        help="the probe aligned to 4 KiB only")
    parser.add_argument("--groups", help="comma-separated probe groups; "
                        "default every group")
    parser.add_argument("--ncpus", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=1200, metavar="SECONDS",
                        help="the longest a probe boot may take to report")
    parser.add_argument("--kernel-argument", action="append", default=[])
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()
    name = f"guest-aarch32-engine-{arguments.engine}"

    failures = run_probe(arguments, "64 KiB layout",
                         arguments.probe.resolve())
    failures += run_probe(arguments, "4 KiB layout",
                          arguments.probe_4k.resolve())
    if failures:
        raise RuntimeError(f"{name}: {failures} failure(s)")
    print(f"{name}: the probe passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
