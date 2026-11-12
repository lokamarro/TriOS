#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A suspended guest stays suspended until one of its wake sources fires.

The management console's ``suspend`` puts the guest into full system
suspend, and its ``wake`` request, arriving on the console's socket, is
what ends it. Every other descriptor the guest reads stays quiet while it
is suspended, as an O_ASYNC descriptor whose flag the suspend cleared does
on a Linux host: data that arrives on it waits for the resume.

Each arm suspends a two-vCPU guest and keeps one kind of descriptor busy
for HOLD_SECONDS: a host client writing a byte every 250 ms through a
network forward into a guest echo server, a line of console input every
250 ms, or a vhost-user backend (the churn server) raising a config change on
its backend channel every 50 ms. The guest must not leave suspend until the
runner sends ``wake``, then must resume promptly and handle what arrived while
it slept: every forwarded byte is echoed, the console input runs, and the
backend's config changes are acknowledged again.

A ``wake`` that arrives while the suspend is still being entered, before
it has stopped interrupts, is answered at once instead of waiting on the
console. The early-wake arm sends one as soon as the guest prints its
suspend entry line, and the guest must not stay suspended.

A guest can also suspend itself, as Android does once its display is off.
The self-wake arm checks that a ``wake``, which the host sends when its
user presses a power key, only answers a running guest, ends the guest's
own suspend once it holds, and ends it while it is still being entered:
eight threads in long reads keep the freezer waiting for most of a second,
as a busy Android does, and the wake must make the PM core give up.
The sync-wake arm sends the wake earlier still, while the guest's own
suspend syncs file systems, before the PM core prepares or freezes
anything: dirty data on a loop device backed by a frozen file system
holds the sync until the arm thaws it. The suspend must end right after
the sync, without freezing tasks, and the guest's next suspend must
still hold.
A suspend the host asked for is entered again when one of the guest's own
wake sources ended it. In the sync-alarm arm an RTC alarm fires while the
host's suspend syncs file systems: the guest must give that attempt up,
enter the suspend again, reply that it is suspended, and hold until the
host's wake. In the sync-host-wake arm the host's own wake arrives during
that sync instead: the request must fail without a second attempt, and the
host's next suspend must hold.
The power arm forwards a power key through the VNC display to a guest that
suspended itself: the key must wake it promptly and reach it exactly once.
The network-wake arm sends a byte through the network forward to a guest that
suspended itself: the suspend must hold while nothing arrives, the byte must
end it promptly and be echoed, and the interface's wakeup source must have
reported it. The host's suspend asked for right after a burst must then hold
under the same traffic, entered once, as the network arm requires of any suspend
the host asked for.
The freezer-hold arm holds the freezer with a FUSE request that is never
answered, so one task can neither freeze nor be killed by the freezer's fake
signal, and the guest's suspend keeps failing every 20 s. A host that waits for
the suspend's reply must stop at its deadline: the guest replies to nothing in
eight seconds, a ``wake`` is answered ``running`` while the suspend blocks, and
the suspend's own socket then carries its ``Suspend failed: -16`` within two
seconds, with the PM core reporting that freezing user space was aborted. The
shell answers, and once the holder is released the next suspend holds and wakes.
The arm checks the kernel half of the host's abort; the host half is the
guest-console and emulator-lifecycle unit tests.
The contract is the suspend paragraph of kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


