#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check what glibc shows of x86's sticky floating-point exception flags.

Boots an x86-64 image with a C++ toolchain and glibc, such as the x86-64 LTP
rootfs, on an emulator built with the macos-x86_64 preset, builds
fenv_probe.cpp there with g++, and runs it. With every exception masked, 29
operations -- SSE arithmetic and conversions, comparisons, libm calls,
feraiseexcept, the rounding functions, x87 arithmetic, an FTZ flush and a
DAZ input -- must leave what fetestexcept() and STMXCSR showed on an Intel
Xeon 8581C, and glibc's fadd, which reads the precision flag to round to
odd, must return the Xeon's value. The documented exception is DE while DAZ
is clear, which no host flag shows (user decision 2026-10-07).
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

from guest import Guest, GuestFile  # noqa: E402
from probe_runner import GUEST_DIRECTORY, parse_arguments  # noqa: E402


PROBE = pathlib.Path(__file__).with_name("fenv_probe.cpp")
GUEST_SOURCE = f"{GUEST_DIRECTORY}/fenv_probe.cpp"
GUEST_BINARY = "/tmp/fenv-probe"
BUILD = (f"g++ -std=c++20 -O2 -Wall -Wextra -Werror -o {GUEST_BINARY} "
         f"{GUEST_SOURCE}")
PROBE_TIMEOUT_SECONDS = 900
DENORMAL_OPERAND = 0x02

# The probe built with g++ 14.2 -O2 against glibc 2.41 on Debian 13, run on
# an Intel Xeon Platinum 8581C: operation, fetestexcept(), MXCSR.
XEON = """\
div0 ZE 1f84
inv IE 1f81
sqrtneg IE 1f81
ovf OE,PE 1fa8
unf UE,PE 1fb0
inexact PE 1fa0
exact - 1f80
denorm - 1f82
cvtnan IE 1f81
cvtbig IE 1f81
cmpnan IE 1f81
eqnan - 1f80
fltovf OE,PE 1fa8
rcp - 1f80
log0 ZE 1f84
expbig OE,PE 1fa8
expsmall UE,PE 1fb0
powneg IE 1f81
sinbig PE 1fa0
raisediv ZE 1f84
raiseinv IE 1f81
raiseovf OE 1f80
nearbyint - 1f80
lrint PE 1fa0
rint PE 1fa0
x87div PE 1f80
x87sin PE 1fa0
ftzflush UE,PE 1fb0
dazdenorm - 1f80
"""
XEON_NARROW = 0x3F800001


def expected() -> dict[str, tuple[set[str], int]]:
    cases = {}
    for line in XEON.splitlines():
        name, flags, mxcsr = line.split()
        cases[name] = (set() if flags == "-" else set(flags.split(",")),
                       int(mxcsr, 16))
    return cases


def parse(output: str) -> tuple[dict[str, tuple[set[str], int]], int | None]:
    cases = {}
    narrow = None
    for line in output.splitlines():
        fields = line.split()
        if fields[:3] == ["fenv", "narrow", "fadd"] and len(fields) == 4:
            narrow = int(fields[3], 16)
        elif (len(fields) >= 5 and fields[0] == "fenv" and
              fields[2] == "fetestexcept" and fields[-2] == "mxcsr"):
            flags = set(fields[3:-2]) - {"-"}
            cases[fields[1]] = (flags, int(fields[-1], 16))
    return cases, narrow


def compare(actual: tuple[set[str], int], want: tuple[set[str], int]) -> str:
    """'ok', 'residual' or 'FAIL'."""
    flags, mxcsr = actual
    want_flags, want_mxcsr = want
    if flags == want_flags and mxcsr == want_mxcsr:
        return "ok"
    # DE, which fetestexcept cannot show, while DAZ (bit 6) is clear.
    if flags == want_flags and mxcsr | DENORMAL_OPERAND == want_mxcsr and \
            want_mxcsr & 0x40 == 0:
        return "residual"
    return "FAIL"


def main() -> int:
    arguments = parse_arguments(
        __doc__, compiler_help=None, rootfs_help="x86-64 image with g++ and glibc")

    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        kernel_arguments=("ncpus=10",),
        init_shell=True,
        engine=arguments.engine,
        payloads={GUEST_SOURCE: GuestFile(PROBE.read_bytes(), 0o644)},
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        result = guest.run_command(
            f"{BUILD} && {GUEST_BINARY} flags && {GUEST_BINARY} narrow",
            PROBE_TIMEOUT_SECONDS,
        )
    print(result.output.strip())
    if result.return_code != 0:
        raise RuntimeError(f"the probe failed with status {result.return_code}")
    cases, narrow = parse(result.output)
    failures = 0
    for name, want in expected().items():
        if name not in cases:
            print(f"FAIL {name}: no result")
            failures += 1
            continue
        status = compare(cases[name], want)
        if status != "ok":
            flags, mxcsr = cases[name]
            print(f"{status} {name}: fetestexcept {sorted(flags)} mxcsr "
                  f"{mxcsr:04x}, x86 {sorted(want[0])} {want[1]:04x}")
        failures += status == "FAIL"
    if narrow != XEON_NARROW:
        print(f"FAIL narrow: fadd {narrow if narrow is None else hex(narrow)}"
              f", x86 {XEON_NARROW:08x}")
        failures += 1
    if failures:
        raise RuntimeError(f"{failures} result(s) differ from the Xeon's")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
