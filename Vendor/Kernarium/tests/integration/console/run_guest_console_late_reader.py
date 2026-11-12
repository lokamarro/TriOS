#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A serial line opens however late the host reads its terminal.

Boots a two-vCPU guest whose harness reads nothing from the console for the
first seconds, as a harness or application that is slow to start reading
does, and then requires the kernel's whole boot log, the console's own
registration line included, and a shell. A line whose open waited for the
host to read what its terminal had queued could have that wait ended by a
signal and fail the open: the kernel then wrote no console text, and PID 1
started without a console and exited, which panicked the kernel. The
contract is in kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402

# How long the harness leaves the console unread after the emulator starts.
# The serial line opens about half a second into the boot, so its output
# channel opens while the emulator's first lines are still unread.
UNREAD_SECONDS = 3
BOOTS = 3
EXPECTED_LINES = (
    "Linux version ",
    "Console initialized on /dev/ttyS0",
    "Run /init as init process",
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    for boot in range(1, BOOTS + 1):
        with Guest(
            arguments.emulator.resolve(),
            arguments.rootfs.resolve(),
            "1G",
            "tmpfs",
            ("ncpus=2",),
            init_shell=True,
            engine=arguments.engine,
        ) as guest:
            # The stimulus: the console stays unread meanwhile.
            time.sleep(UNREAD_SECONDS)
            guest.boot_to_shell(180)
            transcript = guest.output()
            missing = [line for line in EXPECTED_LINES if line not in transcript]
            if missing:
                raise RuntimeError(
                    f"boot {boot}: the console lost kernel output its host "
                    f"read late: no {', no '.join(map(repr, missing))}"
                )
            if not guest.shutdown(60):
                raise RuntimeError(
                    f"boot {boot}: the guest did not power off within 60 s"
                )
        print(f"boot {boot}: the console kept the whole boot log after "
              f"{UNREAD_SECONDS} s unread")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
