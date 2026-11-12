#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that a guest run leaves nothing in the shared /tmp or its $TMPDIR.

Each arm boots a bare guest shell and ends it another way: an orderly
poweroff; SIGTERM, which the guest's own handler turns into an exit; SIGHUP,
which the host's handler takes; a guest panic; and SIGKILL, which nothing
sees. The emulator keeps its management console in a uml-kern<pid> directory
in its $TMPDIR and its per-run initrd beside it, so the runner gives every
guest a private $TMPDIR. While a guest runs, its console must be a socket in
its own directory there, and /tmp must hold nothing named for it. Once it has
ended, the private directory must be empty again and neither /tmp nor the
user's temporary directory may hold anything named for it. A guest killed
outright leaves its directories to the next emulator, which removes them as it
starts. The contract is docs/architecture.md (guest lifecycle).
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
import stat
import subprocess
import sys
import tempfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

import guest as guest_support  # noqa: E402
from guest import Guest  # noqa: E402


SHARED_TEMPORARY = pathlib.Path("/tmp")
EXIT_SECONDS = 30


def named_for(directory: pathlib.Path, process_id: int) -> set[str]:
    """Entries of ``directory`` whose name holds ``process_id`` as a number."""
    pattern = re.compile(rf"(?<!\d){process_id}(?!\d)")
    try:
        entries = os.listdir(directory)
    except FileNotFoundError:
        return set()
    return {entry for entry in entries if pattern.search(entry)}


class Residue:
    """Where a guest may leave files, and what it is allowed to leave."""

    def __init__(self, private: pathlib.Path) -> None:
        self.private = private
        user = guest_support.darwin_user_temporary_directory()
        self.watched = [SHARED_TEMPORARY]
        if user is not None:
            self.watched.append(user)
        self.before = {
            directory: set(os.listdir(directory)) for directory in self.watched
        }

    def new_entries(self, process_id: int) -> list[str]:
        """Entries created since the run began that are named for it."""
        found = []
        for directory in self.watched:
            created = named_for(directory, process_id) - self.before[directory]
            found += [str(directory / name) for name in sorted(created)]
        return found

    def require_running_cleanly(self, guest: Guest) -> None:
        process_id = guest.process_id
        if leaked := self.new_entries(process_id):
            raise RuntimeError(f"a running guest wrote {leaked}")
        directory = self.private / f"uml-kern{process_id}"
        try:
            details = os.lstat(directory)
            console = os.lstat(directory / "mconsole")
        except FileNotFoundError as error:
            raise RuntimeError(
                f"the guest's console is not in {directory}"
            ) from error
        if (
            not stat.S_ISDIR(details.st_mode)
            or stat.S_IMODE(details.st_mode) != 0o700
            or not stat.S_ISSOCK(console.st_mode)
        ):
            raise RuntimeError(f"{directory} is not a private console")

    def require_exited_cleanly(self, process_id: int, arm: str) -> None:
        """The emulator's own cleanup, checked before the harness's."""
        left = [
            str(self.private / name)
            for name in sorted(named_for(self.private, process_id))
        ]
        left += self.new_entries(process_id)
        if left:
            raise RuntimeError(f"{arm}: the guest left {left}")

    def require_empty(self, arm: str) -> None:
        """Nothing of the emulator's or the harness's is left."""
        if left := sorted(os.listdir(self.private)):
            raise RuntimeError(f"{arm}: {self.private} still holds {left}")


def start(arguments: argparse.Namespace, *kernel: str) -> Guest:
    guest = Guest(
        arguments.emulator,
        arguments.rootfs,
        "512M",
        "tmpfs",
        ("ncpus=1", *kernel),
        init_shell=True,
        engine=arguments.engine,
    )
    try:
        guest.boot_to_shell(180)
    except BaseException:
        guest.close()
        raise
    return guest


def end(guest: Guest, arm: str) -> None:
    """End a guest the way ``arm`` names and wait for it to exit."""
    if arm == "poweroff":
        exited = guest.shutdown(EXIT_SECONDS)
    elif arm == "panic":
        guest._write("echo c > /proc/sysrq-trigger\n")
        exited = guest.wait_for_exit(EXIT_SECONDS)
    else:
        os.kill(guest.process_id, getattr(signal, arm))
        exited = guest.wait_for_exit(EXIT_SECONDS)
    if not exited:
        raise RuntimeError(f"{arm}: the guest did not exit in {EXIT_SECONDS}s")


def run_arm(arguments: argparse.Namespace, residue: Residue, arm: str) -> None:
    guest = start(arguments, "sysrq_always_enabled")
    try:
        residue.require_running_cleanly(guest)
        end(guest, arm)
        # Before close(), whose cleanup after a kill must not hide a leak.
        residue.require_exited_cleanly(guest.process_id, arm)
    finally:
        guest.close()
    residue.require_empty(arm)
    print(f"{arm}: status {guest.return_code}, nothing left")


def run_killed_arm(arguments: argparse.Namespace, residue: Residue) -> None:
    killed = start(arguments)
    try:
        residue.require_running_cleanly(killed)
        end(killed, "SIGKILL")
        directory = residue.private / f"uml-kern{killed.process_id}"
        if not directory.is_dir():
            raise RuntimeError("SIGKILL: nothing was left for the next run")
        successor = start(arguments)
        try:
            if directory.exists():
                raise RuntimeError(
                    "SIGKILL: the next emulator kept the killed run's "
                    "directory"
                )
            residue.require_running_cleanly(successor)
            end(successor, "poweroff")
            residue.require_exited_cleanly(
                successor.process_id, "after SIGKILL"
            )
        finally:
            successor.close()
        residue.require_exited_cleanly(killed.process_id, "SIGKILL")
    finally:
        killed.close()
    residue.require_empty("SIGKILL")
    print("SIGKILL: the next emulator removed what was left")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()

    # Short, so the console's directory fits sun_path inside it. The harness
    # stages its files here too, and must also leave nothing.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-res-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        residue = Residue(private)
        for arm in ("poweroff", "SIGTERM", "SIGHUP", "panic"):
            run_arm(arguments, residue, arm)
        run_killed_arm(arguments, residue)
    except BaseException:
        subprocess.run(
            ["ls", "-laR", str(private)], stdin=subprocess.DEVNULL, check=False
        )
        raise
    finally:
        shutil.rmtree(private, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