VCPUS = 2
HOLD_SECONDS = 6.0
POKE_SECONDS = 0.25
REQUEST_SECONDS = 60.0
RESUME_SECONDS = 30.0
GUEST_PORT = 7000
# An experimental Virtio device ID (65535 is the battery's) that no guest
# driver binds: the guest acknowledges each config change and does no more.
CHURN_DEVICE = 65534
CHURN_PERIOD_MS = 50
# The backend raises one change at a time and waits for the guest to
# acknowledge it, so the suspend holds one back. The runner asks the backend
# for its count when the guest has resumed, and the guest runs for a second
# after that, which allows about 20 more. Half of that shows that the guest
# acknowledged them again.
CHURN_MINIMUM_CHANGES = 10
SERVER_EXIT_SECONDS = 20
MCONSOLE_MAGIC = 0xCAFEBABE
MCONSOLE_VERSION = 2
MCONSOLE_DATA = 512
SUSPEND_ENTRY = "PM: suspend entry"
SUSPEND_EXIT = "PM: suspend exit"
# Printed once the PM core has begun preparing the suspend, so a wake sent
# after it reaches a suspend in progress.
SUSPEND_FREEZING = "Freezing user space processes"
SELF_SUSPEND = "(echo mem > /sys/power/state) &\n"
HOLDER = "/opt/uml-test/hold_freeze.py"
HOLDER_READERS = 8
# Suspends the guest while HOLDER_READERS threads are each in one read of
# 2 GiB of holes from tmpfs, which no signal interrupts, so the freezer
# waits for them; on two vCPUs that keeps the suspend being entered for
# most of a second. Prints whether the suspend ran or was refused.
HOLDER_SOURCE = """\
import os, sys, threading
size = 2 << 20
descriptor = os.open("/run/hold-freeze", os.O_RDWR | os.O_CREAT | os.O_TRUNC)
os.ftruncate(descriptor, 1023 * size)
views = [memoryview(bytearray(size))] * 1023
readers = int(sys.argv[1])
start = threading.Barrier(readers + 1)
done = threading.Event()

def read():
    start.wait()
    while not done.is_set():
        os.preadv(descriptor, views, 0)

for _ in range(readers):
    threading.Thread(target=read, daemon=True).start()
start.wait()
state = os.open("/sys/power/state", os.O_WRONLY)
try:
    os.write(state, b"mem")
    result = "suspended"
except OSError as error:
    result = f"refused: {os.strerror(error.errno)}"
os.close(state)
done.set()
print(f"hold-freeze: {result}", flush=True)
"""
HOLDER_REFUSED = "hold-freeze: refused"
# Holds the freezer for as long as it runs: a FUSE mount whose LOOKUP is read
# and never answered. INIT is answered so that the request is not left in
# fuse_get_req(), whose wait is freezable; the stat child then sleeps in
# request_wait_answer(), killable after the freezer's fake signal but not
# freezable, so the freezer fails after pm_freeze_timeout (20 s). Killing the
# holder closes /dev/fuse, which aborts the connection and ends the child.
FREEZER_HOLDER = "/opt/uml-test/freeze_hold.py"
FREEZER_HOLDER_LOG = "/run/freeze-hold.log"
FREEZER_HOLDER_SOURCE = """\
import ctypes, os, struct, sys, time

FUSE_INIT = 26
libc = ctypes.CDLL(None, use_errno=True)
os.makedirs("/run/freeze-hold", exist_ok=True)
fd = os.open("/dev/fuse", os.O_RDWR)
options = f"fd={fd},rootmode=40000,user_id=0,group_id=0".encode()
if libc.mount(b"freeze-hold", b"/run/freeze-hold", b"fuse", 0, options) != 0:
    print("freeze-hold: mount failed: " + os.strerror(ctypes.get_errno()),
          flush=True)
    sys.exit(1)
request = os.read(fd, 1 << 20)
length, opcode, unique = struct.unpack_from("<IIQ", request)
major, minor = struct.unpack_from("<II", request, 40)
if opcode != FUSE_INIT:
    print(f"freeze-hold: first request {opcode}, not INIT", flush=True)
    sys.exit(1)
body = struct.pack("<IIIIHHIIHHIIH11H", 7, min(minor, 31), 0, 0, 0, 0,
                   4096, 0, 0, 0, 0, 0, 0, *([0] * 11))
os.write(fd, struct.pack("<IiQ", 16 + len(body), 0, unique) + body)
if os.fork() == 0:
    # The child keeps no descriptor, so that ending the holder is what closes
    # /dev/fuse: a request already read by the daemon is waited for without a
    # way out, and a child that held the descriptor would hold the connection.
    os.close(fd)
    try:
        os.stat("/run/freeze-hold/x")
        os._exit(0)
    except OSError as error:
        os._exit(error.errno & 0x7F)
os.read(fd, 1 << 20)
print("freeze-hold: held", flush=True)
while True:
    time.sleep(3600)
"""
# The reply the guest must not give within this long of a suspend whose
# freezer is held.
FREEZER_SILENCE_SECONDS = 8.0
# The PM core's report that a wake ended the freezing, which proves the abort
# came from the wake and not from the freezer's own timeout.
FREEZER_ABORTED = "Freezing user space processes aborted"
# How soon after the wake the aborted suspend's own socket reports its end.
FREEZER_FINAL_SECONDS = 2.0
# The holder with no readers only suspends and reports. Before a suspend,
# the sync arms leave dirty data on a loop device whose backing file is on a
# frozen ext4 on a scratch disk, held open so that closing it writes nothing
# back: the suspend's file system sync then waits for the loop's writeback
# until the arm thaws the file system.
SYNC_DISK_MIB = 256
SYNC_DONE = "Filesystems sync: "
SYNC_HELD = "/run/sync-held"
SYNC_HOLD = (
    f"mke2fs -q -t ext4 /dev/ubdb && mkdir -p {SYNC_HELD} && "
    f"mount /dev/ubdb {SYNC_HELD} && truncate -s 64M {SYNC_HELD}/backing && "
    f"loop=$(losetup -f --show {SYNC_HELD}/backing) && "
    "(sleep 100000 <$loop >/dev/null 2>&1 &) && "
    "dd if=/dev/zero of=$loop bs=1M count=16 2>/dev/null && "
    f"fsfreeze -f {SYNC_HELD} && "
    "sed -n 's/^Dirty: *//p' /proc/meminfo"
)
# Long enough after the suspend's entry line for it to have begun.
SYNC_SETTLE_SECONDS = 1.0
SUSPEND_FAILED = "Suspend failed: "
RTC_ALARM = "/sys/class/rtc/rtc0/wakealarm"
# The RTC's interrupts on every CPU, summed: each is an alarm that fired.
RTC_INTERRUPTS = (
    "awk '$NF == \"rtc\" { n = 0; for (i = 2; i < NF - 1; i++) n += $i; "
    "print n }' /proc/interrupts"
)
VNC_HANDSHAKE_SECONDS = 30
XF86_POWER_OFF = 0x1008FF2A
KEY_READER = "/opt/uml-test/power_keys.py"
KEY_LOG = "/run/power-keys"
# Logs the value of every KEY_POWER event the VNC keyboard delivers.
KEY_READER_SOURCE = f"""\
import glob, os, struct
names = glob.glob("/sys/class/input/event*/device/name")
device = next(
    "/dev/input/" + path.split("/")[4]
    for path in names
    if open(path).read().strip() == "Darwin UML VNC Keyboard"
)
descriptor = os.open(device, os.O_RDONLY)
layout = struct.Struct("llHHi")
with open("{KEY_LOG}", "w", buffering=1) as log:
    log.write("ready\\n")
    while True:
        data = os.read(descriptor, layout.size * 64)
        for offset in range(0, len(data) - layout.size + 1, layout.size):
            _, _, kind, code, value = layout.unpack_from(data, offset)
            if kind == 1 and code == 116:
                log.write(f"{{value}}\\n")
"""
# A frame ends a suspend the guest started itself within this long.
NETWORK_WAKE_SECONDS = 5.0
# How many frames the network interface's wakeup source has reported.
NETWORK_WAKEUPS = (
    "for d in /sys/class/wakeup/wakeup*; do "
    "[ \"$(cat $d/name)\" = darwin-net-rx ] && cat $d/event_count; done; true"
)
ECHO = "/opt/uml-test/suspend_echo.py"
ECHO_SERVER = f"""\
import socket
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("0.0.0.0", {GUEST_PORT}))
listener.listen(1)
while True:
    connection, _ = listener.accept()
    while True:
        data = connection.recv(4096)
        if not data:
            break
        connection.sendall(data)
    connection.close()
"""
# The console input's echo shows the format; only its output joins the words.
CONSOLE_LINE = "printf 'suspend-%s-%s\\n' console delivered\n"
CONSOLE_OUTPUT = "suspend-console-delivered"
FAILURE = re.compile(
    r"WARNING:|\bBUG\b|Oops|Call Trace:|Kernel panic|Darwin UML panic|"
    r"Darwin POSIX|Kernel mode fault|rcu: INFO"
)
CHURN_MARK = re.compile(rb"vhost-user churn mark: (\d+)")
CHURN_SUMMARY = re.compile(rb"vhost-user churn: (\d+) config changes raised")


