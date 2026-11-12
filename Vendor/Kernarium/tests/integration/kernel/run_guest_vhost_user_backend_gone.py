#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lose a vhost-user backend while its device is bound.

A lost backend retires its device rather than being warned about: the
guest stops the descriptors that reported the loss, and the
connection-broken path breaks the device's virtqueues and unregisters it.
Its driver's removal then deletes the virtqueues, which tells a live backend
to stop using the rings, and must send a lost one nothing.

The runner serves a Virtio console through the guest kernel's
``virtio_uml.device=`` command-line device from a minimal backend of its
own, which offers what a Darwin guest requires (reply acknowledgements,
backend requests and in-band notifications) and acknowledges every request.
Once the console driver has bound, a writer on the second vCPU starts
writing to the console's hvc0. The backend never uses a buffer, so the
console's write waits in put_chars() for the backend to take it, which is
the output a console has in flight when its backend goes. Once the writer's
first kick has arrived, the backend closes its connection and every
descriptor the guest gave it. The guest must report the loss, remove the
device without a kernel warning, oops or panic, keep running, and power off
with status 0. The contract is the lost-backend paragraph of
docs/architecture.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import array
import os
import pathlib
import re
import shutil
import socket
import struct
import sys
import tempfile
import threading


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


EXIT_SECONDS = 60
CONSOLE_DEVICE = 3
GET_FEATURES = 1
GET_VRING_BASE = 11
GET_PROTOCOL_FEATURES = 15
GET_QUEUE_NUM = 17
GET_CONFIG = 24
VRING_KICK = 35
OUTPUT_QUEUE = 1
REPLY = 0x4
NEED_REPLY = 0x8
VERSION = 0x1
FEATURES = (1 << 32) | (1 << 30)
# Reply acknowledgements, backend requests and in-band notifications.
PROTOCOL_FEATURES = (1 << 3) | (1 << 5) | (1 << 14)
FAILURE = re.compile(
    r"WARNING:|\bBUG\b|Oops|Call Trace:|Kernel panic|Darwin UML panic|"
    r"Darwin POSIX|Kernel mode fault"
)
LOST = "the vhost-user backend is gone"


class Backend(threading.Thread):
    """Acknowledge every request on one connection until told to vanish."""

    def __init__(self, path: str) -> None:
        super().__init__(daemon=True)
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._listener.bind(path)
        self._listener.listen(1)
        self._listener.settimeout(240)
        self._connection: socket.socket | None = None
        self._descriptors: list[int] = []
        self._vanish = threading.Event()
        self.output_kicked = threading.Event()
        self.failure: BaseException | None = None

    def _receive(self, size: int, idle: bool = False) -> bytes:
        """Read ``size`` bytes; only an ``idle`` read may time out, and only
        before its first byte, so a message is never left half read."""
        assert self._connection is not None
        data = b""
        while len(data) < size:
            try:
                chunk, ancillary, _, _ = self._connection.recvmsg(
                    size - len(data), socket.CMSG_SPACE(16 * 4)
                )
            except socket.timeout:
                if idle and not data:
                    raise
                continue
            if not chunk:
                raise EOFError("the guest closed the connection")
            for level, kind, payload in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    received = array.array("i")
                    received.frombytes(payload[:len(payload) - len(payload) % 4])
                    self._descriptors.extend(received)
            data += chunk
        return data

    def _reply(self, request: int, payload: bytes) -> None:
        assert self._connection is not None
        self._connection.sendall(
            struct.pack("<III", request, VERSION | REPLY, len(payload))
            + payload
        )

    def run(self) -> None:
        try:
            self._connection, _ = self._listener.accept()
            self._connection.settimeout(0.2)
            while not self._vanish.is_set():
                try:
                    header = self._receive(12, idle=True)
                except socket.timeout:
                    continue
                request, flags, size = struct.unpack("<III", header)
                payload = self._receive(size) if size else b""
                if request == GET_FEATURES:
                    self._reply(request, struct.pack("<Q", FEATURES))
                elif request == GET_PROTOCOL_FEATURES:
                    self._reply(request, struct.pack("<Q", PROTOCOL_FEATURES))
                elif request == GET_QUEUE_NUM:
                    self._reply(request, struct.pack("<Q", 2))
                elif request == GET_VRING_BASE:
                    self._reply(request, payload[:4] + bytes(4))
                elif request == GET_CONFIG:
                    self._reply(request, payload)
                elif request == VRING_KICK:
                    if struct.unpack_from("<I", payload)[0] == OUTPUT_QUEUE:
                        self.output_kicked.set()
                    self._reply(request, struct.pack("<Q", 0))
                elif flags & NEED_REPLY:
                    self._reply(request, struct.pack("<Q", 0))
        except BaseException as error:  # reported by the main thread
            if not self._vanish.is_set():
                self.failure = error

    def vanish(self) -> None:
        """Close the connection and every descriptor the guest sent."""
        self._vanish.set()
        self.join(10)
        if self._connection is not None:
            self._connection.close()
        for descriptor in self._descriptors:
            try:
                os.close(descriptor)
            except OSError:
                pass
        self._descriptors.clear()
        self._listener.close()


