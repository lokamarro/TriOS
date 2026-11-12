#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record how a guest session ended, for the next launch.

Each case boots a guest with --session-record and ends it one way:

- a kernel panic (``echo c > /proc/sysrq-trigger``) aborts the emulator, and
  the record says ``guest-panic`` with the panic's message and the kernel
  output before it;
- an orderly ``poweroff -f`` exits with status 0 and records ``powered-off``;
- a SIGTERM reaches the kernel's fatal handler, which exits with 143 and
  records ``kernel-exit``;
- a SIGKILL leaves the record ``running``, as a system memory-limit end does.

Every record starts with the session's last memory checkpoint.

The contract is runtime/session_record.hpp.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import signal
import sys
import tempfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


EXIT_SECONDS = 90
PANIC = "sysrq triggered crash"


def fields(record: str) -> dict[str, str]:
    """The record's header fields, up to its quoted kernel output."""
    header = record.split("\nkernel-output\n", 1)[0]
    return dict(
        line.split(" ", 1) for line in header.splitlines() if " " in line
    )


def end_session(
    emulator: pathlib.Path, rootfs: pathlib.Path, engine: str, how: str,
    record: pathlib.Path
) -> tuple[int | None, str, str]:
    with Guest(
        emulator,
        rootfs,
        "512M",
        "tmpfs",
        ("ncpus=2", f"--session-record={record}"),
        init_shell=True,
        engine=engine,
    ) as guest:
        guest.boot_to_shell(180)
        running = fields(record.read_text())
        if running.get("state") != "running" or running.get("pid") != str(
            guest.process_id
        ):
            raise RuntimeError(f"{how}: a booted session is not running: "
                               f"{running}")
        if how == "panic":
            guest._write("echo c > /proc/sysrq-trigger\n")
        elif how == "poweroff":
            guest._write("sync; poweroff -f\n")
        else:
            os.kill(guest.process_id,
                    signal.SIGTERM if how == "sigterm" else signal.SIGKILL)
        if not guest.wait_for_exit(EXIT_SECONDS):
            raise RuntimeError(f"{how}: the emulator did not exit within "
                               f"{EXIT_SECONDS} s")
        return guest.return_code, guest.output(), record.read_text()


def check(how: str, code: int | None, output: str, record: str) -> None:
    header = fields(record)
    expected = {
        "panic": (-signal.SIGABRT, "ended", "guest-panic"),
        "poweroff": (0, "ended", "powered-off"),
        "sigterm": (128 + signal.SIGTERM, "ended", "kernel-exit"),
        "sigkill": (-signal.SIGKILL, "running", "running"),
    }[how]
    if (code, header.get("state"), header.get("end")) != expected:
        raise RuntimeError(
            f"{how}: status {code}, record {header}, expected {expected}:\n"
            + record[-3000:] + "\n--- console ---\n" + output[-3000:]
        )
    if how == "panic":
        if header.get("message") != PANIC:
            raise RuntimeError(f"panic: the record's message is "
                               f"{header.get('message')!r}")
        quoted = record.split("\nkernel-output\n", 1)[-1]
        for line in (f"Kernel panic - not syncing: {PANIC}",
                     f"Darwin UML panic: {PANIC}"):
            if line not in quoted:
                raise RuntimeError(f"panic: the record does not quote "
                                   f"{line!r}:\n{quoted[-3000:]}")
    # The memory checkpoint the record starts with, rewritten every 2 s.
    memory = header.get("memory", "").split()
    if (len(memory) < 6 or memory[0::2][:3] != ["at", "footprint", "peak"]
            or int(memory[1]) < int(header.get("started", "0"))
            or int(memory[3]) == 0):
        raise RuntimeError(f"{how}: the record has no memory checkpoint: "
                           f"{header.get('memory')!r}")
    if how == "sigterm" and header.get("status") != "143":
        raise RuntimeError(f"sigterm: the record's status is "
                           f"{header.get('status')!r}")
    print(f"{how}: status {code}, record {header.get('state')} "
          f"{header.get('end')}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    # The panic writes a crash report, and the system keeps only so many a
    # day for one program name: the case runs the emulator under a name of
    # its own, through a hard link beside it, so that it spends none of the
    # emulator's.
    emulator = arguments.emulator.resolve()
    alias = emulator.with_name(f"{emulator.name}-session-record-test")
    alias.unlink(missing_ok=True)
    try:
        os.link(emulator, alias)
    except OSError:
        alias = emulator
    try:
        with tempfile.TemporaryDirectory(prefix="uml-session-record-") as work:
            for how in ("panic", "poweroff", "sigterm", "sigkill"):
                record = pathlib.Path(work) / f"{how}.session"
                code, output, text = end_session(
                    alias if how == "panic" else emulator,
                    arguments.rootfs.resolve(), arguments.engine, how, record)
                check(how, code, output, text)
    finally:
        if alias != emulator:
            alias.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