class Request:
    """One management-console request, answered at a socket of its own."""

    def __init__(self, directory: pathlib.Path, command: str, name: str) -> None:
        console = directory / "mconsole"
        if not console.is_socket():
            raise RuntimeError(f"the guest's management console is not {console}")
        self.command = command
        self._reply = directory / f"mc-suspend-{name}"
        self._reply.unlink(missing_ok=True)
        self._endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            self._endpoint.bind(str(self._reply))
            data = command.encode()
            self._endpoint.sendto(
                struct.pack("<III", MCONSOLE_MAGIC, MCONSOLE_VERSION, len(data))
                + data.ljust(MCONSOLE_DATA, b"\0"),
                str(console),
            )
            self._endpoint.setblocking(False)
        except BaseException:
            self.close()
            raise

    def answer(self) -> tuple[int, str] | None:
        """The reply's error flag and text, or None while there is none."""
        try:
            data = self._endpoint.recv(12 + MCONSOLE_DATA)
        except BlockingIOError:
            return None
        error, _, length = struct.unpack_from("<III", data)
        return error, data[12:12 + length].decode(errors="replace").rstrip("\0")

    def close(self) -> None:
        self._endpoint.close()
        self._reply.unlink(missing_ok=True)


class ManagementConsole:
    """Requests to the guest's management console that keep its serial
    console drained while they wait, so console output never stalls it."""

    def __init__(self, guest: Guest, private: pathlib.Path) -> None:
        self._guest = guest
        self._directory = private / f"uml-kern{guest.process_id}"
        self._sent = 0

    def send(self, command: str) -> Request:
        self._sent += 1
        return Request(self._directory, command, str(self._sent))

    def collect(self, request: Request) -> tuple[int, str]:
        deadline = time.monotonic() + REQUEST_SECONDS
        while (answer := request.answer()) is None:
            if time.monotonic() > deadline:
                raise RuntimeError(
                    f"mconsole {request.command}: no reply in {REQUEST_SECONDS:g} s"
                )
            drain(self._guest, 0.05)
        return answer

    def request(self, command: str) -> str:
        request = self.send(command)
        try:
            error, text = self.collect(request)
        finally:
            request.close()
        if error:
            raise RuntimeError(f"mconsole {command} failed: {text!r}")
        return text


def drain(guest: Guest, seconds: float) -> None:
    if guest.wait_for_exit(seconds):
        raise RuntimeError(
            f"the guest exited with status {guest.return_code}:\n"
            + guest.output()[-4000:]
        )


def hold(guest: Guest, console: ManagementConsole, arm: str, poke) -> None:
    """Suspend, keep a non-wake descriptor busy, then wake.

    The guest prints SUSPEND_EXIT as it resumes, so its appearance before
    the wake request is a guest that left suspend by itself."""
    exits = guest.output().count(SUSPEND_EXIT)
    request = len(guest.output())
    answer = console.request("suspend")
    if "suspended" not in answer:
        raise RuntimeError(f"{arm}: suspend replied {answer!r}")
    entered = time.monotonic()
    while time.monotonic() - entered < HOLD_SECONDS:
        poke()
        drain(guest, POKE_SECONDS)
        if guest.output().count(SUSPEND_EXIT) != exits:
            raise RuntimeError(
                f"{arm}: the guest left suspend "
                f"{time.monotonic() - entered:.2f} s after entering it, "
                "with no wake request:\n" + guest.output()[request:][-4000:]
            )
    woken = time.monotonic()
    answer = console.request("wake")
    if "running" not in answer:
        raise RuntimeError(f"{arm}: wake replied {answer!r}")
    while guest.output().count(SUSPEND_EXIT) == exits:
        if time.monotonic() - woken > RESUME_SECONDS:
            raise RuntimeError(
                f"{arm}: the guest did not resume within {RESUME_SECONDS:g} s "
                "of its wake request:\n" + guest.output()[request:][-4000:]
            )
        drain(guest, POKE_SECONDS)
    print(
        f"{arm}: suspended {woken - entered:.1f} s under traffic, resumed "
        f"{time.monotonic() - woken:.2f} s after the wake request"
    )


def require_clean(guest: Guest, arm: str, request: int) -> None:
    tail = guest.output()[request:]
    failure = FAILURE.search(tail)
    if failure is not None:
        raise RuntimeError(
            f"{arm}: the guest printed {failure[0]!r}:\n"
            + tail[max(0, failure.start() - 2000):][:6000]
        )


def wait_for_line(guest: Guest, line: str, count: int, what: str) -> None:
    """Waits until the transcript holds more than ``count`` of ``line``."""
    deadline = time.monotonic() + REQUEST_SECONDS
    while guest.output().count(line) <= count:
        if time.monotonic() > deadline:
            raise RuntimeError(f"{what}: the guest never printed {line!r}")
        drain(guest, 0.01)


def wait_for_exit_line(guest: Guest, exits: int, what: str) -> float:
    """Waits for the guest to leave suspend; returns how long that took."""
    started = time.monotonic()
    while guest.output().count(SUSPEND_EXIT) == exits:
        if time.monotonic() - started > RESUME_SECONDS:
            raise RuntimeError(
                f"{what}:\n" + guest.output()[-4000:]
            )
        drain(guest, 0.05)
    return time.monotonic() - started


def hold_quietly(guest: Guest, exits: int, seconds: float, what: str) -> None:
    """Requires a suspended guest to stay suspended for ``seconds``."""
    started = time.monotonic()
    while time.monotonic() - started < seconds:
        drain(guest, POKE_SECONDS)
        if guest.output().count(SUSPEND_EXIT) != exits:
            raise RuntimeError(
                f"{what}: the guest left suspend "
                f"{time.monotonic() - started:.2f} s after entering it, with "
                "no wake:\n" + guest.output()[-4000:]
            )


