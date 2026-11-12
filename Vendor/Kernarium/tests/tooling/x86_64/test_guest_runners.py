#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the command line and the probe builds of the x86-64 guest runners.

The runners under tests/integration/x86_64 are manual: they need a
macos-x86_64 uml-darwin and an x86-64 image. This test runs each as a
subprocess under python3 -I, without a guest, and checks that every runner
prints its usage and options; rejects an emulator that is not executable, a
rootfs that is not a file and a timeout that is not positive; and compiles
its freestanding probes with the compiler flags that pin their bytes. The
compiler is a stand-in that records its command line and fails once the
runner has built every probe, so no guest starts.

usage: test_guest_runners.py --runners DIRECTORY
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile

COMMON_FLAGS = [
    "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
    "-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables", "-fno-pic", "-nostdlib", "-static",
    "-fuse-ld=lld", "-Wl,-e,_start", "-Wl,-z,max-page-size=4096", "-Wl,-s",
]
X86_64 = ["--target=x86_64-unknown-linux-gnu", "-mno-red-zone"]
I386 = ["--target=i686-unknown-linux-gnu", "-march=i686"]
BOTH = "clang driver with x86-64 and i386 Linux targets and ld.lld"
ONE = "clang driver with an x86-64 Linux target and ld.lld"

HELP = "  -h, --help            show this help message and exit"
EMULATOR = "  --emulator EMULATOR"
ROOTFS = "  --rootfs ROOTFS"
ENGINE = "  --engine {gadgets,jit}"
TIMEOUT = "  --timeout TIMEOUT"


def compiler(help_text: str) -> str:
    return f"  --compiler COMPILER   {help_text}"


def options(*lines: str) -> str:
    return "\n".join((HELP, EMULATOR, ROOTFS, ENGINE, *lines)) + "\n"


# Each runner: the options argparse lists, its positivity message, and the
# probes it compiles (source, then each binary with its target flags).
RUNNERS = {
    "smoke": (options(TIMEOUT), "timeout must be positive", None),
    "fenv": (
        options(TIMEOUT).replace(ROOTFS, f"{ROOTFS}       x86-64 image with g++ and glibc"),
        "timeout must be positive", None),
    "code_change": (
        options(compiler(BOTH), f"{TIMEOUT}     boot timeout"),
        "timeout must be positive",
        ("code_change_probe.cpp",
         (("code-change-probe", X86_64), ("code-change-probe32", I386)))),
    "exceptions": (
        options(compiler(BOTH), TIMEOUT), "timeout must be positive",
        ("exception_probe.cpp",
         (("exception-probe", X86_64), ("exception-probe32", I386)))),
    "fork_cow": (
        options(compiler(BOTH), f"{TIMEOUT}     boot timeout"),
        "timeout must be positive",
        ("fork_cow_probe.cpp",
         (("fork-cow-probe", X86_64), ("fork-cow-probe32", I386)))),
    "rflags": (
        options(compiler(BOTH), TIMEOUT), "timeout must be positive",
        ("rflags_probe.cpp",
         (("rflags-probe", X86_64), ("rflags-probe32", I386)))),
    "mm_shared": (
        options(compiler(ONE), TIMEOUT), "timeout must be positive",
        # No -mno-red-zone: the probe has always been built without it.
        ("mm_shared_probe.cpp",
         (("mm-shared-probe", X86_64[:1]),))),
    "vnni": (
        options(compiler(ONE), TIMEOUT), "timeout must be positive",
        ("vnni_probe.cpp", (("vnni-probe", X86_64),))),
    "tso_litmus": (
        options(compiler(BOTH),
                "  --rounds ROUNDS       rounds of each test in each case and ABI",
                f"{TIMEOUT}     boot timeout",
                "  --probe-timeout PROBE_TIMEOUT",
                "                        timeout of one case under one ABI"),
        "rounds and timeouts must be positive",
        ("tso_litmus_probe.cpp",
         (("tso-litmus-probe", X86_64), ("tso-litmus-probe32", I386)))),
}

FAKE_COMPILER = f"""#!{sys.executable}
import json, os, sys
log = os.environ["FAKE_COMPILER_LOG"]
with open(log, "a") as file:
    file.write(json.dumps(sys.argv[1:]) + "\\n")
with open(sys.argv[sys.argv.index("-o") + 1], "wb") as file:
    file.write(b"probe")
with open(log) as file:
    calls = len(file.readlines())
sys.exit(1 if calls >= int(os.environ["FAKE_COMPILER_FAIL_AT"]) else 0)
"""


