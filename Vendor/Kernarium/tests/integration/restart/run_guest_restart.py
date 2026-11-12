#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that a guest that restarts itself boots again in the same process,
and that nothing of one boot is left for the next.

On each engine a Linux guest restarts twice and then powers off, all in one
emulator process (one process ID, one console). A process audit inserted into
the launcher answers from inside it. At the settle point of every boot the
process must look as it did at the first boot's: its descriptors by kind and
path, its threads by name, its Mach ports by right and kind, its next pthread
key, its signal dispositions and timers, and the status flags and terminal
settings of its stdio. At every hand-back the heaps the memory layer maps for a
run's engine metadata (VM tag 241) hold no dirty page, and the scratch of the
threads that remain (tag 242) holds no more than a kept chunk each. A pattern the first boot wrote into tmpfs and
anonymous guest memory is in guest RAM before the restart and nowhere in the
process after it. Then a guest whose /init restarts at once never boots past
it: the launcher ends the launch after the fifth restart in a row that comes
less than 15 s after its boot started, with status 3.

The contract is docs/architecture.md (guest lifecycle) and the launcher's
run loop in darwin/app/macos/main.cpp.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


RESTARTS = 2
# Idle time after a boot's shell prompt before the process is looked at, and
# how long a boot may take to look like the first.
SETTLE_SECONDS = 4
MATCH_SECONDS = 30
GUEST_MEMORY = "2G"
EARLY_RESTARTS = 5
LOOP_GUARD_STATUS = 3
LOOP_GUARD_SECONDS = 180

# What the process audit writes that must not change between boots, by line
# keyword. Memory figures are reported, not compared.
COMPARED = (
    "descriptor", "thread", "port", "pthread_next_key", "sigaction", "itimer",
    "stdio", "termios", "cwd", "umask",
)
# A receive right with no kernel object behind it is, for the most part, a
# thread's reply port: a thread makes it when it first calls the kernel and
# keeps it until it ends, so a thread has at most one. Which threads have
# called by the time the process is looked at, and which of the short-lived
# ones are still there, differs from one look to the next and with the load of
# the host, so the number of these rights may differ by as many as there are
# threads. The process also holds a few receive rights of its own that this
# kind cannot tell apart; every other kind of port must be exactly the first
# boot's, and so must the threads themselves, which is what bounds the count.
REPLY_PORTS = "rights=0x20000 kobject=-1"
# Thread scratch keeps one chunk of this many bytes for the thread's life
# (memory/domain_heap.hpp, ScratchArena::kept_bytes).
SCRATCH_CHUNK = 64 * 1024
# The temporary name of the guest-RAM token differs for each boot.
RANDOM_NAME = re.compile(r"(uml-phys-token-)\w+")


class AuditChannel:
    """The directory the runner and the audit inside the launcher talk
    through."""

    def __init__(self, directory: pathlib.Path) -> None:
        self.directory = directory
        self.count = 0

    def ask(self, verb: str, label: str, *extra: str, suffix: str,
            timeout: float = 180) -> str:
        self.count += 1
        label = f"{label}-{self.count}"
        result = self.directory / (label + suffix)
        pending = self.directory / "request.tmp"
        pending.write_text(" ".join((verb, label, *extra)) + "\n")
        os.rename(pending, self.directory / "request")
        deadline = time.monotonic() + timeout
        while not result.exists():
            if time.monotonic() > deadline:
                raise TimeoutError(f"the process audit did not answer {label}")
            time.sleep(0.1)
        return result.read_text()

    def dump(self, label: str) -> dict[str, list[str]]:
        text = self.ask("dump", label, suffix=".txt")
        if "\nend\n" not in text:
            raise RuntimeError(f"the process audit's {label} is incomplete")
        return parse_dump(text)

    def scan(self, label: str, pattern: bytes) -> dict[str, int]:
        text = self.ask("scan", label, pattern.hex(), suffix=".scan",
                        timeout=600)
        counts = {
            name: int(value)
            for name, value in re.findall(r"^(\w+) (\d+)$", text, re.M)
        }
        counts["text"] = text
        return counts


