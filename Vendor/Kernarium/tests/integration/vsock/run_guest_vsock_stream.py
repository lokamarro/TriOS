#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the Virtio vsock device carries byte streams to and from a guest.

The launcher forwards two loopback TCP ports to two guest vsock ports. A
Python server in the guest listens on the first and nothing listens on the
second. The host runs the shapes an application produces through the first
and checks every body by SHA-256 on both ends:

* 64 MiB each way, with the throughput of each;
* four concurrent streams, two each way;
* a host reader that stops for a while and then resumes, and a guest reader
  that does the same: the sender must be held back to the receiver rather than
  buffer the body, a second stream must stay responsive meanwhile, and the
  body must arrive intact once the reader resumes;
* a half-close in each direction, after which the other end still talks;
* a connection to a port nothing listens on, which must end at once, and
  many more connections than the device holds, one after another, which shows
  that no connection leaks;
* a guest suspend in the middle of a stream each way, which the connections
  must survive; and
* a guest restart, which every open stream must read as its end, and after
  which a new boot serves again.

The contract is the device's README, darwin/device/vsock/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import socket
import sys
import tempfile
import threading
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402
# The runner of the suspend contract owns the management console.
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "kernel"))
from run_guest_suspend_holds import (  # noqa: E402
    ManagementConsole,
    drain,
    hold,
    require_clean,
)


VCPUS = 2
BOOT_SECONDS = 180
SERVER_PORT = 5000
REFUSED_PORT = 5001
PIECE = 64 * 1024

BULK_BYTES = 64 << 20
PARALLEL_BYTES = 16 << 20
STALL_BYTES = 32 << 20
STALL_AFTER = 512 << 10
STALL_SECONDS = 6.0
HALF_CLOSE_BYTES = 5 << 20
HALF_CLOSE_REPLY_BYTES = 1 << 20
STRADDLE_BYTES = 32 << 20
# A paced stream of STRADDLE_BYTES lasts at least 512 pieces of this, so the
# suspend arrives in the middle of it.
STRADDLE_PACE_SECONDS = 0.010
CONNECTIONS_ONE_BY_ONE = 40

# Bounds. Healthy results are far below them; a stall costs a timeout.
STREAM_TIMEOUT_SECONDS = 120
BULK_LIMIT_SECONDS = 90
PARALLEL_LIMIT_SECONDS = 120
REFUSED_LIMIT_SECONDS = 5
PROBE_LIMIT_SECONDS = 1.0
EOF_LIMIT_SECONDS = 10
RESTART_EOF_SECONDS = 30
ECHO_SECONDS = 30
# Bytes a sender may have handed on while its receiver reads nothing: the
# guest's credit, the device's held data and the sockets' buffers.
BACKPRESSURE_BYTES = 4 << 20
IDLE_SECONDS = 2.0
IDLE_BYTES = 128 << 10

SERVER = "/opt/uml-test/vsock_server.py"
SERVER_LOG = "/run/vsock-server.log"
FORWARD = re.compile(r"vsock forward (\S+):(\d+) -> guest port (\d+)")

