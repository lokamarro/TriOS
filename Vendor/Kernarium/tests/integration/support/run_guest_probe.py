#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run one guest probe and fail on its exit status.

The guest boots an init shell on a tmpfs root with /tmp mounted, runs the
probe, prints its output, and then runs one more command, so a probe that
leaves the kernel unable to start a task fails too. A .py probe runs with
python3 -B. A .cpp probe is installed as source, built in the guest with
c++ -std=c++20 -O2 and --cxxflags, and run. Each probe's header states what
it checks; its exit status is its failure count. --console-failure also
fails the run on a console line, the kernel's messages included, that matches.
--guest-argument adds an emulator argument, such as --guest-no-top-byte-ignore,
and --probe-argument an argument of the probe. --probe-input runs a host
executable once, before the guest boots, and installs its standard output as
the file /usr/local/libexec/uml-test/probe-input, which the probe reads; it
gives a probe data that a host model produces, so the probe holds no copy.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import shlex
import subprocess

# The script's own directory, the harness's, is first on sys.path.
from guest import Guest, GuestFile


GUEST_DIRECTORY = "/usr/local/libexec/uml-test"
GUEST_BINARY = "/tmp/probe"
GUEST_PROBE_INPUT = f"{GUEST_DIRECTORY}/probe-input"
BUILD_TIMEOUT_SECONDS = 180
PROBE_INPUT_TIMEOUT_SECONDS = 60


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--ncpus", required=True, type=int)
    parser.add_argument(
        "--probe-timeout", required=True, type=int, metavar="SECONDS"
    )
    parser.add_argument(
        "--cxxflags",
        default="",
        help='compiler flags of a .cpp probe, as --cxxflags="FLAG FLAG"',
    )
    parser.add_argument(
        "--guest-argument",
        action="append",
        default=[],
        metavar="ARGUMENT",
        help="an extra emulator argument, as --guest-argument=ARGUMENT",
    )
    parser.add_argument(
        "--probe-argument",
        action="append",
        default=[],
        metavar="ARGUMENT",
        help="an argument of the probe, as --probe-argument=ARGUMENT",
    )
    parser.add_argument(
        "--probe-input",
        type=pathlib.Path,
        metavar="EXECUTABLE",
        help="a host executable whose output the probe reads from "
        f"{GUEST_PROBE_INPUT}",
    )
    parser.add_argument(
        "--build-timeout",
        type=int,
        default=BUILD_TIMEOUT_SECONDS,
        metavar="SECONDS",
        help="how long the guest has to build a .cpp probe",
    )
    parser.add_argument(
        "--console-failure",
        type=re.compile,
        metavar="REGEX",
        help="fail when a console line matches",
    )
    arguments = parser.parse_args()
    compiled = arguments.probe.suffix == ".cpp"
    if arguments.cxxflags and not compiled:
        parser.error("--cxxflags applies to a .cpp probe")
    if arguments.build_timeout != BUILD_TIMEOUT_SECONDS and not compiled:
        parser.error("--build-timeout applies to a .cpp probe")
    pattern = arguments.console_failure
    guest_probe = f"{GUEST_DIRECTORY}/probe{arguments.probe.suffix}"
    payloads = {guest_probe: GuestFile(arguments.probe.read_bytes())}
    if arguments.probe_input is not None:
        produced = subprocess.run(
            [str(arguments.probe_input.resolve())],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=PROBE_INPUT_TIMEOUT_SECONDS,
            check=False,
        )
        if produced.returncode != 0:
            raise RuntimeError(
                f"{arguments.probe_input.name} failed "
                f"({produced.returncode}): "
                f"{produced.stderr.decode(errors='replace').strip()}"
            )
        payloads[GUEST_PROBE_INPUT] = GuestFile(produced.stdout)

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        (f"ncpus={arguments.ncpus}", *arguments.guest_argument),
        init_shell=True,
        engine=arguments.engine,
        payloads=payloads,
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command("mount -t tmpfs -o mode=1777 tmpfs /tmp", 20)
        if compiled:
            guest.check_command(
                " ".join(
                    ["c++", "-std=c++20", "-O2"]
                    + shlex.split(arguments.cxxflags)
                    + ["-o", GUEST_BINARY, guest_probe]
                ),
                arguments.build_timeout,
            )
            command = shlex.join([GUEST_BINARY, *arguments.probe_argument])
        else:
            command = shlex.join(
                ["python3", "-B", guest_probe, *arguments.probe_argument]
            )
        result = guest.run_command(command, arguments.probe_timeout)
        print(result.output.strip())
        complaints = [
            line.strip()
            for line in guest.output().splitlines()
            if pattern and pattern.search(line)
        ]
        for line in complaints:
            print(f"FAIL console: {line}")
        problems = []
        if result.return_code != 0:
            problems.append(
                f"{arguments.probe.name} reported {result.return_code} "
                "failure(s)"
            )
        if complaints:
            problems.append(f"{len(complaints)} console line(s) matched")
        if problems:
            raise RuntimeError("; ".join(problems))
        guest.check_command("echo alive", 20)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