def parse_dump(text: str) -> dict[str, list[str]]:
    """The audit's lines grouped by their first word."""
    sections: dict[str, list[str]] = {}
    for line in text.splitlines():
        keyword, _, rest = line.partition(" ")
        sections.setdefault(keyword, []).append(rest)
    return sections


def counted(lines: list[str]) -> dict[str, int]:
    """Lines of the form "<count> <what>" as what -> count."""
    found: dict[str, int] = {}
    for line in lines:
        number, _, what = line.partition(" ")
        what = RANDOM_NAME.sub(r"\1*", what)
        found[what] = found.get(what, 0) + int(number)
    return found


def thread_total(dump: dict[str, list[str]]) -> int:
    """The threads the audit counted in the process, its own excluded."""
    return int(dump["threads"][0]) if dump.get("threads") else 0


def differences(first: dict[str, list[str]],
                other: dict[str, list[str]]) -> list[str]:
    """What the process shows now that it did not show at the first boot."""
    found = []
    for keyword in COMPARED:
        if keyword in ("descriptor", "thread", "port"):
            before, now = counted(first.get(keyword, [])), counted(other.get(keyword, []))
            slack = {}
            if keyword == "port":
                slack[REPLY_PORTS] = max(thread_total(first), thread_total(other))
            for what in sorted(set(before) | set(now)):
                if abs(before.get(what, 0) - now.get(what, 0)) > slack.get(what, 0):
                    found.append(
                        f"{keyword} {what}: {before.get(what, 0)} at the first "
                        f"boot, {now.get(what, 0)} now")
            continue
        before, now = sorted(first.get(keyword, [])), sorted(other.get(keyword, []))
        if before != now:
            found.append(
                f"{keyword}: first boot {sorted(set(before) - set(now))}, "
                f"now {sorted(set(now) - set(before))}"
            )
    return found


def settled(channel: AuditChannel, label: str,
            reference: dict[str, list[str]] | None) -> dict[str, list[str]]:
    """The process at this boot's settle point: once it looks as it did at the
    first boot (or, for the first, looks the same twice), or fails with what
    differs. A thread or descriptor that is still on its way out gets
    MATCH_SECONDS to go."""
    time.sleep(SETTLE_SECONDS)
    deadline = time.monotonic() + MATCH_SECONDS
    previous = channel.dump(label)
    while True:
        if reference is None:
            time.sleep(2)
            current = channel.dump(label)
            problems = differences(previous, current)
            previous = current
        else:
            problems = differences(reference, previous)
        if not problems:
            return previous
        if time.monotonic() > deadline:
            raise RuntimeError(
                f"{label}: the process is not as it was at the first boot:\n  "
                + "\n  ".join(problems)
            )
        time.sleep(2)
        if reference is not None:
            previous = channel.dump(label)


def heaps_returned(dump: dict[str, list[str]], label: str) -> str:
    """The memory layer's heaps at a hand-back: a run's domain heaps are given
    back whole when the run's memory domain retires, which the hand-back
    follows, so no page of them is dirty; only the chunk a thread keeps for its
    scratch may remain, one for each thread the process has."""
    def figure(keyword: str) -> int:
        if not dump.get(keyword):
            raise RuntimeError(f"{label}: the process audit did not report {keyword}")
        return int(dump[keyword][0])

    heap, scratch = figure("heap_dirty"), figure("scratch_dirty")
    threads = int(dump["threads"][0]) if dump.get("threads") else 0
    if heap != 0:
        raise RuntimeError(f"{label}: {heap} bytes of domain heaps are dirty "
                           "at the hand-back")
    if scratch > (threads + 1) * SCRATCH_CHUNK:
        raise RuntimeError(
            f"{label}: {scratch} bytes of thread scratch are dirty at the "
            f"hand-back, more than a kept chunk for each of {threads} threads")
    return f"heaps {heap} B, scratch {scratch // 1024} KiB"


