#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A Virtio-GPU context names the guest process that created it.

The guest kernel appends the creating process to the debug name of every
context, "<name>|pid=<tgid>|comm=<comm>" (kernel/common/README.md), and the
host credits a context's graphics memory to that process
(docs/graphics.md). The runner boots a guest with the GPU and the launcher's
--stats line, runs virtgpu_context_owner_probe.cpp, which makes three processes
that each hold graphics memory in a context of their own (Virgl, Venus, and a
Virgl process whose command name imitates the suffix), and checks that the
holders the host printed while they held it are those processes: the process
ID and command name the probe reported, at least the memory it reported, and
no process the imitation names. macOS only.
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
# "OWNER <kind> <pid> <comm> <bytes>", printed by the probe.
OWNER = re.compile(r"^OWNER (\w+) (\d+) (\S+) (\d+)\s*$", re.MULTILINE)
# "<pid>:<comm>:<MiB> MiB:<idle> s", the launcher's --stats holders.
HOLDER = re.compile(r"(?<![\w.])(\d+):(\S+?):(\d+) MiB:(\d+) s")
# The kernel replaces a '|' in the command name.
KERNEL_REPLACES = str.maketrans("|", "?")


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

    problems = []
    if result.return_code != 0:
        problems.append(f"the probe reported {result.return_code} failure(s)")
    owners = {
        kind: (int(pid), comm, int(size))
        for kind, pid, comm, size in OWNER.findall(result.output)
    }
    for kind in ("gl", "vk", "spoof"):
        if kind not in owners:
            problems.append(f"the {kind} process did not report")
    holders = [
        (int(pid), comm, int(mebibytes))
        for pid, comm, mebibytes, _ in HOLDER.findall(console)
    ]
    for kind, (pid, comm, size) in owners.items():
        expected = comm.translate(KERNEL_REPLACES)
        # The host rounds down to MiB; the memory it credits is at least what
        # the process asked for.
        wanted = size >> 20
        best = max(
            (m for p, c, m in holders if p == pid and c == expected),
            default=None,
        )
        if best is None:
            problems.append(
                f"no holder {pid}:{expected} for the {kind} process"
            )
        elif best < wanted:
            problems.append(
                f"{kind} {pid}:{expected} was credited {best} MiB of "
                f"the {wanted} MiB it holds"
            )
        else:
            print(f"PASS {kind} {pid}:{expected} credited {best} MiB")
    for pid, comm, _ in holders:
        if pid == 1 or (comm == "z" and pid != 0):
            problems.append(
                f"the imitation moved memory to process {pid}:{comm}"
            )
    if problems:
        raise RuntimeError("; ".join(problems))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