class VncKeyboard:
    """The smallest RFB client that can type: version 3.8, no security."""

    def __init__(self, guest: Guest, port: int) -> None:
        deadline = time.monotonic() + VNC_HANDSHAKE_SECONDS
        while True:
            try:
                self._socket = socket.create_connection(
                    ("127.0.0.1", port), timeout=10
                )
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise RuntimeError("power: the VNC display never listened")
                drain(guest, 0.2)
        try:
            if not self._receive(12).startswith(b"RFB "):
                raise RuntimeError("power: the VNC display is not RFB")
            self._socket.sendall(b"RFB 003.008\n")
            offered = self._receive(self._receive(1)[0])
            if 1 not in offered:
                raise RuntimeError(f"power: VNC security types {list(offered)}")
            self._socket.sendall(b"\x01")
            if struct.unpack(">I", self._receive(4))[0] != 0:
                raise RuntimeError("power: the VNC display refused the client")
            self._socket.sendall(b"\x01")
            server_init = self._receive(24)
            self._receive(struct.unpack(">I", server_init[20:24])[0])
        except BaseException:
            self._socket.close()
            raise

    def _receive(self, size: int) -> bytes:
        data = b""
        while len(data) < size:
            chunk = self._socket.recv(size - len(data))
            if not chunk:
                raise RuntimeError("power: the VNC display closed the client")
            data += chunk
        return data

    def type_key(self, symbol: int) -> None:
        for down in (1, 0):
            self._socket.sendall(struct.pack(">BBxxI", 4, down, symbol))

    def close(self) -> None:
        self._socket.close()


def unused_loopback_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def connect_echo(guest: Guest, port: int) -> socket.socket:
    """A connection through the forward whose echo already works."""
    deadline = time.monotonic() + 30
    while True:
        connection = socket.create_connection(("127.0.0.1", port), timeout=10)
        try:
            connection.settimeout(2)
            connection.sendall(b"p")
            if connection.recv(1) == b"p":
                connection.settimeout(None)
                return connection
        except OSError:
            pass
        connection.close()
        if time.monotonic() > deadline:
            raise RuntimeError("the guest echo server never answered")
        drain(guest, 0.5)


def read_echo(guest: Guest, connection: socket.socket, count: int) -> None:
    connection.setblocking(False)
    received = b""
    deadline = time.monotonic() + RESUME_SECONDS
    while len(received) < count:
        try:
            chunk = connection.recv(4096)
            if not chunk:
                raise RuntimeError("the echo connection closed after the resume")
            received += chunk
            continue
        except BlockingIOError:
            pass
        if time.monotonic() > deadline:
            raise RuntimeError(
                f"network: {len(received)} of the {count} bytes sent during the "
                "suspend were echoed after it"
            )
        drain(guest, 0.1)
    if received != b"x" * count:
        raise RuntimeError(f"network: the echo returned {received!r}")


def run_network(arguments, private: pathlib.Path) -> None:
    port = unused_loopback_port()
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}", f"--net-forward=tcp:127.0.0.1:{port}-{GUEST_PORT}"),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={ECHO: GuestFile(ECHO_SERVER.encode(), 0o755)},
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        guest.check_command(
            f"(python3 {ECHO} >/run/suspend-echo.log 2>&1 </dev/null &)", 30
        )
        connection = connect_echo(guest, port)
        sent = 0

        def send() -> None:
            nonlocal sent
            connection.sendall(b"x")
            sent += 1

        hold(guest, console, "network", send)
        read_echo(guest, connection, sent)
        print(f"network: all {sent} bytes sent during the suspend were echoed")
        connection.close()
        require_clean(guest, "network", request)
        if not guest.shutdown(60):
            raise RuntimeError("network: the guest did not power off")


def network_wakeups(guest: Guest, required: bool = True) -> int:
    """How many frames the interface's wakeup source has reported; 0 for a
    kernel without the source when ``required`` is false."""
    output = guest.check_command(NETWORK_WAKEUPS, 30).strip().splitlines()
    if not output or not output[-1].strip().isdigit():
        if required:
            raise RuntimeError(
                f"network-wake: no darwin-net-rx wakeup source in {output!r}"
            )
        return 0
    return int(output[-1])


def run_network_wake(arguments, private: pathlib.Path) -> None:
    """A frame ends a suspend the guest started itself, and never fails or
    ends one the host asked for."""
    port = unused_loopback_port()
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}", f"--net-forward=tcp:127.0.0.1:{port}-{GUEST_PORT}"),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={ECHO: GuestFile(ECHO_SERVER.encode(), 0o755)},
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        guest.check_command(
            f"(python3 {ECHO} >/run/suspend-echo.log 2>&1 </dev/null &)", 30
        )
        connection = connect_echo(guest, port)
        wakeups = network_wakeups(guest, required=False)

        # The guest's own suspend holds while nothing arrives, and a byte
        # ends it: the guest resumes and echoes it.
        exits = guest.output().count(SUSPEND_EXIT)
        entries = guest.output().count(SUSPEND_ENTRY)
        guest._write(SELF_SUSPEND)
        wait_for_line(guest, SUSPEND_ENTRY, entries, "network-wake")
        hold_quietly(guest, exits, 2.0, "network-wake")
        sent = time.monotonic()
        connection.sendall(b"x")
        woke = wait_for_exit_line(
            guest, exits,
            "network-wake: the guest stayed in its own suspend after a frame "
            "arrived",
        )
        read_echo(guest, connection, 1)
        echoed = time.monotonic() - sent
        if echoed > NETWORK_WAKE_SECONDS:
            raise RuntimeError(
                f"network-wake: the byte was echoed {echoed:.2f} s after it "
                f"was sent, not within {NETWORK_WAKE_SECONDS:g} s"
            )
        reported = network_wakeups(guest) - wakeups
        if reported < 1:
            raise RuntimeError(
                "network-wake: the interface's wakeup source reported nothing "
                "for the frame that ended the suspend"
            )

        # The host's suspend, asked for right after a burst, holds under
        # traffic and is entered once: the burst's wakeup, still held, does
        # not fail it.
        connection.sendall(b"x" * 8)
        read_echo(guest, connection, 8)
        entries = guest.output().count(SUSPEND_ENTRY)
        sent = 0

        def send() -> None:
            nonlocal sent
            connection.sendall(b"x")
            sent += 1

        hold(guest, console, "network-wake", send)
        attempts = guest.output().count(SUSPEND_ENTRY) - entries
        if attempts != 1:
            raise RuntimeError(
                f"network-wake: the host's suspend took {attempts} attempts "
                "after a burst, not one\n" + guest.output()[request:][-4000:]
            )
        read_echo(guest, connection, sent)
        print(
            f"network-wake: a byte ended the guest's own suspend after "
            f"{woke:.2f} s and was echoed in {echoed:.2f} s "
            f"({reported} wakeup reported); the host's suspend after a burst "
            f"held under {sent} bytes in one attempt"
        )
        connection.close()
        require_clean(guest, "network-wake", request)
        if not guest.shutdown(60):
            raise RuntimeError("network-wake: the guest did not power off")


