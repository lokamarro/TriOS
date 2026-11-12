#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Deliver a serial line's queued output when the guest powers off.

A write to a UML line returns once the line has queued it: what the host
channel does not take waits in the line's 4 KiB ring for the channel to take
more. The runner serves ttyS1 on a host pipe it leaves unread. The guest
writes numbered records to ttyS1 until the line refuses more, so the pipe and
the ring are both full, and powers off at once with the line still open
(console_queued_output_probe.py). The runner starts reading the pipe
SETTLE_SECONDS after the emulator reports the power-off, a moment the
shutdown has passed the exitcalls by on an idle host, and reads to its end.
The pipe must deliver every byte the line took, with each newline turned into
a carriage return and a newline, the ring must have held output when the
guest powered off, and the emulator must exit with status 0.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import sys
import threading
import time

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import KERNEL_LINE, Guest, GuestFile  # noqa: E402


VCPUS = 2
RECORDS = 8192
RECORD = 64
PROBE = "/opt/uml-test/console_queued_output_probe.py"
POWER_OFF = "guest powered off"
# How long after the power-off report the runner leaves the pipe unread.
SETTLE_SECONDS = 0.5
# How long the guest has to fill the line and power off, and the pipe to
# reach its end once the emulator has exited.
PROBE_SECONDS = 120
DELIVERY_SECONDS = 30
QUEUED = re.compile(r"^CONSOLE_QUEUED bytes=(\d+) queued=(\d+)$", re.MULTILINE)


def record(index: int) -> bytes:
    body = f"Q{index:06d}:" + "abcdefghijklmnopqrstuvwxyz0123456789" * 4
    return (body[:RECORD - 1] + "\n").encode()


class PipeReader(threading.Thread):
    """Read the pipe once `start_reading` is set, until its end."""

    def __init__(self, descriptor: int) -> None:
        super().__init__(daemon=True)
        self._descriptor = descriptor
        self.start_reading = threading.Event()
        self.stop = threading.Event()
        self.ended = threading.Event()
        self.data = bytearray()

    def run(self) -> None:
        os.set_blocking(self._descriptor, False)
        while not self.stop.is_set():
            if not self.start_reading.wait(0.05):
                continue
            try:
                chunk = os.read(self._descriptor, 65536)
            except BlockingIOError:
                time.sleep(0.01)
                continue
            if not chunk:
                self.ended.set()
                return
            self.data += chunk


def check(emulator: pathlib.Path, rootfs: pathlib.Path, engine: str) -> None:
    probe = (
        REPOSITORY / "tests/integration/console/console_queued_output_probe.py"
    ).read_bytes()
    line_read, line_write = os.pipe()
    reader = PipeReader(line_read)
    reader.start()
    try:
        try:
            guest = Guest(
                emulator,
                rootfs,
                "512M",
                "tmpfs",
                (f"ncpus={VCPUS}", f"ssl1=null,fd:{line_write}"),
                init_shell=True,
                engine=engine,
                pass_fds=(line_write,),
                payloads={PROBE: GuestFile(probe, 0o755)},
                separate_host_output=True,
            )
        finally:
            os.close(line_write)
        with guest:
            guest.boot_to_shell(180)
            guest._write(f"python3 {PROBE} /dev/ttyS1 {RECORDS} {RECORD}\n")
            deadline = time.monotonic() + PROBE_SECONDS
            while POWER_OFF not in guest.host_output():
                if guest.wait_for_exit(0.05):
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError(
                        f"the guest did not power off in {PROBE_SECONDS} s:\n"
                        + guest.output()[-4000:]
                    )
            time.sleep(SETTLE_SECONDS)
            reader.start_reading.set()
            if not guest.wait_for_exit(PROBE_SECONDS):
                raise RuntimeError("the emulator did not exit")
            if not reader.ended.wait(DELIVERY_SECONDS):
                raise RuntimeError("the ttyS1 pipe did not reach its end")
            output = re.sub(KERNEL_LINE.decode(), "", guest.output() + "\n")
            if POWER_OFF not in guest.host_output():
                raise RuntimeError(
                    "the emulator did not report the power-off:\n"
                    + guest.host_output()[-4000:]
                )
            if guest.return_code != 0:
                raise RuntimeError(
                    f"the emulator exited with status {guest.return_code}"
                )
        queued = QUEUED.search(output)
        if queued is None:
            raise RuntimeError(
                "the probe did not fill the line:\n" + output[-4000:]
            )
        taken, held = int(queued[1]), int(queued[2])
        if held == 0:
            raise RuntimeError(
                "the line held nothing back when the guest powered off"
            )
        stream = b"".join(record(index) for index in range(RECORDS))
        expected = stream[:taken].replace(b"\n", b"\r\n")
        got = bytes(reader.data)
        if got != expected:
            same = next(
                (i for i, (a, b) in enumerate(zip(got, expected)) if a != b),
                min(len(got), len(expected)),
            )
            raise RuntimeError(
                f"ttyS1 delivered {len(got)} of {len(expected)} bytes, first "
                f"difference at byte {same}; the line held {held} bytes when "
                "the guest powered off"
            )
        print(
            f"ttyS1 delivered all {len(expected)} bytes, {held} of them "
            "queued in the line when the guest powered off"
        )
    finally:
        reader.stop.set()
        reader.start_reading.set()
        reader.join(5)
        os.close(line_read)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()
    check(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.engine,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
