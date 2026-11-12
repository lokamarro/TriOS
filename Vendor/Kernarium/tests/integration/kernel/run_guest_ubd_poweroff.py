#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Power a guest off while its tasks keep reading the root disk.

`poweroff -f` reaches the kernel's power-off with every task still running,
and UML runs its exitcalls, which tear its drivers down, while the other
vCPUs keep scheduling those tasks. Readers on every vCPU keep direct reads of
/dev/ubda in flight, so the UBD driver is still asked for I/O while its
exitcall runs. The exitcall must stop the disks' queues before it destroys
the workqueue their requests run on: a request queued while the workqueue
drains is dropped with a warning ("cannot queue darwin_ubd_work"), and one
queued after it is gone faults on a NULL workqueue and panics the guest.

Each round boots a fresh guest, checks that the readers are moving, and
powers it off. The kernel must reach "reboot: Power down", the emulator must
exit with status 0, and nothing after the power-off request may be a kernel
warning, oops, panic or host fault report. The contract is the UBD paragraph
of kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


VCPUS = 4
READERS = 2 * VCPUS
POWEROFF_SECONDS = 60
# Each reader reads its own 64 MiB stretch of the image in 4 KiB direct
# reads, so every read is a request and none is served from the page cache.
READER = (
    "while :; do dd if=/dev/ubda of=/dev/null bs=4k count=16384 "
    "skip=$((16384 * {index})) iflag=direct 2>/dev/null; done"
)
FAILURE = re.compile(
    r"WARNING:|BUG:|Oops|Call Trace:|cannot queue|Kernel panic|"
    r"Darwin UML panic|Darwin POSIX|Kernel mode fault"
)


def reads_completed(guest: Guest) -> int:
    """The completed-read count of /dev/ubda's statistics."""
    return int(
        guest.check_command("set -- $(cat /sys/block/ubda/stat); echo $1", 30)
        .strip()
        .splitlines()[-1]
    )


def power_off_while_reading(
    emulator: pathlib.Path, rootfs: pathlib.Path, engine: str, round: int
) -> None:
    with Guest(
        emulator,
        rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}",),
        init_shell=True,
        engine=engine,
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command(
            " ".join(
                f"( {READER.format(index=index)} ) >/dev/null 2>&1 &"
                for index in range(READERS)
            )
            + " true",
            30,
        )
        before = reads_completed(guest)
        guest.check_command("sleep 2", 30)
        after = reads_completed(guest)
        if after <= before:
            raise RuntimeError(
                f"round {round}: the readers completed no reads "
                f"({before} before, {after} after)"
            )
        request = len(guest.output())
        if not guest.shutdown(POWEROFF_SECONDS):
            raise RuntimeError(
                f"round {round}: the guest did not power off within "
                f"{POWEROFF_SECONDS} s"
            )
        tail = guest.output()[request:]
        failure = FAILURE.search(tail)
        if failure is not None:
            raise RuntimeError(
                f"round {round}: the power-off printed {failure[0]!r}:\n"
                + tail[max(0, failure.start() - 2000):][:8000]
            )
        if "reboot: Power down" not in tail:
            raise RuntimeError(
                f"round {round}: the kernel never reached its power-off:\n"
                + tail[-4000:]
            )
        if guest.return_code != 0:
            raise RuntimeError(
                f"round {round}: the emulator exited with status "
                f"{guest.return_code}:\n" + tail[-4000:]
            )
        print(
            f"round {round}: {after - before} reads in 2 s on {READERS} "
            "readers; powered off cleanly"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--rounds", type=int, default=2)
    arguments = parser.parse_args()

    for round in range(1, arguments.rounds + 1):
        power_off_while_reading(
            arguments.emulator.resolve(),
            arguments.rootfs.resolve(),
            arguments.engine,
            round,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