def require_clean(tail: str, step: str) -> None:
    failure = FAILURE.search(tail)
    if failure is not None:
        raise RuntimeError(
            f"{step}: the guest printed {failure[0]!r}:\n"
            + tail[max(0, failure.start() - 2000):][:8000]
        )


def run(arguments: argparse.Namespace, private: pathlib.Path) -> None:
    path = str(private / "console.sock")
    backend = Backend(path)
    backend.start()
    try:
        with Guest(
            arguments.emulator,
            arguments.rootfs,
            "1G",
            "tmpfs",
            # SLUB checks that the removal frees nothing twice.
            (
                "ncpus=2",
                "slab_debug=FZPU",
                f"virtio_uml.device={path}:{CONSOLE_DEVICE}",
            ),
            init_shell=True,
            engine=arguments.engine,
        ) as guest:
            guest.boot_to_shell(180)
            platform = guest.check_command(
                "grep -aF '" + path + ":' /sys/module/virtio_uml/parameters/device"
                " | cut -d: -f3",
                30,
            ).strip().splitlines()[-1]
            device = f"/sys/devices/virtio-uml-cmdline/virtio-uml.{platform}"
            guest.check_command(
                "for i in $(seq 1 100); do "
                f"ls {device}/virtio*/driver >/dev/null 2>&1 && break; "
                f"sleep 0.1; done; readlink {device}/virtio*/driver"
                " | grep -q virtio_console",
                60,
            )
            if backend.failure is not None:
                raise RuntimeError(f"the backend failed: {backend.failure!r}")
            # Pinned off CPU 0, which alone polls the host descriptors that
            # report the loss: put_chars() waits with interrupts disabled.
            guest.check_command(
                "(python3 -c 'import os; os.sched_setaffinity(0, {1}); "
                "os.execvp(\"sh\", [\"sh\", \"-c\", "
                "\"while printf x; do :; done\"])' "
                ">/dev/hvc0 2>/dev/null </dev/null &)",
                30,
            )
            if not backend.output_kicked.wait(30):
                raise RuntimeError("the console's output was never kicked")
            start = len(guest.output())
            backend.vanish()
            guest.check_command(
                "for i in $(seq 1 100); do "
                f"[ -e {device} ] || break; sleep 0.1; done; ! [ -e {device} ]",
                60,
            )
            guest.check_command("echo alive", 30)
            tail = guest.output()[start:]
            if LOST not in tail:
                raise RuntimeError(f"the guest never reported the loss:\n{tail}")
            require_clean(tail, "lost backend")
            request = len(guest.output())
            if not guest.shutdown(EXIT_SECONDS):
                raise RuntimeError("the guest did not power off")
            require_clean(guest.output()[request:], "power-off")
            if guest.return_code != 0:
                raise RuntimeError(
                    f"the emulator exited with status {guest.return_code}:\n"
                    + guest.output()[request:][-4000:]
                )
    finally:
        backend.vanish()
    print("the guest retired the device of a lost backend without a warning")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()

    # Short, so the backend's socket path fits sun_path.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-vu-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        run(arguments, private)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