class Failures(list):
    def check(self, condition: bool, message: str) -> None:
        if not condition:
            self.append(message)
            print(f"FAIL {message}")


def run(command: list[str], environment: dict[str, str]) -> subprocess.CompletedProcess:
    return subprocess.run(
        command, env=environment, stdin=subprocess.DEVNULL, capture_output=True,
        text=True, timeout=60)


def usage_and_options(script: pathlib.Path, environment: dict[str, str]) -> tuple[int, str, str]:
    result = run([sys.executable, "-I", str(script), "--help"], environment)
    text = result.stdout
    return (result.returncode, text.partition("options:\n")[2],
            text.partition("usage: ")[2].partition("\n\n")[0])


def check_runner(failures: Failures, name: str, script: pathlib.Path,
                 positivity: str, probes, scratch: pathlib.Path,
                 environment: dict[str, str]) -> None:
    expected_options = RUNNERS[name][0]
    status, listed, usage = usage_and_options(script, environment)
    failures.check(status == 0, f"{name}: --help exits {status} under python3 -I")
    failures.check(listed == expected_options,
                   f"{name}: options differ:\n{listed}\nexpected:\n{expected_options}")
    failures.check(usage.startswith(script.name + " [-h] --emulator EMULATOR --rootfs ROOTFS"),
                   f"{name}: usage line is {usage!r}")

    executable = scratch / "emulator"
    plain = scratch / "plain-file"
    rootfs = scratch / "rootfs.erofs"
    for path in (executable, plain, rootfs):
        path.write_bytes(b"")
    executable.chmod(0o755)
    plain.chmod(0o644)
    base = [sys.executable, "-I", str(script)]
    refusals = (
        ([*base, "--rootfs", str(rootfs)], "the following arguments are required"),
        ([*base, "--emulator", str(plain), "--rootfs", str(rootfs)],
         "emulator is not executable"),
        ([*base, "--emulator", str(executable), "--rootfs", str(scratch)],
         "rootfs is not a regular file"),
        ([*base, "--emulator", str(executable), "--rootfs", str(rootfs),
          "--engine", "interpreter"], "invalid choice"),
        ([*base, "--emulator", str(executable), "--rootfs", str(rootfs),
          "--timeout", "0"], positivity),
    )
    for command, message in refusals:
        result = run(command, environment)
        failures.check(result.returncode == 2 and message in result.stderr,
                       f"{name}: {command[3:]} gave {result.returncode}: {result.stderr!r}")

    if probes is None:
        return
    source, binaries = probes
    log = scratch / "compiler.log"
    log.write_text("")
    fake = scratch / "fake-clang"
    fake.write_text(FAKE_COMPILER)
    fake.chmod(0o755)
    result = run([*base, "--emulator", str(executable), "--rootfs", str(rootfs),
                  "--compiler", str(fake), "--timeout", "1"],
                 {**environment, "FAKE_COMPILER_LOG": str(log),
                  "FAKE_COMPILER_FAIL_AT": str(len(binaries))})
    failures.check(result.returncode != 0 and "CalledProcessError" in result.stderr,
                   f"{name}: the runner did not stop at the failing compiler: "
                   f"{result.returncode} {result.stderr[-300:]!r}")
    calls = [json.loads(line) for line in log.read_text().splitlines()]
    expected = [[*target, *COMMON_FLAGS, source, "-o", binary]
                for binary, target in binaries]
    # The source and the output sit in the repository and a temporary
    # directory; their names are what a probe's bytes and its guest path pin.
    calls = [[*call[:-3], pathlib.PurePath(call[-3]).name, call[-2],
              pathlib.PurePath(call[-1]).name] for call in calls]
    failures.check(calls == expected, f"{name}: compiler commands {calls}, expected {expected}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runners", type=pathlib.Path, required=True,
                        help="tests/integration/x86_64")
    arguments = parser.parse_args()

    failures = Failures()
    environment = {**os.environ, "COLUMNS": "100"}
    for name, (_, positivity, probes) in RUNNERS.items():
        script = arguments.runners / f"run_guest_x86_64_{name}.py"
        with tempfile.TemporaryDirectory(prefix="uml-x86-runners-") as scratch:
            check_runner(failures, name, script, positivity, probes,
                         pathlib.Path(scratch), environment)
        print(f"checked {name}")
    if failures:
        print(f"{len(failures)} check(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
