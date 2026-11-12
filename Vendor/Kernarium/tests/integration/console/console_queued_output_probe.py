#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fill a serial line until it refuses more, then power the guest off.

Usage: console_queued_output_probe.py DEVICE RECORDS RECORD_BYTES

Writes numbered records to DEVICE without blocking until the line takes
less than a write offers, so the host channel and the line's output ring are
both full, and prints CONSOLE_QUEUED with the bytes the line took and the
bytes it still holds (TIOCOUTQ), which the host has not received. It then
runs poweroff -f with the line still open: closing it would wait for the
line to drain first.
"""

from __future__ import annotations

import fcntl
import os
import struct
import sys
import termios


def record(index: int, size: int) -> bytes:
    body = f"Q{index:06d}:" + "abcdefghijklmnopqrstuvwxyz0123456789" * 4
    return (body[:size - 1] + "\n").encode()


def main() -> int:
    device, records, size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    descriptor = os.open(device, os.O_WRONLY | os.O_NONBLOCK | os.O_NOCTTY)
    taken = 0
    for index in range(records):
        data = record(index, size)
        try:
            written = os.write(descriptor, data)
        except BlockingIOError:
            break
        taken += written
        if written < len(data):
            break
    else:
        print(f"CONSOLE_NOT_FULL bytes={taken}", flush=True)
        return 1
    queued = struct.unpack(
        "i", fcntl.ioctl(descriptor, termios.TIOCOUTQ, b"\0" * 4))[0]
    print(f"CONSOLE_QUEUED bytes={taken} queued={queued}", flush=True)
    os.set_inheritable(descriptor, True)
    os.execvp("poweroff", ["poweroff", "-f"])


if __name__ == "__main__":
    raise SystemExit(main())
