#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host-side statistics of a running Linux guest, as --stats prints them.

Boots a two-vCPU guest with the offscreen GPU display and
`--stats=500`, which has the launcher read the guest once per interval
through the public calls the iOS applications make and print one line:

    uml-darwin: stats <s> s cpu <n>% of <v> vCPUs frames <p>/s presented
    <o>/s offered [memory <u> MiB in use of <t> MiB] held <h> of <r> MiB
    [gpu-refusals <n>] [gpu-memory <u>/<b> MiB [attributed <a> MiB
    [holders <pid>:<name>:<h> MiB:<i> s ...]]]

The launcher's standard error goes to a file instead of the console, so the
lines neither wait for the harness to drain the console nor land inside a
command's markers; a poller stamps each line as it arrives.

- CPU, idle: under 5% of the guest's vCPUs.
- CPU, one busy loop: the line's share agrees with the same carriers'
  CPU time as libproc reports it (within 5 points), and does not exceed what
  the guest's own /proc/stat counted as busy by more than 5. The guest counts
  the time the host took away from a vCPU as busy, so on a loaded host the
  host figure is the lower one and the guest cannot bound it from below.
- Frames: a display nobody draws on presents 0 frames a second, and one a
  guest writes to presents some.
- Memory: the guest's own kernel answers the `mem` console request, so the
  line must show the guest's MemTotal in whole MiB and its MemTotal less
  MemAvailable within 16 MiB, as the launcher's reply parser read them.
  A kernel without the request is a failure here; for the launcher, which
  must show no memory part for such a kernel, a console of the test's own
  that refuses every request stands where the guest's was: the launcher must
  send only the datagram `mem`, in the console's request layout, and show no
  memory part. macOS only.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import dataclasses
import os
import pathlib
import re
import shutil
import socket
import stat
import struct
import sys
import tempfile
import threading
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from guest import Guest  # noqa: E402
from run_guest_thread_names import (  # noqa: E402
    PROC_PIDLISTTHREADS,
    PROC_PIDTHREADINFO,
    ThreadInfo,
)

import ctypes  # noqa: E402


VCPUS = 2
INTERVAL_MS = 500
MIB = 1 << 20
# How far the launcher's CPU share may be from an independent measurement of
# the same carriers, and from the guest's own count.
CPU_TOLERANCE_POINTS = 5.0
IDLE_CPU_LIMIT = 5.0
MEMORY_TOLERANCE_MIB = 16
STATIC_SETTLE_SECONDS = 3.0
WINDOW_SECONDS = 8.0
LINE_WAIT_SECONDS = 30.0
MCONSOLE_MAGIC = 0xCAFEBABE
MCONSOLE_VERSION = 2
MCONSOLE_DATA = 512
STATS_LINE = re.compile(
    r"uml-darwin: stats (?P<elapsed>\d+\.\d) s "
    r"cpu (?P<cpu>\d+\.\d)% of (?P<vcpus>\d+) vCPUs "
    r"frames (?P<presented>\d+\.\d)/s presented (?P<offered>\d+\.\d)/s offered"
    r"(?: memory (?P<used>\d+) MiB in use of (?P<total>\d+) MiB)?"
    r"(?: held (?P<held>\d+) of (?P<ram>\d+) MiB)?"
    r"(?: gpu-refusals (?P<refusals>\d+))?"
    r"(?: gpu-memory (?P<gpuused>\d+)/(?P<gpubudget>\d+) MiB"
    r"(?: attributed (?P<gpuheld>\d+) MiB(?P<holders>(?: holders.*?)?))?)?"
    r"(?P<reclaim> guest-reclaim=on)?$"
)
CARRIER = re.compile(r"^uml-vcpu-\d+-carrier$")
WRAPPER = """\
#!/bin/sh
# Keeps the launcher's standard error out of the harness's console.
exec "$UML_STATS_EMULATOR" "$@" 2>>"$UML_STATS_LOG"
"""