GUEST_SERVER = r'''
import hashlib, os, socket, sys, threading, time, traceback

PORT = int(sys.argv[1])
CHUNK = 256 * 1024
PIECE = 64 * 1024
results, progress = {}, {}


def header(conn):
    data = b""
    while not data.endswith(b"\n"):
        byte = conn.recv(1)
        if not byte:
            return None
        data += byte
        if len(data) > 128:
            raise ValueError("header too long")
    return data.decode().split()


def receive(conn, length, stall_after=None, stall_seconds=0.0):
    digest, count = hashlib.sha256(), 0
    view = memoryview(bytearray(CHUNK))
    while length is None or count < length:
        want = CHUNK if length is None else min(CHUNK, length - count)
        if stall_after is not None:
            want = min(want, stall_after - count)
        got = conn.recv_into(view, want)
        if not got:
            break
        digest.update(view[:got])
        count += got
        if stall_after is not None and count == stall_after:
            time.sleep(stall_seconds)
            stall_after = None
    return digest.hexdigest(), count


def send(conn, name, length, pace):
    block = os.urandom(PIECE)
    digest, count, index = hashlib.sha256(), 0, 0
    while count < length:
        piece = (index.to_bytes(8, "little") + block)[:min(PIECE, length - count)]
        conn.sendall(piece)
        digest.update(piece)
        count += len(piece)
        index += 1
        progress[name] = count
        if pace:
            time.sleep(pace)
    return digest.hexdigest()


def finish(conn):
    conn.shutdown(socket.SHUT_WR)
    while conn.recv(4096):
        pass


def serve(conn):
    try:
        with conn:
            fields = header(conn)
            if fields is None:
                return
            op, name, args = fields[0], fields[1], fields[2:]
            if op == "ping":
                conn.sendall(b"pong\n")
            elif op == "echo":
                while True:
                    data = conn.recv(PIECE)
                    if not data:
                        break
                    conn.sendall(data)
            elif op == "sink":
                stall = (int(args[1]), float(args[2])) if len(args) > 1 else (None, 0)
                digest, count = receive(conn, int(args[0]), *stall)
                conn.sendall(("%s %d\n" % (digest, count)).encode())
            elif op == "source":
                digest = send(conn, name, int(args[0]), int(args[1]) / 1000.0)
                conn.sendall((digest + "\n").encode())
                finish(conn)
            elif op == "halfhost":
                digest, count = receive(conn, None)
                conn.sendall(("%s %d\n" % (digest, count)).encode())
            elif op == "halfguest":
                digest = send(conn, name, int(args[0]), 0)
                conn.sendall((digest + "\n").encode())
                conn.shutdown(socket.SHUT_WR)
                results[name] = "%s %d" % receive(conn, None)
            elif op == "report":
                deadline = time.monotonic() + 60
                while name not in results and time.monotonic() < deadline:
                    time.sleep(0.05)
                conn.sendall((results.get(name, "none") + "\n").encode())
            elif op == "progress":
                conn.sendall(("%d\n" % progress.get(name, 0)).encode())
            else:
                raise ValueError("unknown request " + op)
    except Exception:
        traceback.print_exc()
        sys.stderr.flush()


listener = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
listener.bind((socket.VMADDR_CID_ANY, PORT))
listener.listen(16)
while True:
    connection, _ = listener.accept()
    connection.settimeout(120)
    threading.Thread(target=serve, args=(connection,), daemon=True).start()
'''


class Counter:
    """Bytes a transfer has moved; one thread writes, others read."""

    def __init__(self) -> None:
        self.value = 0

    def add(self, count: int) -> None:
        self.value += count


class Background:
    """A transfer on its own thread while the console stays drained."""

    def __init__(self, target, *arguments, **keywords) -> None:
        self._result = None
        self._error: BaseException | None = None

        def run() -> None:
            try:
                self._result = target(*arguments, **keywords)
            except BaseException as error:  # reported by join, never lost
                self._error = error

        self._thread = threading.Thread(target=run, daemon=True)
        self._thread.start()

    def done(self) -> bool:
        return not self._thread.is_alive()

    def join(self, guest: Guest, seconds: float, what: str):
        deadline = time.monotonic() + seconds
        while self._thread.is_alive():
            if time.monotonic() > deadline:
                raise RuntimeError(f"{what}: not finished in {seconds:g} s")
            drain(guest, 0.1)
        if self._error is not None:
            raise RuntimeError(f"{what}: {self._error!r}") from self._error
        return self._result


def open_stream(
    port: int, request: str, *, receive_buffer: int | None = None,
    send_buffer: int | None = None,
) -> socket.socket:
    """A connection through the forward, with the guest server's request sent."""
    stream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        # A stalled reader's tests use small buffers, which keep the host's
        # own sockets from absorbing the body its stall should hold back.
        if receive_buffer is not None:
            stream.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        if send_buffer is not None:
            stream.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, send_buffer)
        stream.settimeout(STREAM_TIMEOUT_SECONDS)
        stream.connect(("127.0.0.1", port))
        stream.sendall(request.encode() + b"\n")
    except BaseException:
        stream.close()
        raise
    return stream