def run_console(arguments, private: pathlib.Path) -> None:
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "512M",
        "tmpfs",
        (f"ncpus={VCPUS}",),
        init_shell=True,
        engine=arguments.engine,
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        lines = 0

        def type_line() -> None:
            nonlocal lines
            guest._write(CONSOLE_LINE if lines == 0 else ":\n")
            lines += 1

        hold(guest, console, "console", type_line)
        output = guest.check_command("echo console-after-resume", 60)
        if CONSOLE_OUTPUT not in guest.output()[request:]:
            raise RuntimeError(
                "console: the line typed during the suspend did not run after "
                f"it:\n{output}"
            )
        print(f"console: the {lines} lines typed during the suspend ran after it")
        require_clean(guest, "console", request)
        if not guest.shutdown(60):
            raise RuntimeError("console: the guest did not power off")


def run_vhost_user(arguments, private: pathlib.Path) -> None:
    backend_end, guest_end = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    server = subprocess.Popen(
        [
            str(arguments.server.resolve()),
            "--fd", str(backend_end.fileno()),
            "--period-ms", str(CHURN_PERIOD_MS),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        pass_fds=(backend_end.fileno(),),
    )
    backend_end.close()
    output = b""
    try:
        try:
            guest = Guest(
                arguments.emulator,
                arguments.rootfs,
                "512M",
                "tmpfs",
                (
                    f"ncpus={VCPUS}",
                    f"virtio_uml.device=fd={guest_end.fileno()}:{CHURN_DEVICE}",
                ),
                init_shell=True,
                engine=arguments.engine,
                pass_fds=(guest_end.fileno(),),
            )
        finally:
            guest_end.close()
        with guest:
            guest.boot_to_shell(180)
            console = ManagementConsole(guest, private)
            request = len(guest.output())
            hold(guest, console, "vhost-user", lambda: None)
            server.send_signal(signal.SIGUSR1)
            guest.check_command("sleep 1; echo churn-after-resume", 60)
            require_clean(guest, "vhost-user", request)
            if not guest.shutdown(60):
                raise RuntimeError("the guest did not power off")
        output, _ = server.communicate(timeout=SERVER_EXIT_SECONDS)
        summary = CHURN_SUMMARY.search(output)
        mark = CHURN_MARK.search(output)
        if server.returncode != 0 or summary is None or mark is None:
            raise RuntimeError("the churn backend failed")
        at_resume, changes = int(mark.group(1)), int(summary.group(1))
        if changes - at_resume < CHURN_MINIMUM_CHANGES:
            raise RuntimeError(
                f"vhost-user: only {changes - at_resume} config changes were "
                "raised after the resume; the guest stopped acknowledging them"
            )
        print(
            f"vhost-user: {at_resume} config changes raised by the resume and "
            f"{changes - at_resume} after it"
        )
    except BaseException:
        if not output:
            try:
                output, _ = server.communicate(timeout=SERVER_EXIT_SECONDS)
            except subprocess.TimeoutExpired:
                server.kill()
                output, _ = server.communicate()
        print(output.decode(errors="replace")[-4000:], file=sys.stderr)
        raise


def run_early_wake(arguments, private: pathlib.Path) -> None:
    """A wake sent as soon as the suspend starts, while the console still
    answers it, must not leave the guest suspended."""
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "512M",
        "tmpfs",
        (f"ncpus={VCPUS}",),
        init_shell=True,
        engine=arguments.engine,
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        entries = guest.output().count(SUSPEND_ENTRY)
        exits = guest.output().count(SUSPEND_EXIT)
        suspend = console.send("suspend")
        try:
            deadline = time.monotonic() + REQUEST_SECONDS
            while guest.output().count(SUSPEND_ENTRY) == entries:
                if time.monotonic() > deadline:
                    raise RuntimeError("early-wake: the suspend never began")
                drain(guest, 0.01)
            answer = console.request("wake")
            if "running" not in answer:
                raise RuntimeError(f"early-wake: wake replied {answer!r}")
            woken = time.monotonic()
            # pm_suspend() prints its exit line however the suspend ends.
            # Nothing else reaches the guest meanwhile: console input would
            # end a suspend by itself on a kernel without the O_ASYNC fix.
            while guest.output().count(SUSPEND_EXIT) == exits:
                if time.monotonic() - woken > RESUME_SECONDS:
                    raise RuntimeError(
                        "early-wake: the guest stayed suspended after a wake "
                        "sent while it was entering the suspend:\n"
                        + guest.output()[request:][-4000:]
                    )
                drain(guest, POKE_SECONDS)
            running = time.monotonic() - woken
            _, suspended = console.collect(suspend)
        finally:
            suspend.close()
        guest.check_command("echo early-wake-running", 60)
        print(
            f"early-wake: the suspend ended {running:.2f} s after a wake sent "
            f"as it began; it replied {suspended!r}"
        )
        require_clean(guest, "early-wake", request)
        if not guest.shutdown(60):
            raise RuntimeError("early-wake: the guest did not power off")


def run_self_wake(arguments, private: pathlib.Path) -> None:
    """The power key's wake: an answer to a running guest, and the end of a
    suspend the guest entered or is still entering by itself."""
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}",),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={HOLDER: GuestFile(HOLDER_SOURCE.encode(), 0o755)},
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        answer = console.request("wake")
        if "running" not in answer:
            raise RuntimeError(f"self-wake: a running guest replied {answer!r}")

        # Nothing of that answer lingers: the guest's next suspend holds.
        exits = guest.output().count(SUSPEND_EXIT)
        entries = guest.output().count(SUSPEND_ENTRY)
        guest._write(SELF_SUSPEND)
        wait_for_line(guest, SUSPEND_ENTRY, entries, "self-wake")
        hold_quietly(guest, exits, HOLD_SECONDS, "self-wake")
        answer = console.request("wake")
        if "running" not in answer:
            raise RuntimeError(f"self-wake: wake replied {answer!r}")
        held = wait_for_exit_line(
            guest, exits,
            "self-wake: the guest stayed in its own suspend after a wake",
        )

        # A wake sent while the freezer still waits for the readers, long
        # before interrupts stop, makes the PM core give the suspend up.
        exits = guest.output().count(SUSPEND_EXIT)
        freezing = guest.output().count(SUSPEND_FREEZING)
        guest._write(f"python3 {HOLDER} {HOLDER_READERS} &\n")
        wait_for_line(guest, SUSPEND_FREEZING, freezing, "self-wake")
        answer = console.request("wake")
        if "running" not in answer:
            raise RuntimeError(f"self-wake: early wake replied {answer!r}")
        early = wait_for_exit_line(
            guest, exits,
            "self-wake: the guest stayed suspended after a wake sent while it "
            "was entering its own suspend",
        )
        wait_for_line(guest, "hold-freeze: ", 0, "self-wake")
        if HOLDER_REFUSED not in guest.output():
            raise RuntimeError(
                "self-wake: the wake reached the suspend only after it had "
                "been entered:\n" + guest.output()[-4000:]
            )
        guest.check_command("echo self-wake-running", 60)
        print(
            f"self-wake: a held suspend ended {held:.2f} s and one being "
            f"entered {early:.2f} s after a wake; a running guest only "
            "answered"
        )
        require_clean(guest, "self-wake", request)
        if not guest.shutdown(60):
            raise RuntimeError("self-wake: the guest did not power off")