def hand_back_dump(audit: pathlib.Path, number: int) -> dict[str, list[str]]:
    """The process as it was when the guest was handed back: the audit dumps
    it at the hand-back's own announcement (UML_PROCESS_AUDIT_HAND_BACK)."""
    path = audit / f"handback-{number}.txt"
    deadline = time.monotonic() + MATCH_SECONDS
    while not path.exists():
        if time.monotonic() > deadline:
            raise RuntimeError(f"the process audit made no {path.name}")
        time.sleep(0.2)
    text = path.read_text()
    if "\nend\n" not in text:
        raise RuntimeError(f"the process audit's {path.name} is incomplete")
    return parse_dump(text)


def footprint(dump: dict[str, list[str]]) -> str:
    def figure(keyword: str) -> int:
        return int(dump[keyword][0]) if dump.get(keyword) else -1

    return (
        f"footprint {figure('phys_footprint') / 1048576:.1f} MiB, "
        f"heap in use {figure('malloc_in_use') / 1048576:.2f} MiB"
    )


def require_aliased_text(engine: str, dump: dict[str, list[str]]) -> None:
    """The kernel's executable pages are the embedded section's own, not a
    dirty copy of them in anonymous memory."""
    def figure(keyword: str) -> int:
        return int(dump[keyword][0]) if dump.get(keyword) else -1

    pages, dirty = figure("kernel_text_pages"), figure("kernel_text_dirty_pages")
    if pages <= 0 or dirty != 0:
        raise RuntimeError(
            f"{engine}: the kernel's text is {pages} executable pages with "
            f"{dirty} dirty, not clean aliases of the embedded section"
        )
    print(f"{engine}: kernel text is {pages} executable pages, none dirty")


def write_pattern(guest: Guest) -> bytes:
    """A pattern in tmpfs and in anonymous memory of a process that stays. The
    host never holds it: the guest builds it from letters the host does send."""
    digits = "%040x" % random.getrandbits(160)
    letters = digits.translate(str.maketrans("0123456789abcdef", "ghijklmnopqrstuv"))
    guest.check_command(
        f"p=GRP$(echo {letters} | tr g-v 0-9a-f)AT; "
        'yes "$p" | head -c 33554432 > /tmp/pattern && '
        'dd if=/tmp/pattern of=/dev/null bs=1048576 2>/dev/null; '
        '( ( v=$(yes "$p" | head -c 4194304); while :; do sleep 100; done ) '
        "</dev/null >/dev/null 2>&1 & ); unset p; sleep 2; "
        "test -s /tmp/pattern",
        120,
    )
    return f"GRP{digits}AT".encode()


