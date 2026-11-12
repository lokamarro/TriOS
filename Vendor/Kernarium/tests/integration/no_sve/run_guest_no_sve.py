#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check on one execution engine that the guest shows no SVE.

Boots no_sve_probe.cpp as the guest's /init and checks that the virtual CPU
presents no SVE outside streaming mode: no AT_HWCAP or AT_HWCAP2 bit, no
prctl, sysctl, ptrace regset or signal-frame record, and undefined SVE
instructions. The check holds on every host, with the guest-SME policy on or
off, since the virtual CPU implements no SVE either way.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import sys

# The script's own directory and the harness's are on sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))

from guest import ConsoleProcess, emulator_arguments  # noqa: E402


SUMMARY = re.compile(
    rb"no-sve-probe: (?P<failures>\d+) failure\(s\) in (?P<checks>\d+) check\(s\)"
)
HOST_CRASH = re.compile(rb"SIGILL|[Ii]llegal instruction|host fault|Kernel panic")


def run_probe_boot(arguments: argparse.Namespace) -> int:
    """Boot the probe as /init; returns its failure count."""
    command = emulator_arguments(
        arguments.emulator, arguments.rootfs, "512M",
        (f"ncpus={arguments.ncpus}", "console=tty0", "console=ttyS0",
         *arguments.kernel_argument),
        engine=arguments.engine,
    )
    # The launcher's per-run initramfs takes the probe as its /init.
    command.insert(command.index(str(arguments.emulator)) + 1,
                   f"--guest-init={arguments.probe}")
    with ConsoleProcess(command) as guest:
        match = guest._read_until(SUMMARY, arguments.timeout)
        guest.wait_for_exit(30)
        output = guest.output()
        for line in output.splitlines():
            if line.startswith(("PASS ", "FAIL ", "no-sve-probe: ")):
                print(line)
        if HOST_CRASH.search(output.encode()):
            raise RuntimeError("the guest or the emulator crashed")
        if int(match.group("checks")) == 0:
            raise RuntimeError("the probe ran no checks")
        return int(match.group("failures"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True, help="the registered test name")
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), required=True)
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--ncpus", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--kernel-argument", action="append", default=[])
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()
    arguments.probe = arguments.probe.resolve()
    print(f"== {arguments.name}", flush=True)
    failures = run_probe_boot(arguments)
    if failures:
        raise RuntimeError(f"{arguments.name}: {failures} failure(s)")
    print(f"{arguments.name}: passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