def sync_guest(arguments, private: pathlib.Path, arm: str) -> Guest:
    """A guest with a scratch disk for the frozen file system."""
    disk = private / f"{arm}.img"
    with open(disk, "wb") as image:
        image.truncate(SYNC_DISK_MIB << 20)
    return Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}", f"ubd1={disk}"),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={HOLDER: GuestFile(HOLDER_SOURCE.encode(), 0o755)},
    )


def hold_sync(guest: Guest, arm: str) -> str:
    """Leaves dirty data that the next sync waits on; returns how much."""
    dirty = guest.check_command(SYNC_HOLD, 60).strip().splitlines()[-1]
    if int(dirty.split()[0]) < 8 << 10:
        raise RuntimeError(f"{arm}: only {dirty} stayed dirty")
    return dirty


def rtc_interrupts(guest: Guest) -> int:
    output = guest.check_command(RTC_INTERRUPTS, 30)
    return int(output.strip().splitlines()[-1])


def run_sync_wake(arguments, private: pathlib.Path) -> None:
    """A wake sent while the guest's own suspend still syncs file systems,
    before the PM core prepares it, ends that suspend."""
    with sync_guest(arguments, private, "sync-wake") as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        dirty = hold_sync(guest, "sync-wake")

        exits = guest.output().count(SUSPEND_EXIT)
        entries = guest.output().count(SUSPEND_ENTRY)
        syncs = guest.output().count(SYNC_DONE)
        freezing = guest.output().count(SUSPEND_FREEZING)
        guest._write(f"python3 {HOLDER} 0 &\n")
        wait_for_line(guest, SUSPEND_ENTRY, entries, "sync-wake")
        drain(guest, SYNC_SETTLE_SECONDS)
        answer = console.request("wake")
        if "running" not in answer:
            raise RuntimeError(f"sync-wake: wake replied {answer!r}")
        if guest.output().count(SYNC_DONE) != syncs:
            raise RuntimeError(
                "sync-wake: the file system sync ended before the wake "
                "arrived, so the arm tested nothing:\n"
                + guest.output()[request:][-4000:]
            )
        guest.check_command(f"fsfreeze -u {SYNC_HELD}", 30)
        early = wait_for_exit_line(
            guest, exits,
            "sync-wake: the guest stayed suspended after a wake sent while "
            "its own suspend synced file systems",
        )
        wait_for_line(guest, "hold-freeze: ", 0, "sync-wake")
        if HOLDER_REFUSED not in guest.output():
            raise RuntimeError(
                "sync-wake: the suspend ran despite a wake sent while it "
                "synced file systems:\n" + guest.output()[request:][-4000:]
            )
        if guest.output().count(SUSPEND_FREEZING) != freezing:
            raise RuntimeError(
                "sync-wake: the suspend went on to freeze tasks after a wake "
                "sent while it synced file systems:\n"
                + guest.output()[request:][-4000:]
            )

        # The wake ended that suspend only: the guest's next one holds.
        exits = guest.output().count(SUSPEND_EXIT)
        entries = guest.output().count(SUSPEND_ENTRY)
        guest._write(SELF_SUSPEND)
        wait_for_line(guest, SUSPEND_ENTRY, entries, "sync-wake")
        hold_quietly(guest, exits, 2.0, "sync-wake")
        answer = console.request("wake")
        if "running" not in answer:
            raise RuntimeError(f"sync-wake: wake replied {answer!r}")
        wait_for_exit_line(
            guest, exits,
            "sync-wake: the guest stayed in its next suspend after a wake",
        )
        guest.check_command("echo sync-wake-running", 60)
        print(
            f"sync-wake: a wake sent while a suspend synced {dirty} of dirty "
            f"data ended it {early:.2f} s after the sync was released, "
            "before it froze tasks; the next suspend held"
        )
        require_clean(guest, "sync-wake", request)
        if not guest.shutdown(60):
            raise RuntimeError("sync-wake: the guest did not power off")


