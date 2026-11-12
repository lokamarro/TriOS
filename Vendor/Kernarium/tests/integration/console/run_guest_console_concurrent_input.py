#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep serial console input whole while two CPUs drain it.

A reader on guest CPU 1 takes a numbered 1 MiB stream from ttyS0 while the
host keeps the input pipe full. The reader is slower than the stream, so n_tty
throttles the line again and again. Each unthrottle drains the host channel on
the reader's CPU while new host writes raise the line's input interrupt on
another CPU. If the kernel lets both drains run at once, they split and reorder
the stream, lose parts of it, or leave the line without input. The reader
checks every line and gives up after 20 idle seconds, so a lost line is
reported instead of blocking.
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

from guest import Guest, GuestFile  # noqa: E402


LINES = 16384
PAD = 54
ROUNDS = 2
CPUS = 4
ROUND_TIMEOUT_SECONDS = 120

# The reader is a payload file; only the stream it checks crosses the console.
GUEST_READER = "/usr/local/libexec/uml-test/reader.py"
READER = f"""
import os, select, sys
n = int(sys.argv[1]); got = 0; bad = 0; first = None; buf = b""
while got < n:
    ready, _, _ = select.select([0], [], [], 20)
    if not ready:
        break
    chunk = os.read(0, 65536)
    if not chunk:
        break
    buf += chunk
    while b"\\n" in buf and got < n:
        line, buf = buf.split(b"\\n", 1)
        want = ("%08d:" % got + "".join(
            chr(97 + (got + k) % 26) for k in range({PAD}))).encode()
        if line != want:
            bad += 1
            if first is None:
                first = (got, line[:40])
        got += 1
print("stream lines=%d got=%d bad=%d first=%r" % (n, got, bad, first))
"""


def stream_line(index: int) -> str:
    return (f"{index:08d}:"
            + "".join(chr(97 + (index + k) % 26) for k in range(PAD)) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    payload = "".join(stream_line(index) for index in range(LINES))
    command = f"taskset -c 1 python3 -B {GUEST_READER} {LINES}"
    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        (f"ncpus={CPUS}",),
        init_shell=True,
        payloads={GUEST_READER: GuestFile(READER.encode())},
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command("stty -echo", 20)
        for round_index in range(ROUNDS):
            try:
                result = guest.run_command(
                    command, ROUND_TIMEOUT_SECONDS, stdin=payload
                )
            except TimeoutError as error:
                raise RuntimeError(
                    f"round {round_index}: console input stopped while two "
                    "CPUs drained the serial line"
                ) from error
            found = re.search(
                r"stream lines=(\d+) got=(\d+) bad=(\d+) first=(.*)",
                result.output,
            )
            if result.return_code != 0 or found is None:
                raise RuntimeError(
                    f"round {round_index}: the reader failed:\n{result.output}"
                )
            print(f"round {round_index}: {len(payload)} bytes: {found[0]}")
            if int(found[2]) != LINES or int(found[3]) != 0:
                raise RuntimeError(
                    f"round {round_index}: the console lost or damaged input "
                    "while two CPUs drained the serial line"
                )
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