def read_line(stream: socket.socket) -> str:
    data = bytearray()
    while not data.endswith(b"\n"):
        byte = stream.recv(1)
        if not byte:
            raise EOFError(f"the stream ended after {bytes(data)!r}")
        data += byte
    return data.decode().rstrip("\n")


def read_body(
    stream: socket.socket, length: int, digest, counter: Counter | None = None,
    stall: tuple[int, float] | None = None, stalled: dict | None = None,
) -> None:
    """Reads exactly ``length`` bytes into ``digest``. A ``stall`` of
    (bytes, seconds) stops reading for that long once that many have come."""
    view = memoryview(bytearray(256 * 1024))
    count = 0
    while count < length:
        limit = min(len(view), length - count)
        if stall is not None:
            limit = min(limit, stall[0] - count)
        if limit == 0:
            if stalled is not None:
                stalled["start"] = time.monotonic()
            time.sleep(stall[1])
            if stalled is not None:
                stalled["end"] = time.monotonic()
            stall = None
            continue
        got = stream.recv_into(view, limit)
        if not got:
            raise EOFError(f"the stream ended after {count} of {length} bytes")
        digest.update(view[:got])
        count += got
        if counter is not None:
            counter.add(got)


def pieces(length: int):
    """A body of ``length`` bytes in pieces of up to 64 KiB, each numbered so
    that a lost, repeated or reordered piece changes the digest."""
    block = os.urandom(PIECE)
    sent = index = 0
    while sent < length:
        size = min(PIECE, length - sent)
        yield (index.to_bytes(8, "little") + block)[:size]
        sent += size
        index += 1


def expect_end(stream: socket.socket, seconds: float, what: str,
               reset_is_end: bool = False) -> None:
    """Requires the stream to end, with end of file, within ``seconds``."""
    stream.settimeout(seconds)
    try:
        data = stream.recv(1)
    except ConnectionResetError:
        if reset_is_end:
            return
        raise RuntimeError(f"{what}: the stream was reset, not ended")
    except socket.timeout:
        raise RuntimeError(f"{what}: the stream did not end in {seconds:g} s")
    if data:
        raise RuntimeError(f"{what}: the stream carried {data!r} past its end")


def upload(port: int, name: str, length: int, *, pace: float = 0.0,
           stall: tuple[int, float] | None = None,
           counter: Counter | None = None,
           small_buffer: bool = False) -> dict:
    """Sends a body to the guest, which answers with its digest."""
    request = f"sink {name} {length}"
    if stall is not None:
        request += f" {stall[0]} {stall[1]}"
    counter = counter or Counter()
    digest = hashlib.sha256()
    with open_stream(
        port, request, send_buffer=PIECE if small_buffer else None
    ) as stream:
        start = time.monotonic()
        for piece in pieces(length):
            stream.sendall(piece)
            digest.update(piece)
            counter.add(len(piece))
            if pace:
                time.sleep(pace)
        reply = read_line(stream)
        elapsed = time.monotonic() - start
    expected = f"{digest.hexdigest()} {length}"
    if reply != expected:
        raise RuntimeError(f"{name}: the guest read {reply!r}, sent {expected!r}")
    return result("host-to-guest", name, length, elapsed)


def download(port: int, name: str, length: int, *, pace_ms: int = 0,
             stall: tuple[int, float] | None = None,
             counter: Counter | None = None,
             stalled: dict | None = None,
             small_buffer: bool = False) -> dict:
    """Receives a body from the guest, which follows it with its digest."""
    digest = hashlib.sha256()
    with open_stream(
        port, f"source {name} {length} {pace_ms}",
        receive_buffer=PIECE if small_buffer else None
    ) as stream:
        start = time.monotonic()
        read_body(stream, length, digest, counter, stall, stalled)
        trailer = read_line(stream)
        elapsed = time.monotonic() - start
        if trailer != digest.hexdigest():
            raise RuntimeError(
                f"{name}: the guest sent {trailer}, the host received "
                f"{digest.hexdigest()}"
            )
        # The guest ends its side and waits for the host's end.
        stream.shutdown(socket.SHUT_WR)
        expect_end(stream, EOF_LIMIT_SECONDS, f"{name} close")
    return result("guest-to-host", name, length, elapsed)