def run_engine(arguments: argparse.Namespace, engine: str) -> None:
    audit = pathlib.Path(tempfile.mkdtemp(prefix="uml-rs-"))
    environment = dict(os.environ)
    environment["DYLD_INSERT_LIBRARIES"] = str(arguments.audit_library)
    environment["UML_PROCESS_AUDIT_DIRECTORY"] = str(audit)
    environment["UML_PROCESS_AUDIT_HAND_BACK"] = "1"
    guest = Guest(
        arguments.emulator,
        arguments.rootfs,
        GUEST_MEMORY,
        "tmpfs",
        ("ncpus=4",),
        overlay_shell=True,
        engine=engine,
        environment=environment,
    )
    try:
        process_id = guest.process_id
        channel = AuditChannel(audit)
        guest.boot_to_shell(300)
        first = settled(channel, "boot1", None)
        print(f"{engine}: boot 1, pid {process_id}: {footprint(first)}")
        require_aliased_text(engine, first)
        token = write_pattern(guest)
        before = channel.scan("pattern-before", token)
        if before["window_matches"] == 0:
            raise RuntimeError(
                f"{engine}: the pattern is not in the guest's RAM before the "
                f"restart:\n{before['text']}"
            )
        print(
            f"{engine}: pattern in {before['window_matches']} pages of the "
            f"guest's window, {before['host_matches']} elsewhere, before the "
            "restart"
        )
        for restart in range(1, RESTARTS + 1):
            started = time.monotonic()
            guest.restart_to_shell(300)
            if guest.process_id != process_id or guest.return_code is not None:
                raise RuntimeError(f"{engine}: restart {restart} ended the process")
            current = settled(channel, f"boot{restart + 1}", first)
            after = channel.scan(f"pattern-after-{restart}", token)
            if after["window_matches"] or after["host_matches"]:
                raise RuntimeError(
                    f"{engine}: the first boot's pattern survived restart "
                    f"{restart}:\n{after['text']}"
                )
            require_aliased_text(engine, current)
            retries = guest.output().count("is not handed back yet")
            returned = heaps_returned(
                hand_back_dump(audit, restart),
                f"{engine}: hand-back {restart}")
            print(
                f"{engine}: restart {restart} to a shell in "
                f"{time.monotonic() - started:.1f} s, pid {process_id}, "
                f"as the first boot ({footprint(current)}), no pattern left, "
                f"{returned} at the hand-back"
                + (f", {retries} refused hand-backs so far" if retries else "")
            )
        if not guest.shutdown(60) or guest.return_code != 0:
            raise RuntimeError(
                f"{engine}: the power-off did not end the process with 0: "
                f"{guest.return_code}"
            )
        console = guest.output()
        for restart in range(1, RESTARTS + 1):
            if f"guest restarting (command '', restart {restart})" not in console:
                raise RuntimeError(f"{engine}: restart {restart} was not logged")
        if "guest restarting (command '', restart 3)" in console:
            raise RuntimeError(f"{engine}: the guest restarted a third time")
        print(f"{engine}: powered off after {RESTARTS} restarts, status 0")
    finally:
        guest.close()
        shutil.rmtree(audit, ignore_errors=True)


def run_loop_guard(arguments: argparse.Namespace, engine: str) -> None:
    """A guest whose /init restarts at once is booted again until the fifth
    early restart in a row, then the launcher stops with status 3."""
    command = [
        str(arguments.emulator),
        f"--execution-engine={engine}",
        f"--guest-init={arguments.restart_init}",
        f"ubd0c={arguments.rootfs}",
        "mem=256M",
        "ncpus=1",
    ]
    started = time.monotonic()
    try:
        done = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=LOOP_GUARD_SECONDS,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"{engine}: a guest that restarts at once was still booting "
            f"after {LOOP_GUARD_SECONDS} s"
        ) from error
    output = done.stdout.decode(errors="replace")
    restarts = len(re.findall(r"^guest restarting \(command ", output, re.M))
    if done.returncode != LOOP_GUARD_STATUS:
        raise RuntimeError(
            f"{engine}: the loop guard did not end the launch with status "
            f"{LOOP_GUARD_STATUS}: {done.returncode}\n{output[-2000:]}"
        )
    if restarts != EARLY_RESTARTS - 1:
        raise RuntimeError(
            f"{engine}: the guest was booted again {restarts} times, not "
            f"{EARLY_RESTARTS - 1}\n{output[-2000:]}"
        )
    if f"restarted {EARLY_RESTARTS} times in a row" not in output:
        raise RuntimeError(f"{engine}: the loop guard said nothing\n{output[-2000:]}")
    print(
        f"{engine}: a guest that restarts at once was booted {restarts + 1} "
        f"times in {time.monotonic() - started:.1f} s, then the launch ended "
        f"with status {done.returncode}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--audit-library", required=True, type=pathlib.Path)
    parser.add_argument("--restart-init", required=True, type=pathlib.Path)
    parser.add_argument("--engine", action="append", choices=("jit", "gadgets"))
    arguments = parser.parse_args()
    for name in ("emulator", "rootfs", "audit_library", "restart_init"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    for engine in arguments.engine or ["gadgets", "jit"]:
        run_engine(arguments, engine)
        run_loop_guard(arguments, engine)
    return 0


if __name__ == "__main__":
    sys.exit(main())
