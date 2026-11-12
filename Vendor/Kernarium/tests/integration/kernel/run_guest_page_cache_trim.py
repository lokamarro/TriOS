#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host decides when a guest trims the page cache it already caches.

page_cache_trim reclaims inactive file pages above a floor, 1/32 of RAM,
because the host already caches every guest disk. A guest in front of its
user pays a host read for every evicted page it reads again, so trimming is
off until the host asks through the management console. A program that is
starting reads files faster than it touches their pages a second time, and
a trimmer that took those pages made it read each one back, from the host
and, on the EROFS root, through a decompression; Chromium's start-up read
back about 20,000 pages in 15 s. A trimming guest therefore backs off while
what it reclaimed is read back, and resumes once that stops.

One 2 GiB guest reads the root disk's block device, whose page cache is file
cache like any other, in phases:

- off by default: 256 MiB read once must stay cached for five intervals,
  and page_cache_trim.enabled must read N;
- ``trim once``: the cache must be at the floor when the reply comes, and
  the reply must report what was trimmed and released to the host;
- ``trim on``: the parameter must read Y, and 256 MiB read once must shrink
  to the floor. Then every 2 s the guest reads a new 24 MiB stretch and
  reads again the stretch it read 6 s earlier. Once the trimmer has seen its
  evictions come back, the second reads must find their pages: what is read
  back in the last eight steps must stay under an eighth of what they read
  again. Without the back-off about a third came back. Once the reads stop,
  the cache must shrink to the floor again within the longest back-off;
- ``trim off``: 256 MiB of the image's largest files is copied a page at a
  time into a tmpfs file, so that their cache lies between pages still in
  use, in pieces smaller than the chunks the host takes back. ``trim once``
  must still release at least half of what it trims, which takes compaction:
  without it 30 to 40% came back. This comes after the periodic phases,
  whose back-off the guest's reading back of what the pass took would sway.
  Then 256 MiB read once must stay cached again, and a command the trimmer
  does not know must be refused.

The contract is the guest RAM paragraph of kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import socket
import struct
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


MEMORY = "2G"
MIB = 1 << 20
COLD_MIB = 256
# Each cold read takes a stretch of its own, past every stretch the working-set
# phase reads: a page evicted once and read again counts as a refault and goes
# straight to the active list, where it is no longer cold.
COLD_OFFSET_MIB = {"at boot": 1024, "on": 1280, "off": 1536}
# Cache of files on the image read between pages of a tmpfs file.
FRAGMENT_MIB = 256
FRAGMENT_FILES = "find /usr -xdev -type f -size +8M"
FILL = "/mnt/trim-fill"
READ_AHEAD = "/sys/block/ubda/queue/read_ahead_kb"
CHUNK_MIB = 24
LAG_STEPS = 3
STEPS = 15
STEP_SECONDS = 2
# The working set is judged over the steps after the trimmer has had time to
# see its first evictions come back.
JUDGED_FROM_STEP = 7
# Within this much of the floor counts as trimmed.
SLACK_MIB = 16
COLD_SECONDS = 60
# Five intervals of 2 s, in which a trimming guest takes 32 MiB each.
UNTRIMMED_SECONDS = 10
# The longest back-off is 32 intervals of 2 s, and a pass after it.
RESUME_SECONDS = 100
REQUEST_SECONDS = 60.0
MCONSOLE_MAGIC = 0xCAFEBABE
MCONSOLE_VERSION = 2
MCONSOLE_DATA = 512
TRIMMED = re.compile(
    r"^trimmed (\d+) KiB in \d+ ms, compacted in (\d+) ms, "
    r"released (\d+) KiB in \d+ ms$"
)
FAILURE = re.compile(
    r"WARNING:|BUG:|Oops|Call Trace:|Kernel panic|Darwin UML panic|"
    r"Darwin POSIX|Kernel mode fault"
)
STATE = (
    "inactive() { awk '/^Inactive\\(file\\):/ { print $2 }' /proc/meminfo; }; "
    "refaults() { awk '/^workingset_refault_file / { print $2 }' "
    "/proc/vmstat; }; "
)
ENABLED = "/sys/module/page_cache_trim/parameters/enabled"