@dataclasses.dataclass(frozen=True)
class Line:
    arrived: float
    text: str
    cpu: float
    vcpus: int
    presented: float
    offered: float
    used: int | None
    total: int | None
    held: int | None
    ram: int | None


class StatsLog:
    """Follows the file the launcher's standard error goes to and stamps every
    --stats line with the time it was first seen, within the poll interval."""

    POLL_SECONDS = 0.02

    def __init__(self, path: pathlib.Path) -> None:
        self._path = path
        self._lines: list[Line] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._text = ""
        self._thread = threading.Thread(target=self._follow, daemon=True)

    def __enter__(self) -> "StatsLog":
        self._thread.start()
        return self

    def __exit__(self, *_: object) -> None:
        self._stop.set()
        self._thread.join(10)

    def _follow(self) -> None:
        offset = 0
        pending = b""
        while not self._stop.wait(self.POLL_SECONDS):
            try:
                with self._path.open("rb") as stream:
                    stream.seek(offset)
                    chunk = stream.read()
            except FileNotFoundError:
                continue
            if not chunk:
                continue
            arrived = time.monotonic()
            offset += len(chunk)
            pending += chunk
            *complete, pending = pending.split(b"\n")
            with self._lock:
                for raw in complete:
                    text = raw.decode(errors="replace").strip()
                    self._text += text + "\n"
                    match = STATS_LINE.search(text)
                    if match is None:
                        continue
                    number = lambda name: (  # noqa: E731
                        int(match[name]) if match[name] is not None else None
                    )
                    self._lines.append(
                        Line(
                            arrived,
                            text,
                            float(match["cpu"]),
                            int(match["vcpus"]),
                            float(match["presented"]),
                            float(match["offered"]),
                            number("used"),
                            number("total"),
                            number("held"),
                            number("ram"),
                        )
                    )

    def lines(self) -> list[Line]:
        with self._lock:
            return list(self._lines)

    def text(self) -> str:
        with self._lock:
            return self._text

    def window(self, start: float, end: float) -> list[Line]:
        """The lines whose whole interval lies inside [start, end]: a line
        arrives when its interval ends, and the interval is INTERVAL_MS long."""
        interval = INTERVAL_MS / 1000
        # Lines can be late (a sampler that waited on the guest's memory), but
        # never early, so this keeps only intervals that began after `start`.
        return [
            line
            for line in self.lines()
            if line.arrived - interval - 0.1 >= start and line.arrived <= end + 0.1
        ]

    def wait_for(self, predicate, seconds: float, what: str) -> Line:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for line in reversed(self.lines()):
                if predicate(line):
                    return line
            time.sleep(0.05)
        raise RuntimeError(f"no stats line {what} in {seconds:g} s:\n"
                           + self.text()[-2000:])