def result(direction: str, name: str, length: int, seconds: float) -> dict:
    return {
        "name": name,
        "direction": direction,
        "bytes": length,
        "seconds": round(seconds, 3),
        "mb_per_s": round(length / seconds / 1e6, 2),
    }


def guest_reply(port: int, request: str) -> str:
    with open_stream(port, request) as stream:
        return read_line(stream)


def probe(port: int, count: int, interval: float) -> list[float]:
    """Round trips of 1 KiB through an echo stream, in seconds."""
    message = os.urandom(1024)
    times = []
    with open_stream(port, "echo probe") as stream:
        for _ in range(count):
            start = time.monotonic()
            stream.sendall(message)
            received = bytearray()
            while len(received) < len(message):
                chunk = stream.recv(len(message) - len(received))
                if not chunk:
                    raise EOFError("the probe stream ended")
                received += chunk
            times.append(time.monotonic() - start)
            if bytes(received) != message:
                raise RuntimeError("the probe stream returned other bytes")
            time.sleep(interval)
    return times


def forward_ports(guest: Guest) -> dict[int, int]:
    """Guest port to host port, from the launcher's lines at the first boot.
    The launcher's stderr is the console's terminal, which loses what it has
    no room for once a run is busy, so only a boot's own start can be read."""
    found = {
        int(guest_port): int(host_port)
        for _, host_port, guest_port in FORWARD.findall(guest.output())
    }
    for port in (SERVER_PORT, REFUSED_PORT):
        if port not in found:
            raise RuntimeError(f"the launcher did not report a forward to {port}")
    return found


def start_server(guest: Guest) -> None:
    guest.check_command(
        "python3 -c 'import socket; "
        "socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)'",
        30,
    )
    guest.check_command(
        f"(python3 {SERVER} {SERVER_PORT} >{SERVER_LOG} 2>&1 </dev/null &)", 30
    )


def wait_for_server(guest: Guest, port: int) -> None:
    """The first reply of the guest server through the forward."""
    deadline = time.monotonic() + 60
    while True:
        try:
            if guest_reply(port, "ping ready") == "pong":
                return
        except (OSError, EOFError):
            pass
        if time.monotonic() > deadline:
            raise RuntimeError("the guest vsock server never answered")
        drain(guest, 0.5)


def run_bulk(guest: Guest, port: int) -> list[dict]:
    outcomes = []
    for function, name in ((upload, "bulk-up"), (download, "bulk-down")):
        outcome = Background(function, port, name, BULK_BYTES).join(
            guest, BULK_LIMIT_SECONDS, name)
        outcomes.append(outcome)
        print(
            f"{name}: {BULK_BYTES >> 20} MiB in {outcome['seconds']} s, "
            f"{outcome['mb_per_s']} MB/s"
        )
    return outcomes


def run_parallel(guest: Guest, port: int) -> dict:
    start = time.monotonic()
    transfers = [
        Background(upload, port, "parallel-up-0", PARALLEL_BYTES),
        Background(download, port, "parallel-down-0", PARALLEL_BYTES),
        Background(upload, port, "parallel-up-1", PARALLEL_BYTES),
        Background(download, port, "parallel-down-1", PARALLEL_BYTES),
    ]
    for index, transfer in enumerate(transfers):
        transfer.join(guest, PARALLEL_LIMIT_SECONDS, f"parallel stream {index}")
    elapsed = time.monotonic() - start
    total = 4 * PARALLEL_BYTES
    print(f"parallel: 4 streams, {total >> 20} MiB in {elapsed:.2f} s")
    return {"streams": 4, "bytes": total, "seconds": round(elapsed, 3)}


def wait_until(guest: Guest, condition, seconds: float, what: str) -> None:
    deadline = time.monotonic() + seconds
    while not condition():
        if time.monotonic() > deadline:
            raise RuntimeError(what)
        drain(guest, 0.05)