class ManagementConsole:
    """Requests to the guest's management console, each answered at a socket
    of its own, that keep the guest's serial console drained meanwhile."""

    def __init__(self, guest: Guest, private: pathlib.Path) -> None:
        self._guest = guest
        self._directory = private / f"uml-kern{guest.process_id}"
        self._sent = 0

    def request(self, command: str) -> tuple[int, str]:
        console = self._directory / "mconsole"
        if not console.is_socket():
            raise RuntimeError(f"the guest's management console is not {console}")
        self._sent += 1
        reply = self._directory / f"mc-trim-{self._sent}"
        reply.unlink(missing_ok=True)
        endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            endpoint.bind(str(reply))
            data = command.encode()
            endpoint.sendto(
                struct.pack("<III", MCONSOLE_MAGIC, MCONSOLE_VERSION, len(data))
                + data.ljust(MCONSOLE_DATA, b"\0"),
                str(console),
            )
            endpoint.setblocking(False)
            deadline = time.monotonic() + REQUEST_SECONDS
            while True:
                try:
                    answer = endpoint.recv(12 + MCONSOLE_DATA)
                    break
                except BlockingIOError:
                    pass
                if time.monotonic() > deadline:
                    raise RuntimeError(
                        f"mconsole {command}: no reply in {REQUEST_SECONDS:g} s"
                    )
                if self._guest.wait_for_exit(0.05):
                    raise RuntimeError(
                        f"the guest exited with status {self._guest.return_code}:\n"
                        + self._guest.output()[-4000:]
                    )
        finally:
            endpoint.close()
            reply.unlink(missing_ok=True)
        error, _, length = struct.unpack_from("<III", answer)
        return error, answer[12:12 + length].decode(errors="replace").rstrip("\0")

    def accepted(self, command: str) -> str:
        error, text = self.request(command)
        if error:
            raise RuntimeError(f"mconsole {command} was refused: {text!r}")
        return text


def number(output: str, name: str) -> int:
    match = re.search(rf"^{name}=(\d+)$", output, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"the guest did not report {name}:\n{output}")
    return int(match.group(1))


def inactive(guest: Guest) -> int:
    return number(guest.check_command(STATE + "echo INACTIVE=$(inactive)", 30),
                  "INACTIVE")


def enabled(guest: Guest) -> str:
    return guest.check_command(f"cat {ENABLED}", 30).strip()


def read_cold(guest: Guest, phase: str) -> int:
    """Read COLD_MIB once and return the inactive file cache after it."""
    guest.check_command(
        f"dd if=/dev/ubda of=/dev/null bs=1M count={COLD_MIB} "
        f"skip={COLD_OFFSET_MIB[phase]} 2>/dev/null",
        120,
    )
    return inactive(guest)


def require_untrimmed(guest: Guest, phase: str) -> None:
    """Cold cache read now must still be cached UNTRIMMED_SECONDS later."""
    cached = read_cold(guest, phase)
    later = number(
        guest.check_command(
            STATE + f"sleep {UNTRIMMED_SECONDS}; echo INACTIVE=$(inactive)",
            UNTRIMMED_SECONDS + 60,
        ),
        "INACTIVE",
    )
    if cached - later > 2 * SLACK_MIB * 1024:
        raise RuntimeError(
            f"{phase}: the inactive file cache fell from {cached} KiB to "
            f"{later} KiB in {UNTRIMMED_SECONDS} s with trimming off"
        )


