#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write to the Virtio console and a UML serial line with allocations traced.

A console write runs with a spinlock held and interrupts off, so it must not
allocate: hvc throws away output its put_chars() failed to take for want of
memory, and a UML line that could not allocate its output ring reported
room it lacked, so the line discipline counted characters that never
reached the host as written. The Virtio console sends from a buffer it
allocated with the port, and a line allocates its ring when it opens.

The runner serves hvc0 on a --virtio-console-socket it reads as output
arrives, and acknowledges every WINDOW bytes it has read: the host drops what
the Virtio console sends while its reader is behind, so the guest sends one
window and waits for the acknowledgement, and no host load changes what
arrives. It serves ttyS1 on a host pipe it leaves unread until the guest reports
that the line refused a write (EAGAIN: the pipe is full and so is the
line's ring), so the line's writer must wait. Nothing releases the pipe on a
timer: the guest says when the line has held its writer back, however late
a loaded host brings it there, and only then does the runner read. The guest
traces slab and non-sleeping page allocations with their stacks while it
writes the same numbered records to both (console_write_memory_probe.py).
The trace must lose nothing and show no allocation under put_chars() or
line_write(), the ttyS1 writer must have been refused room before the host
read anything, both channels must deliver every byte in order, ttyS1's with
each newline turned into a carriage return and a newline, and the guest must
print no kernel warning and power off cleanly.

--writer-delay SECONDS makes the probe start its ttyS1 writes that much late,
as a loaded host would: the result must not change.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import socket
import sys
import tempfile
import threading
import time

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


VCPUS = 2
RECORDS = 4096
RECORD = 64
# The guest never has more than this much hvc0 output outstanding. The host
# drops output past what the socket holds, 8 KiB on Darwin, and a window
# well inside that is never dropped however late the runner reads.
HVC_WINDOW = 2048
ACKNOWLEDGE = b"\x06"
DELIVERY_SECONDS = 30
PROBE = "/opt/uml-test/console_write_memory_probe.py"
# The probe runs in the background, so that the runner can read the host's
# channels when the probe says so; it leaves its output and exit status here.
PROBE_OUTPUT = "/tmp/console-write-memory.out"
PROBE_STATUS = "/tmp/console-write-memory.status"
# How long the probe may take to trace, write 256 KiB to hvc0 and have the
# line refuse a write, and then to finish once the host reads.
PROBE_SECONDS = 120
FAILURE = re.compile(
    r"WARNING:|BUG:|Oops|Call Trace:|page allocation failure|"
    r"atomic allocation failed|Kernel panic|Darwin UML panic|Darwin POSIX|"
    r"Kernel mode fault"
)
RESULT = re.compile(r"^CONSOLE_(WRITES|ALLOCATIONS) (.*)$", re.MULTILINE)
LINE_HELD = re.compile(r"^CONSOLE_LINE_HELD bytes=(\d+)$", re.MULTILINE)


def record(index: int) -> bytes:
    body = f"R{index:06d}:" + "abcdefghijklmnopqrstuvwxyz0123456789" * 2
    return (body[:RECORD - 1] + "\n").encode()


class Drain(threading.Thread):
    """Collect one host channel's output until told to stop."""

    def __init__(self, receive, after=None) -> None:
        super().__init__(daemon=True)
        self._receive = receive
        self._after = after
        self.opened = threading.Event()
        self.stop = threading.Event()
        self.data = bytearray()
        self.failure: BaseException | None = None

    def run(self) -> None:
        try:
            while not self.stop.is_set():
                if not self.opened.wait(0.05):
                    continue
                chunk = self._receive()
                if chunk is None:
                    continue
                if not chunk:
                    return
                self.data += chunk
                if self._after is not None:
                    self._after(len(self.data))
        except BaseException as error:  # reported by the main thread
            self.failure = error


class SocketChannel:
    """The emulator's console socket, connected once it listens."""

    def __init__(self, path: str) -> None:
        self._path = path
        self._connection: socket.socket | None = None
        self._acknowledged = 0

    def receive(self) -> bytes | None:
        if self._connection is None:
            candidate = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                candidate.connect(self._path)
            except OSError:
                candidate.close()
                time.sleep(0.1)
                return None
            candidate.settimeout(0.2)
            self._connection = candidate
        try:
            return self._connection.recv(65536)
        except socket.timeout:
            return None

    def acknowledge(self, received: int) -> None:
        """Send one byte for each full window read so far."""
        while (
            self._connection is not None
            and (self._acknowledged + 1) * HVC_WINDOW <= received
        ):
            self._connection.send(ACKNOWLEDGE)
            self._acknowledged += 1


def pipe_receiver(descriptor: int):
    os.set_blocking(descriptor, False)

    def receive() -> bytes | None:
        try:
            return os.read(descriptor, 65536)
        except BlockingIOError:
            time.sleep(0.01)
            return None

    return receive


def parse(output: str) -> dict[str, dict[str, float]]:
    results: dict[str, dict[str, float]] = {}
    for match in RESULT.finditer(output):
        results[match[1]] = {
            key: float(value)
            for key, value in (field.split("=") for field in match[2].split())
        }
    return results


def await_bytes(drain: Drain, size: int) -> None:
    deadline = time.monotonic() + DELIVERY_SECONDS
    while len(drain.data) < size and time.monotonic() < deadline:
        if drain.failure is not None:
            raise RuntimeError(f"reading a console failed: {drain.failure}")
        time.sleep(0.1)


def probe_output(guest: Guest) -> str:
    return guest.check_command(f"cat {PROBE_OUTPUT}", 30)


def probe_ended(guest: Guest) -> bool:
    return guest.run_command(f"test -e {PROBE_STATUS}", 30).return_code == 0


def await_probe(guest: Guest, done) -> str:
    """The probe's output once `done(output)`, or, with no `done`, once the
    probe has ended. Nothing but the probe's own progress is waited for,
    bounded by PROBE_SECONDS, and a probe that ends before `done` is a
    failure at once."""
    deadline = time.monotonic() + PROBE_SECONDS
    while True:
        output = probe_output(guest)
        if done is not None and done(output):
            return output
        if probe_ended(guest):
            output = probe_output(guest)
            if done is None or done(output):
                return output
            raise RuntimeError("the probe ended first:\n" + output[-4000:])
        if time.monotonic() > deadline:
            raise RuntimeError(
                f"the probe did not get there in {PROBE_SECONDS} s:\n"
                + output[-4000:]
            )
        guest.wait_for_exit(0.2)


def check(
    emulator: pathlib.Path, rootfs: pathlib.Path, engine: str, delay: float
) -> None:
    probe = (
        REPOSITORY / "tests/integration/console/console_write_memory_probe.py"
    ).read_bytes()
    expected = b"".join(record(index) for index in range(RECORDS))
    with tempfile.TemporaryDirectory(prefix="uml-console-memory-") as private:
        console = os.path.join(private, "console.sock")
        channel = SocketChannel(console)
        hvc = Drain(channel.receive, channel.acknowledge)
        hvc.opened.set()
        line_read, line_write = os.pipe()
        line = Drain(pipe_receiver(line_read))
        hvc.start()
        line.start()
        try:
            try:
                guest = Guest(
                    emulator,
                    rootfs,
                    "512M",
                    "tmpfs",
                    (
                        f"ncpus={VCPUS}",
                        f"--virtio-console-socket={console}",
                        f"ssl1=null,fd:{line_write}",
                    ),
                    init_shell=True,
                    engine=engine,
                    pass_fds=(line_write,),
                    payloads={PROBE: GuestFile(probe, 0o755)},
                )
            finally:
                os.close(line_write)
            with guest:
                guest.boot_to_shell(180)
                mark = len(guest.output())
                guest.check_command(
                    f"rm -f {PROBE_STATUS}; "
                    f"(python3 {PROBE} {RECORDS} {HVC_WINDOW} {delay:g} "
                    f"> {PROBE_OUTPUT} 2>&1; echo $? > {PROBE_STATUS}) "
                    "& echo started",
                    30,
                )
                # The host reads ttyS1 only once the guest says the line
                # refused it room.
                held_output = await_probe(
                    guest, lambda text: LINE_HELD.search(text) is not None
                )
                if "CONSOLE_WRITES" in held_output:
                    raise RuntimeError(
                        "the ttyS1 writer finished before the host read "
                        "anything:\n" + held_output[-4000:]
                    )
                held = int(LINE_HELD.search(held_output)[1])
                line.opened.set()
                output = await_probe(guest, None)
                if guest.check_command(f"cat {PROBE_STATUS}", 30).strip() != "0":
                    raise RuntimeError("the probe failed:\n" + output[-4000:])
                results = parse(output)
                for name in ("WRITES", "ALLOCATIONS"):
                    if name not in results:
                        raise RuntimeError(
                            f"the probe printed no CONSOLE_{name} line:\n"
                            + output[-4000:]
                        )
                writes, allocations = results["WRITES"], results["ALLOCATIONS"]
                for channel in ("hvc", "line"):
                    if writes[channel] != len(expected):
                        raise RuntimeError(
                            f"{channel} took {int(writes[channel])} of "
                            f"{len(expected)} bytes:\n" + output[-4000:]
                        )
                if allocations["lost"] != 0:
                    raise RuntimeError(
                        f"the trace lost {int(allocations['lost'])} events:\n"
                        + output[-4000:]
                    )
                if allocations["hvc"] or allocations["line"]:
                    raise RuntimeError(
                        f"console writes allocated memory: put_chars() "
                        f"{int(allocations['hvc'])} times, line_write() "
                        f"{int(allocations['line'])} times:\n" + output[-4000:]
                    )
                if not 0 < held < len(expected):
                    raise RuntimeError(
                        f"the line refused room after {held} of "
                        f"{len(expected)} bytes:\n" + output[-4000:]
                    )
                if int(writes["held"]) != held:
                    raise RuntimeError(
                        f"the probe reported the line held after {held} bytes "
                        f"and then {int(writes['held'])}:\n" + output[-4000:]
                    )
                await_bytes(hvc, len(expected))
                line_expected = expected.replace(b"\n", b"\r\n")
                await_bytes(line, len(line_expected))
                for channel, drain, want in (
                    ("hvc0", hvc, expected),
                    ("ttyS1", line, line_expected),
                ):
                    got = bytes(drain.data)
                    if got != want:
                        same = next(
                            (i for i, (a, b) in enumerate(zip(got, want)) if a != b),
                            min(len(got), len(want)),
                        )
                        raise RuntimeError(
                            f"{channel} delivered {len(got)} of {len(want)} "
                            f"bytes, first difference at byte {same}"
                        )
                tail = guest.output()[mark:]
                failure = FAILURE.search(tail)
                if failure is not None:
                    raise RuntimeError(
                        f"the guest printed {failure[0]!r}:\n"
                        + tail[max(0, failure.start() - 2000):][:8000]
                    )
                if not guest.shutdown(60) or guest.return_code != 0:
                    raise RuntimeError("the guest did not power off cleanly")
                print(
                    f"hvc0 and ttyS1 delivered {len(expected)} bytes each; "
                    f"the line refused the ttyS1 writer after {held} bytes "
                    f"until the host read, {writes['line_seconds']:.1f} s; "
                    f"{int(allocations['events'])} allocations traced, none "
                    "by a console write"
                )
        finally:
            hvc.stop.set()
            line.stop.set()
            hvc.join(5)
            line.join(5)
            os.close(line_read)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument(
        "--writer-delay",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help="start the probe's ttyS1 writes this late, as a loaded host would",
    )
    arguments = parser.parse_args()
    check(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.engine,
        arguments.writer_delay,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