def run_stalled_host_reader(guest: Guest, port: int) -> None:
    """A host reader that stops holds the guest's sender back, and a second
    stream stays responsive meanwhile."""
    counter, stalled = Counter(), {}
    transfer = Background(
        download, port, "stall-down", STALL_BYTES,
        stall=(STALL_AFTER, STALL_SECONDS), counter=counter, stalled=stalled,
        small_buffer=True)
    wait_until(guest, lambda: "start" in stalled, 60,
               "the host reader never reached its stall")
    # Let the guest's sender run into the stall before measuring it.
    drain(guest, STALL_SECONDS / 3)
    sent = int(guest_reply(port, "progress stall-down"))
    times = probe(port, 5, 0.1)
    if sent < STALL_AFTER or sent > STALL_AFTER + BACKPRESSURE_BYTES:
        raise RuntimeError(
            f"stall-down: the guest handed on {sent} bytes while the host "
            f"read nothing after {STALL_AFTER}; the limit is "
            f"{STALL_AFTER + BACKPRESSURE_BYTES}"
        )
    if max(times) > PROBE_LIMIT_SECONDS:
        raise RuntimeError(
            f"stall-down: a second stream took {max(times):.2f} s per round "
            "trip while a reader stalled"
        )
    transfer.join(guest, STREAM_TIMEOUT_SECONDS, "stall-down")
    print(
        f"stall-down: the guest handed on {sent} bytes during the stall, the "
        f"second stream's worst round trip was {max(times) * 1000:.0f} ms"
    )


def run_stalled_guest_reader(guest: Guest, port: int) -> None:
    """A guest reader that stops holds the host's sender back."""
    counter = Counter()
    transfer = Background(
        upload, port, "stall-up", STALL_BYTES,
        stall=(STALL_AFTER, STALL_SECONDS), counter=counter, small_buffer=True)
    wait_until(guest, lambda: counter.value >= STALL_AFTER, 60,
               "the host sender never started")
    # The counter, sampled while the guest reads nothing.
    samples: list[tuple[float, int]] = []
    start = time.monotonic()
    while time.monotonic() - start < STALL_SECONDS and not transfer.done():
        samples.append((time.monotonic(), counter.value))
        drain(guest, 0.1)
    idle, held_at = longest_idle(samples)
    if idle < IDLE_SECONDS:
        raise RuntimeError(
            f"stall-up: the host sender stopped for {idle:.1f} s of the "
            f"guest's {STALL_SECONDS:g} s stall; it must stop for "
            f"{IDLE_SECONDS:g} s"
        )
    if held_at > STALL_AFTER + BACKPRESSURE_BYTES:
        raise RuntimeError(
            f"stall-up: the host handed on {held_at} bytes while the guest "
            f"read nothing after {STALL_AFTER}; the limit is "
            f"{STALL_AFTER + BACKPRESSURE_BYTES}"
        )
    transfer.join(guest, STREAM_TIMEOUT_SECONDS, "stall-up")
    print(f"stall-up: the host sender was held back for {idle:.1f} s at "
          f"{held_at} bytes")


def longest_idle(samples: list[tuple[float, int]]) -> tuple[float, int]:
    """The longest time over which the counter moved by at most IDLE_BYTES,
    and its value there."""
    best, value = 0.0, 0
    for first in range(len(samples)):
        for last in range(first, len(samples)):
            if samples[last][1] - samples[first][1] > IDLE_BYTES:
                break
            if samples[last][0] - samples[first][0] > best:
                best = samples[last][0] - samples[first][0]
                value = samples[last][1]
    return best, value