def wait_until_trimmed(
    guest: Guest, floor_kib: int, seconds: int, phase: str
) -> int:
    """Return how long the inactive file cache took to reach the floor."""
    limit = floor_kib + SLACK_MIB * 1024
    output = guest.check_command(
        STATE
        + f"waited=0; while [ \"$(inactive)\" -gt {limit} ] && "
        f"[ $waited -lt {seconds} ]; do sleep 1; waited=$((waited + 1)); "
        "done; echo WAITED=$waited; echo INACTIVE=$(inactive)",
        seconds + 60,
    )
    cached = number(output, "INACTIVE")
    if cached > limit:
        raise RuntimeError(
            f"{phase}: the inactive file cache stayed at {cached} KiB, "
            f"above the {floor_kib} KiB floor, for {seconds} s"
        )
    return number(output, "WAITED")


def pass_amounts(reply: str) -> tuple[int, int, int]:
    """KiB trimmed, milliseconds compacting and KiB released by one pass."""
    match = TRIMMED.match(reply)
    if match is None:
        raise RuntimeError(f"trim once replied {reply!r}")
    return int(match[1]), int(match[2]), int(match[3])


def trim_once(guest: Guest, console: ManagementConsole, floor_kib: int) -> str:
    """One pass must leave the floor behind before it replies. The cache
    read at boot is still there."""
    cached = inactive(guest)
    started = time.monotonic()
    reply = console.accepted("trim once")
    took = time.monotonic() - started
    trimmed, _, released = pass_amounts(reply)
    left = inactive(guest)
    if left > floor_kib + SLACK_MIB * 1024:
        raise RuntimeError(
            f"trim once left {left} KiB of inactive file cache of {cached} "
            f"KiB, above the {floor_kib} KiB floor"
        )
    if trimmed < (cached - floor_kib) // 2 or released * 2 < trimmed:
        raise RuntimeError(
            f"trim once took {cached} KiB of inactive file cache to "
            f"{left} KiB but replied {reply!r}"
        )
    return (
        f"trim once took {trimmed // 1024} MiB, released "
        f"{released // 1024} MiB in {took:.2f} s"
    )


def trim_fragmented(
    guest: Guest, console: ManagementConsole, page: int
) -> str:
    """Files copied a page at a time into a tmpfs file, which stays in use,
    leave cache that frees memory only in pieces smaller than the host's
    chunks. File cache and tmpfs pages are both movable, so they take turns
    at the same free list, and without readahead each read caches only its
    own page; the block device's own cache would come from blocks of its own.
    The pass must still hand most of what it trims to the host."""
    output = guest.check_command(
        f"mkdir -p {FILL} && mount -t tmpfs tmpfs {FILL} && "
        f"ra=$(cat {READ_AHEAD}) && echo 0 > {READ_AHEAD} && "
        f"left={FRAGMENT_MIB * MIB // page}; "
        f"for f in $({FRAGMENT_FILES} 2>/dev/null); do "
        f"n=$(($(stat -c %s $f) / {page})); "
        "if [ $n -gt $left ]; then n=$left; fi; "
        f"dd if=$f bs={page} count=$n 2>/dev/null >> {FILL}/fill; "
        "left=$((left - n)); if [ $left -le 0 ]; then break; fi; done; "
        f"echo $ra > {READ_AHEAD}; echo LEFT=$left",
        300,
    )
    if number(output, "LEFT") > 0:
        raise RuntimeError(
            f"the image has less than {FRAGMENT_MIB} MiB in files of "
            f"{FRAGMENT_FILES!r}"
        )
    reply = console.accepted("trim once")
    trimmed, compacted_ms, released = pass_amounts(reply)
    guest.check_command(f"umount {FILL}", 60)
    if trimmed < FRAGMENT_MIB * 1024 // 2 or released * 2 < trimmed:
        raise RuntimeError(
            f"after {FRAGMENT_MIB} MiB of file cache read between pages of a "
            f"tmpfs file, trim once replied {reply!r}"
        )
    return (
        f"between tmpfs pages it took {trimmed // 1024} MiB and released "
        f"{released // 1024} MiB after {compacted_ms} ms of compaction"
    )


