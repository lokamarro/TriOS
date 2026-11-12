#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep serial console input alive after an input burst throttles the tty.

The runner sends ttyS0 a burst larger than the line discipline's 4 KiB input
buffer as a single host write, followed by a marker command, and then requires
a separately typed command to run. The burst outruns the shell, so n_tty
throttles the line once 3968 bytes sit unread. A kernel that drops the line's
input interrupt at that throttle still executes the burst, which it had
already read, but never sees the typed command.

One guest CPU makes the throttle deterministic: the kernel is not preemptible,
so the line discipline fills its buffer before the shell can read any of it.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


N_TTY_BUFFER_BYTES = 4096
BURST_LINES = 140
ROUNDS = 2
BURST_TIMEOUT_SECONDS = 60
TYPED_TIMEOUT_SECONDS = 20


def burst(lines: int) -> str:
    return "".join(f": {'x' * 70} {index:04d}\n" for index in range(lines))


def serial_interrupts(guest: Guest) -> str:
    result = guest.run_command("grep -w ssl /proc/interrupts", 20)
    return " ".join(result.output.split())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    filler = burst(BURST_LINES)
    if len(filler) <= 2 * N_TTY_BUFFER_BYTES:
        raise SystemExit("the burst must exceed the line discipline buffer")
    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "512M",
        "tmpfs",
        ("ncpus=1",),
        init_shell=True,
    ) as guest:
        guest.boot_to_shell(180)
        for round_index in range(ROUNDS):
            result = guest.run_command(
                "echo BURST-$((6*7))",
                BURST_TIMEOUT_SECONDS,
                preamble=filler,
                single_write=True,
            )
            if result.return_code != 0 or "BURST-42" not in result.output:
                raise RuntimeError(
                    f"round {round_index}: the burst's marker did not run:\n"
                    + result.output
                )
            try:
                typed = guest.run_command(
                    "echo TYPED-$((6*7))", TYPED_TIMEOUT_SECONDS
                )
            except TimeoutError as error:
                raise RuntimeError(
                    f"round {round_index}: console input stopped after a "
                    f"{len(filler)}-byte burst; the serial line lost its input "
                    "interrupt when the tty throttled"
                ) from error
            if typed.return_code != 0 or "TYPED-42" not in typed.output:
                raise RuntimeError(
                    f"round {round_index}: typed command failed:\n" + typed.output
                )
            print(
                f"round {round_index}: {len(filler)}-byte burst, typed input "
                f"still delivered ({serial_interrupts(guest)})"
            )
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