def carrier_nanoseconds(process_id: int) -> tuple[int, int]:
    """The carriers' threads and the CPU time they have used, as libproc
    reports it."""
    libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    handles = (ctypes.c_uint64 * 4096)()
    size = libproc.proc_pidinfo(
        process_id, PROC_PIDLISTTHREADS, 0, handles, ctypes.sizeof(handles)
    )
    if size <= 0:
        raise RuntimeError(f"cannot list the threads of {process_id}")
    count = 0
    total = 0
    for handle in handles[: size // ctypes.sizeof(ctypes.c_uint64)]:
        if not handle:
            continue
        info = ThreadInfo()
        if libproc.proc_pidinfo(
            process_id,
            PROC_PIDTHREADINFO,
            ctypes.c_uint64(handle),
            ctypes.byref(info),
            ctypes.sizeof(info),
        ) != ctypes.sizeof(info):
            continue  # the thread ended meanwhile
        if CARRIER.match(info.pth_name.decode(errors="replace")):
            count += 1
            total += info.pth_user_time + info.pth_system_time
    return count, total


def guest_cpu(guest: Guest) -> tuple[int, int]:
    """The guest's busy and total jiffies from /proc/stat's first line."""
    output = guest.check_command("head -n 1 /proc/stat", 30)
    fields = [int(field) for field in output.split()[1:]]
    # user nice system idle iowait irq softirq steal
    idle = fields[3] + fields[4]
    return sum(fields[:8]) - idle, sum(fields[:8])


def mean(values: list[float]) -> float:
    return sum(values) / len(values)


class FakeConsole:
    """A management console of the test's own, bound where the guest's was.

    It refuses every request the way a kernel without the `mem` request
    does, with the error flag and "Unknown command", and keeps the requests
    it received, decoded from the console's layout."""

    def __init__(self, path: pathlib.Path) -> None:
        path.unlink()  # the guest's own socket; its binding is left behind
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self._socket.bind(str(path))
        self._socket.settimeout(0.05)
        self.requests: list[tuple[int, int, bytes]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        self._thread.join(10)
        self._socket.close()

    def _serve(self) -> None:
        text = b"Unknown command"
        while not self._stop.is_set():
            try:
                request, client = self._socket.recvfrom(4096)
            except socket.timeout:
                continue
            magic, version, length = struct.unpack_from("<III", request)
            self.requests.append(
                (magic, version, request[12:12 + length]) if len(request) >= 12
                else (0, 0, b"")
            )
            reply = struct.pack("<III", 1, 0, len(text)) + text.ljust(
                MCONSOLE_DATA, b"\0"
            )
            self._socket.sendto(reply, client)


def run_directory(process_id: int, private: pathlib.Path) -> pathlib.Path:
    directory = private / f"uml-kern{process_id}"
    if not (directory / "mconsole").is_socket():
        raise RuntimeError(
            f"the guest's management console is not {directory}/mconsole"
        )
    return directory


def check_idle(log: StatsLog, guest: Guest) -> None:
    """A display nobody draws on presents nothing, and an idle guest is idle."""
    guest.check_command(
        "echo 0 > /sys/class/graphics/fbcon/cursor_blink; "
        "printf '\\033[?25l' > /dev/tty0",
        30,
    )
    time.sleep(STATIC_SETTLE_SECONDS)
    start = time.monotonic()
    time.sleep(WINDOW_SECONDS)
    lines = log.window(start, time.monotonic())
    if len(lines) < 8:
        raise RuntimeError(f"only {len(lines)} stats lines in the idle window")
    for line in lines:
        if line.vcpus != VCPUS:
            raise RuntimeError(f"{line.vcpus} vCPUs reported, not {VCPUS}:\n{line.text}")
        if line.cpu >= IDLE_CPU_LIMIT:
            raise RuntimeError(f"an idle guest used {line.cpu}% of its vCPUs:\n{line.text}")
        if line.presented != 0.0:
            raise RuntimeError(f"a static display presented frames:\n{line.text}")
        if line.ram is None or line.held is None or not 0 < line.held <= line.ram:
            raise RuntimeError(f"the guest's RAM was not reported:\n{line.text}")
    print(
        f"idle: {len(lines)} lines, cpu {mean([l.cpu for l in lines]):.2f}% "
        f"(limit {IDLE_CPU_LIMIT}), 0 frames/s"
    )


def check_animated(log: StatsLog, guest: Guest) -> None:
    """A guest that writes to its display presents frames."""
    guest.check_command(
        "(i=0; while [ $i -lt 120 ]; do echo \"frame $i\" > /dev/tty0; "
        "sleep 0.1; i=$((i+1)); done) >/dev/null 2>&1 & echo started",
        30,
    )
    time.sleep(1.0)
    start = time.monotonic()
    time.sleep(6.0)
    lines = log.window(start, time.monotonic())
    if len(lines) < 6:
        raise RuntimeError(f"only {len(lines)} stats lines in the animated window")
    rate = mean([line.presented for line in lines])
    if rate < 1.0:
        raise RuntimeError(
            f"a display the guest writes to presented {rate:.2f} frames/s:\n"
            + "\n".join(line.text for line in lines)
        )
    print(f"animated: {len(lines)} lines, {rate:.1f} frames/s presented")
    # The writer runs for twelve seconds; let it end before the next phase.
    time.sleep(6.0)
    log.wait_for(lambda line: line.presented == 0.0, LINE_WAIT_SECONDS,
                 "with the display static again")


def check_busy(log: StatsLog, guest: Guest) -> None:
    """One busy loop is half of two vCPUs."""
    guest.check_command("sh -c 'while :; do :; done' & echo $! > /tmp/busy.pid", 30)
    time.sleep(2.0)
    busy_before, total_before = guest_cpu(guest)
    start = time.monotonic()
    carriers_before, host_before = carrier_nanoseconds(guest.process_id)
    host_began = time.monotonic()
    time.sleep(WINDOW_SECONDS)
    host_ended = time.monotonic()
    carriers_after, host_after = carrier_nanoseconds(guest.process_id)
    end = time.monotonic()
    busy_after, total_after = guest_cpu(guest)
    guest.check_command("kill $(cat /tmp/busy.pid)", 30)
    if carriers_before != VCPUS or carriers_after != VCPUS:
        raise RuntimeError(f"libproc found {carriers_after} carriers, not {VCPUS}")
    lines = log.window(start, end)
    if len(lines) < 8:
        raise RuntimeError(f"only {len(lines)} stats lines in the busy window")
    reported = mean([line.cpu for line in lines])
    libproc = (
        100.0
        * (host_after - host_before)
        / ((host_ended - host_began) * 1e9 * VCPUS)
    )
    counted = 100.0 * (busy_after - busy_before) / (total_after - total_before)
    print(
        f"busy: {len(lines)} lines, reported {reported:.1f}%, "
        f"libproc {libproc:.1f}%, guest /proc/stat {counted:.1f}%"
    )
    if abs(reported - libproc) > CPU_TOLERANCE_POINTS:
        raise RuntimeError(
            f"the reported share {reported:.1f}% is not libproc's {libproc:.1f}% "
            f"of the same carriers (within {CPU_TOLERANCE_POINTS} points)"
        )
    if reported > counted + CPU_TOLERANCE_POINTS:
        raise RuntimeError(
            f"the reported share {reported:.1f}% exceeds the guest's own "
            f"count {counted:.1f}% by more than {CPU_TOLERANCE_POINTS} points"
        )
    if reported < IDLE_CPU_LIMIT:
        raise RuntimeError(f"a busy loop shows only {reported:.1f}%")


def meminfo_mib(guest: Guest) -> tuple[int, int]:
    """The guest's MemTotal, in whole MiB, and its MemTotal less MemAvailable,
    in MiB, from /proc/meminfo."""
    output = guest.check_command(
        "grep -E '^Mem(Total|Available):' /proc/meminfo", 30
    )
    values = {
        name: int(value)
        for name, value in re.findall(r"^(MemTotal|MemAvailable):\s+(\d+) kB",
                                      output, re.MULTILINE)
    }
    if len(values) != 2:
        raise RuntimeError(f"the guest did not report its memory:\n{output}")
    return (values["MemTotal"] // 1024,
            (values["MemTotal"] - values["MemAvailable"]) // 1024)


def check_real_memory(log: StatsLog, guest: Guest) -> None:
    """The guest's kernel answers `mem`, and the launcher shows the figures
    its reply parser read: MemTotal exactly, and the memory in use as
    /proc/meminfo has it."""
    log.wait_for(
        lambda l: l.used is not None, LINE_WAIT_SECONDS,
        "with the guest's memory (a kernel without the `mem` request?)"
    )
    # The launcher reads the memory with every second sample, and a guest that
    # was just busy still moves it: judge only the lines that came after the
    # guest had settled.
    began = time.monotonic()
    time.sleep(3.0)
    total, used = meminfo_mib(guest)
    lines = [
        line for line in log.lines()
        if line.used is not None and line.arrived >= began
    ]
    if not lines:
        raise RuntimeError("no line with the guest's memory in 3 s")
    for line in lines:
        if line.total != total:
            raise RuntimeError(
                f"the line shows {line.total} MiB of memory but MemTotal is "
                f"{total} MiB:\n{line.text}"
            )
        if abs(line.used - used) > MEMORY_TOLERANCE_MIB:
            raise RuntimeError(
                f"the guest's memory in use is {line.used} MiB in the line but "
                f"{used} MiB in /proc/meminfo (within {MEMORY_TOLERANCE_MIB}):\n"
                + line.text
            )
    print(f"memory: {line.used} MiB in use of {line.total} MiB, "
          f"/proc/meminfo {used} of {total} MiB")


def check_old_kernel_console(
    log: StatsLog, guest: Guest, private: pathlib.Path
) -> None:
    """Where the console refuses `mem`, as a kernel without it does, the
    launcher sends the one datagram in the console's layout and shows no
    memory part."""
    directory = run_directory(guest.process_id, private)
    console = FakeConsole(directory / "mconsole")
    try:
        # The launcher reads the memory with every second sample, so the
        # lines between lack the part whatever the kernel says: the test
        # waits for the refused request and judges the line after it.
        deadline = time.monotonic() + LINE_WAIT_SECONDS
        while not console.requests and time.monotonic() < deadline:
            time.sleep(0.05)
        if not console.requests:
            raise RuntimeError(
                f"the refusing console received no request in "
                f"{LINE_WAIT_SECONDS:g} s"
            )
        asked = time.monotonic()
        time.sleep(WINDOW_SECONDS / 2)
        later = [line for line in log.lines() if line.arrived > asked]
        if len(later) < 6 or len(console.requests) < 3:
            raise RuntimeError(
                f"{len(later)} lines and {len(console.requests)} requests in "
                f"{WINDOW_SECONDS / 2:g} s of a console that refuses `mem`"
            )
        for line in later:
            if line.used is not None:
                raise RuntimeError(
                    f"a memory part from a console that refuses `mem`:\n"
                    + line.text
                )
        print(f"refusing console: {len(later)} lines without a memory part, "
              f"{len(console.requests)} requests refused")
        for magic, version, data in console.requests:
            if (magic, version, data) != (MCONSOLE_MAGIC, MCONSOLE_VERSION, b"mem"):
                raise RuntimeError(
                    f"the launcher sent {(hex(magic), version, data)!r}, "
                    f"not {(hex(MCONSOLE_MAGIC), MCONSOLE_VERSION, b'mem')!r}"
                )
    finally:
        console.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="gadgets")
    arguments = parser.parse_args()

    # The console's run directory is in $TMPDIR; a private one tells where.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-stats-"))
    try:
        wrapper = private / "emulator"
        wrapper.write_text(WRAPPER)
        wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)
        log_path = private / "launcher.log"
        environment = dict(os.environ)
        environment.update(
            TMPDIR=str(private),
            UML_STATS_EMULATOR=str(arguments.emulator.resolve()),
            UML_STATS_LOG=str(log_path),
        )
        with StatsLog(log_path) as log, Guest(
            wrapper,
            arguments.rootfs.resolve(),
            "1G",
            "tmpfs",
            (
                f"ncpus={VCPUS}",
                f"--stats={INTERVAL_MS}",
                "--gpu",
                "--gpu-display=offscreen",
            ),
            init_shell=True,
            engine=arguments.engine,
            environment=environment,
        ) as guest:
            guest.boot_to_shell(180)
            log.wait_for(lambda line: line.vcpus == VCPUS, LINE_WAIT_SECONDS,
                         f"with {VCPUS} vCPUs")
            check_idle(log, guest)
            check_animated(log, guest)
            check_busy(log, guest)
            check_real_memory(log, guest)
            check_old_kernel_console(log, guest, private)
            if not guest.shutdown(60):
                raise RuntimeError("the guest did not power off within 60 s")
        print("the host-side statistics agree with the guest")
        return 0
    finally:
        shutil.rmtree(private, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