def run_sync_alarm(arguments, private: pathlib.Path) -> None:
    """An RTC alarm that fires while a suspend the host asked for still
    syncs file systems ends that attempt; the guest enters the suspend
    again, replies that it is suspended, and holds until the host's wake."""
    with sync_guest(arguments, private, "sync-alarm") as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        dirty = hold_sync(guest, "sync-alarm")
        alarms = rtc_interrupts(guest)
        exits = guest.output().count(SUSPEND_EXIT)
        entries = guest.output().count(SUSPEND_ENTRY)
        syncs = guest.output().count(SYNC_DONE)
        suspend = console.send("suspend")
        try:
            wait_for_line(guest, SUSPEND_ENTRY, entries, "sync-alarm")
            drain(guest, SYNC_SETTLE_SECONDS)
            guest.check_command(f"echo +2 > {RTC_ALARM}", 30)
            deadline = time.monotonic() + REQUEST_SECONDS
            while rtc_interrupts(guest) == alarms:
                if time.monotonic() > deadline:
                    raise RuntimeError("sync-alarm: the RTC alarm never fired")
                drain(guest, POKE_SECONDS)
            if guest.output().count(SYNC_DONE) != syncs:
                raise RuntimeError(
                    "sync-alarm: the file system sync ended before the alarm "
                    "fired, so the arm tested nothing:\n"
                    + guest.output()[request:][-4000:]
                )
            # Not a command that waits for its status: the suspend entered
            # again freezes the shell soon after the sync it releases.
            guest._write(f"fsfreeze -u {SYNC_HELD}\n")
            error, answer = console.collect(suspend)
        finally:
            suspend.close()
        if error or "suspended" not in answer:
            raise RuntimeError(
                f"sync-alarm: the host's suspend replied {answer!r} after an "
                "RTC alarm fired while it synced file systems:\n"
                + guest.output()[request:][-4000:]
            )
        # The attempt the alarm ended prints its exit line before the guest
        # enters the suspend again.
        deadline = time.monotonic() + RESUME_SECONDS
        while guest.output().count(SUSPEND_EXIT) == exits:
            if time.monotonic() > deadline:
                raise RuntimeError(
                    "sync-alarm: the alarm did not end the first attempt, so "
                    "the arm tested nothing:\n"
                    + guest.output()[request:][-4000:]
                )
            drain(guest, 0.05)
        exits = guest.output().count(SUSPEND_EXIT)
        hold_quietly(guest, exits, 2.0, "sync-alarm")
        answer = console.request("wake")
        if "running" not in answer:
            raise RuntimeError(f"sync-alarm: wake replied {answer!r}")
        resumed = wait_for_exit_line(
            guest, exits,
            "sync-alarm: the guest stayed suspended after the host's wake",
        )
        guest.check_command("echo sync-alarm-running", 60)
        attempts = guest.output().count(SUSPEND_ENTRY) - entries
        if attempts != 2:
            raise RuntimeError(
                f"sync-alarm: the host's suspend took {attempts} attempts, "
                "not the one the alarm ended and one that held:\n"
                + guest.output()[request:][-4000:]
            )
        print(
            f"sync-alarm: an RTC alarm ended a host suspend syncing {dirty} "
            "of dirty data; the guest entered it again, held, and resumed "
            f"{resumed:.2f} s after the host's wake"
        )
        require_clean(guest, "sync-alarm", request)
        if not guest.shutdown(60):
            raise RuntimeError("sync-alarm: the guest did not power off")


def run_sync_host_wake(arguments, private: pathlib.Path) -> None:
    """The host's wake, sent while a suspend the host asked for still syncs
    file systems, ends that request without a second attempt."""
    with sync_guest(arguments, private, "sync-host-wake") as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        dirty = hold_sync(guest, "sync-host-wake")
        exits = guest.output().count(SUSPEND_EXIT)
        entries = guest.output().count(SUSPEND_ENTRY)
        syncs = guest.output().count(SYNC_DONE)
        freezing = guest.output().count(SUSPEND_FREEZING)
        suspend = console.send("suspend")
        try:
            wait_for_line(guest, SUSPEND_ENTRY, entries, "sync-host-wake")
            drain(guest, SYNC_SETTLE_SECONDS)
            answer = console.request("wake")
            if "running" not in answer:
                raise RuntimeError(f"sync-host-wake: wake replied {answer!r}")
            if guest.output().count(SYNC_DONE) != syncs:
                raise RuntimeError(
                    "sync-host-wake: the file system sync ended before the "
                    "wake arrived, so the arm tested nothing:\n"
                    + guest.output()[request:][-4000:]
                )
            guest.check_command(f"fsfreeze -u {SYNC_HELD}", 30)
            error, answer = console.collect(suspend)
        finally:
            suspend.close()
        if not error or not answer.startswith(SUSPEND_FAILED):
            raise RuntimeError(
                f"sync-host-wake: the host's suspend replied {answer!r} after "
                "the host's wake arrived while it synced file systems:\n"
                + guest.output()[request:][-4000:]
            )
        wait_for_exit_line(
            guest, exits,
            "sync-host-wake: the guest stayed suspended after the host's wake",
        )
        guest.check_command("echo sync-host-wake-running", 60)
        attempts = guest.output().count(SUSPEND_ENTRY) - entries
        if attempts != 1 or guest.output().count(SUSPEND_FREEZING) != freezing:
            raise RuntimeError(
                f"sync-host-wake: the guest made {attempts} attempts at the "
                "suspend after the host's wake, or froze tasks:\n"
                + guest.output()[request:][-4000:]
            )

        # The wake ended that request only: the host's next suspend holds.
        hold(guest, console, "sync-host-wake", lambda: None)
        print(
            "sync-host-wake: the host's wake ended its suspend syncing "
            f"{dirty} of dirty data with {answer!r} and no second attempt; "
            "the next one held"
        )
        require_clean(guest, "sync-host-wake", request)
        if not guest.shutdown(60):
            raise RuntimeError("sync-host-wake: the guest did not power off")


def run_power(arguments, private: pathlib.Path) -> None:
    """A power key forwarded through the VNC display wakes a guest that
    suspended itself, and reaches it exactly once."""
    port = unused_loopback_port()
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (
            f"ncpus={VCPUS}",
            "--gpu",
            "--gpu-display=vnc",
            f"--gpu-vnc-port={port}",
        ),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={KEY_READER: GuestFile(KEY_READER_SOURCE.encode(), 0o755)},
    ) as guest:
        guest.boot_to_shell(180)
        request = len(guest.output())
        guest.check_command(
            f"(python3 {KEY_READER} >/run/power-keys.err 2>&1 </dev/null &)",
            30,
        )
        deadline = time.monotonic() + REQUEST_SECONDS
        while "ready" not in guest.check_command(
            f"cat {KEY_LOG} 2>/dev/null; true", 30
        ):
            if time.monotonic() > deadline:
                raise RuntimeError(
                    "power: the key reader did not start:\n"
                    + guest.check_command("cat /run/power-keys.err; true", 30)
                )
            drain(guest, 0.2)
        keyboard = VncKeyboard(guest, port)
        try:
            exits = guest.output().count(SUSPEND_EXIT)
            entries = guest.output().count(SUSPEND_ENTRY)
            guest._write(SELF_SUSPEND)
            wait_for_line(guest, SUSPEND_ENTRY, entries, "power")
            hold_quietly(guest, exits, 2.0, "power")
            keyboard.type_key(XF86_POWER_OFF)
            woke = wait_for_exit_line(
                guest, exits,
                "power: the guest stayed in its own suspend after a power key",
            )
        finally:
            keyboard.close()
        # The emulator's VNC server reports the keyboard client's departure
        # on the console it shares with the guest, and that report can land
        # in this command's output. One printf writes the events as one line,
        # which reaches the console in one piece, and only that line is read.
        listed = guest.check_command(
            f"printf 'keys:%s:end\\n' \"$(tr '\\n' ' ' < {KEY_LOG})\"", 30
        )
        found = re.search(r"keys:([^:\r\n]*):end", listed)
        if found is None:
            raise RuntimeError(f"power: no key events in {listed!r}")
        keys = found.group(1).split()
        if keys != ["ready", "1", "0"]:
            raise RuntimeError(
                f"power: the guest received power key events {keys[1:]}, not "
                "one press and one release"
            )
        print(
            f"power: a power key woke the guest's own suspend in {woke:.2f} s "
            "and arrived once"
        )
        require_clean(guest, "power", request)
        if not guest.shutdown(60):
            raise RuntimeError("power: the guest did not power off")