def run_half_closes(guest: Guest, port: int) -> None:
    def host_closes() -> None:
        digest = hashlib.sha256()
        with open_stream(port, "halfhost halfhost") as stream:
            for piece in pieces(HALF_CLOSE_BYTES):
                stream.sendall(piece)
                digest.update(piece)
            stream.shutdown(socket.SHUT_WR)
            reply = read_line(stream)
            expect_end(stream, EOF_LIMIT_SECONDS, "halfhost close")
        expected = f"{digest.hexdigest()} {HALF_CLOSE_BYTES}"
        if reply != expected:
            raise RuntimeError(
                f"halfhost: the guest read {reply!r} before the host's "
                f"half-close, sent {expected!r}"
            )

    def guest_closes() -> None:
        received = hashlib.sha256()
        with open_stream(port, f"halfguest halfguest {HALF_CLOSE_BYTES}") as stream:
            read_body(stream, HALF_CLOSE_BYTES, received)
            trailer = read_line(stream)
            if trailer != received.hexdigest():
                raise RuntimeError("halfguest: the body arrived altered")
            expect_end(stream, EOF_LIMIT_SECONDS, "halfguest half-close")
            # The guest's half-close ends its stream only: the host still talks.
            sent = hashlib.sha256()
            for piece in pieces(HALF_CLOSE_REPLY_BYTES):
                stream.sendall(piece)
                sent.update(piece)
            stream.shutdown(socket.SHUT_WR)
            expect_end(stream, EOF_LIMIT_SECONDS, "halfguest close")
        reported = guest_reply(port, "report halfguest")
        expected = f"{sent.hexdigest()} {HALF_CLOSE_REPLY_BYTES}"
        if reported != expected:
            raise RuntimeError(
                f"halfguest: the guest read {reported!r} after its own "
                f"half-close, the host sent {expected!r}"
            )

    Background(host_closes).join(guest, STREAM_TIMEOUT_SECONDS, "host half-close")
    Background(guest_closes).join(guest, STREAM_TIMEOUT_SECONDS, "guest half-close")
    print("half-closes: the host's and the guest's each ended one direction only")


def run_refused_and_leaks(guest: Guest, ports: dict[int, int]) -> None:
    for attempt in range(3):
        start = time.monotonic()
        stream = socket.create_connection(("127.0.0.1", ports[REFUSED_PORT]))
        try:
            try:
                stream.sendall(b"ping refused\n")
            except OSError:
                pass
            expect_end(stream, REFUSED_LIMIT_SECONDS, "refused port", True)
        finally:
            stream.close()
        elapsed = time.monotonic() - start
        if elapsed > REFUSED_LIMIT_SECONDS:
            raise RuntimeError(f"refused port: ended after {elapsed:.1f} s")
    # More connections than the device holds, each ended, show that none leaks.
    for _ in range(CONNECTIONS_ONE_BY_ONE):
        stream = socket.create_connection(("127.0.0.1", ports[REFUSED_PORT]))
        try:
            try:
                stream.sendall(b"ping refused\n")
            except OSError:
                pass
            expect_end(stream, REFUSED_LIMIT_SECONDS, "refused port", True)
        finally:
            stream.close()
    for _ in range(CONNECTIONS_ONE_BY_ONE):
        if guest_reply(ports[SERVER_PORT], "ping again") != "pong":
            raise RuntimeError("the guest server did not answer")
    print(
        f"refused port: ended at once, and {CONNECTIONS_ONE_BY_ONE} refused and "
        f"{CONNECTIONS_ONE_BY_ONE} answered connections left no entry behind"
    )


def run_suspend(guest: Guest, console: ManagementConsole, port: int) -> dict:
    """A host-requested suspend in the middle of a stream each way and of an
    idle echo stream: all three survive, and neither moves while it lasts."""
    idle = open_stream(port, "echo suspended")
    idle.sendall(b"p")
    if idle.recv(1) != b"p":
        raise RuntimeError("suspend: the echo stream did not echo")
    up, down = Counter(), Counter()
    uploading = Background(
        upload, port, "straddle-up", STRADDLE_BYTES,
        pace=STRADDLE_PACE_SECONDS, counter=up)
    downloading = Background(
        download, port, "straddle-down", STRADDLE_BYTES,
        pace_ms=int(STRADDLE_PACE_SECONDS * 1000), counter=down)
    wait_until(guest, lambda: min(up.value, down.value) >= 1 << 20, 60,
               "suspend: the streams never started")
    if uploading.done() or downloading.done():
        raise RuntimeError("suspend: a stream ended before the suspend")
    samples: list[tuple[float, int, int]] = []
    echoed = 0
    started = time.monotonic()

    def poke() -> None:
        nonlocal echoed
        idle.sendall(b"x")
        echoed += 1
        samples.append((time.monotonic() - started, up.value, down.value))

    hold(guest, console, "vsock", poke)
    steady = [sample for sample in samples if sample[0] - samples[0][0] >= 2.0]
    for index, direction in ((1, "host-to-guest"), (2, "guest-to-host")):
        moved = max(s[index] for s in steady) - min(s[index] for s in steady)
        if moved > IDLE_BYTES:
            raise RuntimeError(
                f"suspend: the {direction} stream moved {moved} bytes while "
                "the guest was suspended"
            )
    if max(s[1] for s in samples) >= STRADDLE_BYTES or \
            max(s[2] for s in samples) >= STRADDLE_BYTES:
        raise RuntimeError("suspend: a stream was over before the guest resumed")
    answered = bytearray()
    idle.settimeout(ECHO_SECONDS)
    while len(answered) < echoed:
        chunk = idle.recv(echoed - len(answered))
        if not chunk:
            raise RuntimeError("suspend: the echo stream ended with the suspend")
        answered += chunk
    if bytes(answered) != b"x" * echoed:
        raise RuntimeError(f"suspend: the echo stream returned {bytes(answered)!r}")
    idle.close()
    outcomes = [
        uploading.join(guest, STREAM_TIMEOUT_SECONDS, "suspend upload"),
        downloading.join(guest, STREAM_TIMEOUT_SECONDS, "suspend download"),
    ]
    print(
        f"suspend: both streams and the echo stream ({echoed} bytes) survived "
        "a suspend and resume"
    )
    return {"echoed": echoed, "streams": outcomes}


