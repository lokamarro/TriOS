#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host's graphics memory report reaches the guest's reclaimer.

A host GPU with a fixed memory budget tells the guest how near the budget is
and which guest processes hold it; the guest kernel keeps the answer in the
root-only file gpu_memory_report on the DRM device (kernel/common/README.md,
docs/graphics.md). The runner boots a guest with the GPU and the launcher's
--stats line, and runs virtgpu_memory_report_probe.cpp, which reads the file as
Android's lmkd does: it arms the file, waits with poll(), fills the budget with
a process, filters the holders, ends the process, has an allocation refused and
disarms. The probe prints one PASS or FAIL line per check. The runner also
checks that the launcher's --stats line said "guest-reclaim=on" while the file
was armed, then powers the guest off and checks that the kernel said nothing
about it: the driver removes its file with the DRM device, at shutdown as at
removal. macOS only.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import shlex
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


GUEST_DIRECTORY = "/usr/local/libexec/uml-test"
GUEST_BINARY = "/tmp/probe"
BUILD_TIMEOUT_SECONDS = 180
CHECK = re.compile(r"^(PASS|FAIL) (.*)$", re.MULTILINE)
# The launcher's line, with the clause it appends while the guest's reclaimer
# is armed.
RECLAIMING = re.compile(r"uml-darwin: stats .* guest-reclaim=on\s*$", re.MULTILINE)
# The checks the probe makes, so a probe that stopped early is a failure.
MINIMUM_CHECKS = 20
POWEROFF_SECONDS = 60
# What the kernel prints at power-off when the driver tears its file down in
# the wrong order ("kernfs: can not remove 'gpu_memory_report', no directory").
POWEROFF_FAILURE = re.compile(
    r"^.*(?:WARNING:|BUG:|Oops|Call Trace:|kernfs:|Kernel panic|"
    r"Darwin UML panic|Kernel mode fault).*$",
    re.MULTILINE,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--ncpus", required=True, type=int)
    parser.add_argument(
        "--probe-timeout", required=True, type=int, metavar="SECONDS"
    )
    parser.add_argument("--cxxflags", default="")
    parser.add_argument(
        "--guest-argument", action="append", default=[], metavar="ARGUMENT"
    )
    arguments = parser.parse_args()

    guest_probe = f"{GUEST_DIRECTORY}/probe.cpp"
    payloads = {guest_probe: GuestFile(arguments.probe.read_bytes())}
    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        (f"ncpus={arguments.ncpus}", *arguments.guest_argument),
        init_shell=True,
        payloads=payloads,
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command("mount -t tmpfs -o mode=1777 tmpfs /tmp", 20)
        guest.check_command(
            " ".join(
                ["c++", "-std=c++20", "-O2"]
                + shlex.split(arguments.cxxflags)
                + ["-o", GUEST_BINARY, guest_probe]
            ),
            BUILD_TIMEOUT_SECONDS,
        )
        result = guest.run_command(GUEST_BINARY, arguments.probe_timeout)
        print(result.output.strip())
        console = guest.output()
        powered_off = guest.shutdown(POWEROFF_SECONDS)
        poweroff_output = guest.output()[len(console):]

    problems = []
    checks = CHECK.findall(result.output)
    for verdict, what in checks:
        if verdict == "FAIL":
            problems.append(what)
    if len(checks) < MINIMUM_CHECKS:
        problems.append(
            f"the probe made {len(checks)} of at least {MINIMUM_CHECKS} checks"
        )
    if result.return_code != 0 and not problems:
        problems.append(f"the probe reported {result.return_code} failure(s)")
    if RECLAIMING.search(console) is None:
        problems.append(
            "the launcher's --stats line never said guest-reclaim=on"
        )
    if not powered_off:
        problems.append(f"the guest did not power off in {POWEROFF_SECONDS} s")
    for line in POWEROFF_FAILURE.findall(poweroff_output):
        problems.append(f"the kernel said at power-off: {line.strip()}")
    if problems:
        raise RuntimeError("; ".join(problems))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