def working_set(guest: Guest, page: int) -> tuple[int, int]:
    """Bytes read back of bytes read again in the judged steps."""
    output = guest.check_command(
        STATE
        + f"k=0; while [ $k -lt {STEPS} ]; do "
        f"dd if=/dev/ubda of=/dev/null bs=1M count={CHUNK_MIB} "
        f"skip=$((k * {CHUNK_MIB})) 2>/dev/null; "
        f"if [ $k -ge {LAG_STEPS} ]; then "
        f"dd if=/dev/ubda of=/dev/null bs=1M count={CHUNK_MIB} "
        f"skip=$(((k - {LAG_STEPS}) * {CHUNK_MIB})) 2>/dev/null; fi; "
        "echo STEP$k=$(refaults); "
        f"sleep {STEP_SECONDS}; k=$((k + 1)); done",
        STEPS * (STEP_SECONDS + 30),
    )
    before = number(output, f"STEP{JUDGED_FROM_STEP - 1}")
    after = number(output, f"STEP{STEPS - 1}")
    read_back = (after - before) * page
    read_again = (STEPS - JUDGED_FROM_STEP) * CHUNK_MIB * MIB
    if read_back * 8 >= read_again:
        raise RuntimeError(
            f"the trimmer kept evicting a working set in use: "
            f"{read_back // MIB} MiB of {read_again // MIB} MiB read "
            f"again in steps {JUDGED_FROM_STEP}-{STEPS - 1} came back "
            "from the disk"
        )
    return read_back, read_again


def run(
    emulator: pathlib.Path, rootfs: pathlib.Path, engine: str,
    private: pathlib.Path
) -> None:
    with Guest(
        emulator,
        rootfs,
        MEMORY,
        "tmpfs",
        ("ncpus=2",),
        init_shell=True,
        engine=engine,
    ) as guest:
        guest.boot_to_shell(180)
        console = ManagementConsole(guest, private)
        identity = guest.check_command(
            "echo PAGE=$(getconf PAGESIZE); "
            "echo TOTAL=$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)",
            30,
        )
        page = number(identity, "PAGE")
        floor_kib = number(identity, "TOTAL") // 32

        require_untrimmed(guest, "at boot")
        if enabled(guest) != "N":
            raise RuntimeError(f"{ENABLED} reads {enabled(guest)!r} at boot")
        once = trim_once(guest, console, floor_kib)

        console.accepted("trim on")
        if enabled(guest) != "Y":
            raise RuntimeError(f"{ENABLED} reads {enabled(guest)!r} after trim on")
        read_cold(guest, "on")
        cold = wait_until_trimmed(guest, floor_kib, COLD_SECONDS, "cold")
        read_back, read_again = working_set(guest, page)
        resumed = wait_until_trimmed(
            guest, floor_kib, RESUME_SECONDS, "after the working set"
        )

        console.accepted("trim off")
        if enabled(guest) != "N":
            raise RuntimeError(f"{ENABLED} reads {enabled(guest)!r} after trim off")
        fragmented = trim_fragmented(guest, console, page)
        require_untrimmed(guest, "off")
        error, text = console.request("trim sometimes")
        if not error:
            raise RuntimeError(f"trim sometimes was accepted: {text!r}")

        failure = FAILURE.search(guest.output())
        if failure is not None:
            raise RuntimeError(
                f"the guest printed {failure[0]!r}:\n"
                + guest.output()[max(0, failure.start() - 2000):][:6000]
            )
        print(
            f"page {page} B: off at boot; {once}; on: cold cache trimmed in "
            f"{cold} s, {read_back // MIB} MiB of {read_again // MIB} MiB "
            f"read again came back, trimming resumed in {resumed} s; off: "
            f"{fragmented}"
        )
        if not guest.shutdown(60):
            raise RuntimeError("the guest did not power off")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-trim-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        run(arguments.emulator.resolve(), arguments.rootfs.resolve(),
            arguments.engine, private)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