def run_restart(guest: Guest, port: int) -> None:
    stream = open_stream(port, "echo restart")
    stream.sendall(b"p")
    if stream.recv(1) != b"p":
        raise RuntimeError("restart: the echo stream did not echo")
    guest.restart_to_shell(BOOT_SECONDS)
    expect_end(stream, RESTART_EOF_SECONDS, "restart", True)
    stream.close()
    # The new boot has its own device, forwarder and server, and the forwarder
    # listens on the same host port again.
    start_server(guest)
    wait_for_server(guest, port)
    outcome = Background(upload, port, "after-restart", 4 << 20).join(
        guest, STREAM_TIMEOUT_SECONDS, "after restart")
    print(f"restart: the open stream ended, and the next boot carried "
          f"{outcome['bytes'] >> 20} MiB")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-vsock-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    # The server's forward has a port of its own choosing, so that the next
    # boot's forwarder can be found again; the refused port's is chosen by the
    # launcher, which prints it.
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        server_port = probe.getsockname()[1]
    try:
        with Guest(
            arguments.emulator.resolve(),
            arguments.rootfs.resolve(),
            "1G",
            "tmpfs",
            (
                f"ncpus={VCPUS}",
                f"--vsock-forward=127.0.0.1:{server_port}-{SERVER_PORT}",
                f"--vsock-forward=127.0.0.1:0-{REFUSED_PORT}",
            ),
            overlay_shell=True,
            payloads={SERVER: GuestFile(GUEST_SERVER.encode(), 0o755)},
        ) as guest:
            guest.boot_to_shell(BOOT_SECONDS)
            console = ManagementConsole(guest, private)
            request = len(guest.output())
            ports = forward_ports(guest)
            if ports[SERVER_PORT] != server_port:
                raise RuntimeError(
                    f"the launcher reported port {ports[SERVER_PORT]} for the "
                    f"forward asked to listen on {server_port}")
            port = server_port
            start_server(guest)
            wait_for_server(guest, port)

            report: dict = {}
            report["bulk"] = run_bulk(guest, port)
            report["parallel"] = run_parallel(guest, port)
            run_stalled_host_reader(guest, port)
            run_stalled_guest_reader(guest, port)
            run_half_closes(guest, port)
            run_refused_and_leaks(guest, ports)
            report["suspend"] = run_suspend(guest, console, port)

            log = guest.check_command(f"cat {SERVER_LOG}", 30).strip()
            if log:
                raise RuntimeError(f"the guest server reported:\n{log}")
            require_clean(guest, "vsock", request)
            run_restart(guest, port)
            print(json.dumps(report))
            if not guest.shutdown(60):
                raise RuntimeError("the guest did not power off")
    finally:
        shutil.rmtree(private, ignore_errors=True)
    print("vsock streams carried every body intact in both directions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