def wait_for_answer(
    guest: Guest, request: Request, seconds: float
) -> tuple[int, str] | None:
    """The request's reply if one arrives within ``seconds``, draining the
    guest's console meanwhile; None when none does."""
    deadline = time.monotonic() + seconds
    while (answer := request.answer()) is None:
        if time.monotonic() >= deadline:
            return None
        drain(guest, 0.02)
    return answer


def run_freezer_hold(arguments, private: pathlib.Path) -> None:
    """A suspend whose freezer is held stays unanswered, and the host's abort
    is a wake: answered while the suspend blocks, and followed by the
    suspend's own failure on the request's socket."""
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}",),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={
            FREEZER_HOLDER: GuestFile(FREEZER_HOLDER_SOURCE.encode(), 0o755)
        },
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        request = len(guest.output())
        guest.check_command(
            f"(python3 -B {FREEZER_HOLDER} </dev/null >{FREEZER_HOLDER_LOG} "
            "2>&1 &)",
            30,
        )
        deadline = time.monotonic() + 30
        while "freeze-hold: held" not in guest.check_command(
            f"cat {FREEZER_HOLDER_LOG}", 30
        ):
            if time.monotonic() > deadline:
                raise RuntimeError(
                    "freezer-hold: the holder never held a request:\n"
                    + guest.check_command(f"cat {FREEZER_HOLDER_LOG}; true", 30)
                )
            drain(guest, 0.5)

        # The freezer cannot finish, so the suspend gives no reply at all.
        suspend = console.send("suspend")
        try:
            early = wait_for_answer(guest, suspend, FREEZER_SILENCE_SECONDS)
            if early is not None:
                raise RuntimeError(
                    f"freezer-hold: the suspend replied {early!r} within "
                    f"{FREEZER_SILENCE_SECONDS:g} s, so the freezer was not "
                    "held and the arm tested nothing:\n"
                    + guest.output()[request:][-4000:]
                )

            # The wake is answered although the suspend request still blocks
            # the console's handler.
            woken = time.monotonic()
            wake = console.send("wake")
            try:
                error, text = console.collect(wake)
            finally:
                wake.close()
            if error or "running" not in text:
                raise RuntimeError(
                    f"freezer-hold: wake replied {(error, text)!r}"
                )
            answered = time.monotonic() - woken

            # The aborted suspend reports its own end on its own socket.
            final = wait_for_answer(guest, suspend, FREEZER_FINAL_SECONDS)
            if final is None:
                raise RuntimeError(
                    "freezer-hold: the suspend gave no final reply within "
                    f"{FREEZER_FINAL_SECONDS:g} s of the wake:\n"
                    + guest.output()[request:][-4000:]
                )
            final_error, final_text = final
            reported = time.monotonic() - woken
            if not final_error or not final_text.startswith(
                SUSPEND_FAILED + "-16"
            ):
                raise RuntimeError(
                    f"freezer-hold: the aborted suspend replied {final!r}, "
                    f"not '{SUSPEND_FAILED}-16'"
                )
        finally:
            suspend.close()
        # The PM core says a wake ended the freezing, not the freezer's own
        # timeout, and the guest runs.
        counted = guest.check_command(
            f"echo freezer-aborted:$(dmesg | grep -c '{FREEZER_ABORTED}'):end",
            60,
        )
        found = re.search(r"freezer-aborted:(\d+):end", counted)
        if found is None or int(found[1]) < 1:
            raise RuntimeError(
                f"freezer-hold: dmesg has no {FREEZER_ABORTED!r} ({counted!r}):\n"
                + guest.output()[request:][-4000:]
            )
        guest.check_command("echo freezer-hold-running", 60)

        # Released, the freezer finishes: the next suspend holds and wakes.
        # Ending the holder closes /dev/fuse, which aborts the connection and
        # lets the stat child, stuck in an uninterruptible wait, finish.
        guest.check_command(
            f"pkill -9 -f {FREEZER_HOLDER}; sleep 1; "
            "umount -l /run/freeze-hold; true",
            60,
        )
        hold(guest, console, "freezer-hold", lambda: None)
        print(
            f"freezer-hold: no reply in {FREEZER_SILENCE_SECONDS:g} s, "
            f"the wake was answered in {answered * 1000:.0f} ms and the "
            f"suspend failed {reported * 1000:.0f} ms after it ({final_text})"
        )
        require_clean(guest, "freezer-hold", request)
        if not guest.shutdown(60):
            raise RuntimeError("freezer-hold: the guest did not power off")


ARMS = {
    "network": run_network,
    "console": run_console,
    "vhost-user": run_vhost_user,
    "early-wake": run_early_wake,
    "self-wake": run_self_wake,
    "sync-wake": run_sync_wake,
    "sync-alarm": run_sync_alarm,
    "sync-host-wake": run_sync_host_wake,
    "power": run_power,
    "network-wake": run_network_wake,
    "freezer-hold": run_freezer_hold,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--server", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--arm", action="append", choices=tuple(ARMS))
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-suspend-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        for arm in arguments.arm or tuple(ARMS):
            ARMS[arm](arguments, private)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
