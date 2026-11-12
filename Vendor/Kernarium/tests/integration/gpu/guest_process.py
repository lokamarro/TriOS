# SPDX-License-Identifier: GPL-2.0-or-later
"""Signal-safe ownership of the host processes used by GPU guest runners."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import signal
import stat
import subprocess
from typing import Any, Callable, Iterator, Sequence


OWNERSHIP_ENV = "UML_GPU_GUEST_OWNERSHIP"
JOURNAL_NAME = "guest-ownership.jsonl"
MAX_JOURNAL_BYTES = 1024
EMULATOR_STOP = ((signal.SIGTERM, 10), (signal.SIGKILL, 5))
HARNESS_STOP = ((signal.SIGINT, 45), (signal.SIGTERM, 20), (signal.SIGKILL, 5))
# The leaf must finish the complete harness fallback before the pool escalates.
LEAF_CLEANUP_GRACE = sum(timeout for _, timeout in HARNESS_STOP) + 10


class OwnershipError(RuntimeError):
    """A nested owner has not proved that its exact child was reaped."""


class Interrupts:
    """Deliver at most one SIGINT/SIGTERM to the body as KeyboardInterrupt.

    The phase leaves "running" before the raise, never after it: a repeated
    signal handled while that exception unwinds through the body, contextlib
    and the cleanup's first lines is then ignored instead of raising past the
    stop/reap/journal cleanup.
    """

    def __init__(self) -> None:
        self.previous: dict[int, Any] = {}
        self.pending = False
        self.phase = "setup"

    def handle(self, _number: int, _frame: Any) -> None:
        if self.phase == "setup":
            self.pending = True
        elif self.phase == "running":
            self.phase = "interrupted"
            raise KeyboardInterrupt

    def __enter__(self) -> Interrupts:
        try:
            for number in (signal.SIGINT, signal.SIGTERM):
                self.previous[number] = signal.signal(number, self.handle)
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def __exit__(self, *_error: Any) -> None:
        for number, handler in self.previous.items():
            signal.signal(number, handler)

    def running(self) -> None:
        self.phase = "running"
        if self.pending:
            self.phase = "interrupted"
            raise KeyboardInterrupt

    def cleanup(self) -> None:
        self.phase = "cleanup"
        for number in self.previous:
            signal.signal(number, signal.SIG_IGN)


def stop_process(process: subprocess.Popen[bytes],
                 stages: Sequence[tuple[int, float]]) -> None:
    """Reap only the direct child; repeated signals cannot skip its fallback."""
    with Interrupts() as interrupts:
        interrupts.cleanup()
        for number, timeout in stages:
            if process.poll() is not None:
                return
            try:
                process.send_signal(number)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=timeout)
                return
            except subprocess.TimeoutExpired:
                if number == stages[-1][0]:
                    raise


class Journal:
    def __init__(self, setting: str) -> None:
        value = json.loads(setting)
        if (not isinstance(value, dict) or set(value) != {"path", "token"}
                or not isinstance(value["path"], str)
                or not isinstance(value["token"], str)
                or re.fullmatch(r"[0-9a-f]{32}", value["token"]) is None):
            raise OwnershipError("invalid guest ownership setting")
        self.path = Path(value["path"])
        if not self.path.is_absolute() or self.path.name != JOURNAL_NAME:
            raise OwnershipError("guest ownership requires its exact absolute journal")
        self.output = self.path.open("x", encoding="ascii")
        try:
            self.write({"event": "starting", "token": value["token"],
                        "owner_pid": os.getpid()})
        except BaseException:
            self.close()
            raise

    def write(self, record: dict[str, Any]) -> None:
        self.output.write(json.dumps(record, sort_keys=True) + "\n")
        self.output.flush()

    def close(self) -> None:
        self.output.close()


class SocketOwnership:
    """Retire only the observed socket of this launch, after its child is reaped."""

    def __init__(self, command: Sequence[str]) -> None:
        paths = [argument.split("=", 1)[1] for argument in command
                 if argument.startswith("--virtio-serial-socket=")]
        if len(paths) > 1:
            raise OwnershipError("multiple guest artifact sockets")
        self.path = Path(paths[0]) if paths else None
        self.identity: tuple[int, int] | None = None
        if self.path is not None:
            if not self.path.is_absolute() or os.path.lexists(self.path):
                raise OwnershipError("guest artifact socket path is not fresh and absolute")

    def observe(self) -> None:
        if self.path is None:
            return
        try:
            details = self.path.lstat()
        except FileNotFoundError:
            return
        identity = (details.st_dev, details.st_ino)
        if (not stat.S_ISSOCK(details.st_mode) or details.st_uid != os.getuid()
                or stat.S_IMODE(details.st_mode) != 0o600
                or self.identity not in (None, identity)):
            raise OwnershipError("guest artifact socket ownership changed")
        self.identity = identity

    def close(self) -> None:
        if self.path is None:
            return
        try:
            details = self.path.lstat()
        except FileNotFoundError:
            return
        if (self.identity != (details.st_dev, details.st_ino)
                or not stat.S_ISSOCK(details.st_mode) or details.st_uid != os.getuid()):
            raise OwnershipError("cannot retire an unobserved or replaced artifact socket")
        self.path.unlink()


@contextmanager
def managed_child(command: Sequence[str], *,
                  stop: Callable[[subprocess.Popen[bytes]], None],
                  cleanup: Callable[[], None] = lambda: None,
                  journal_setting: str | None = None,
                  **popen_options: Any) -> Iterator[subprocess.Popen[bytes]]:
    """Arm cleanup before Popen, including failures in subsequent setup.

    Only the emulator owner writes a journal. Its final record follows wait()
    and all caller-owned socket/console cleanup, even when the body failed.
    """
    process = None
    journal = None
    with Interrupts() as interrupts:
        try:
            if journal_setting is not None:
                journal = Journal(journal_setting)
            process = subprocess.Popen(command, **popen_options)
            if journal is not None:
                journal.write({"event": "started", "pid": process.pid})
            interrupts.running()
            yield process
        finally:
            interrupts.cleanup()
            try:
                try:
                    if process is not None:
                        stop(process)
                        return_code = process.wait(timeout=5)
                finally:
                    cleanup()
                if journal is not None:
                    journal.write({"event": "not-started"} if process is None else
                                  {"event": "reaped", "pid": process.pid,
                                   "return_code": return_code})
            finally:
                if journal is not None:
                    journal.close()


def validate_journal(path: Path, token: str, *, required: bool = False) -> None:
    """A missing pre-spawn journal is safe only for a failed/preflight leaf."""
    if not isinstance(token, str) or re.fullmatch(r"[0-9a-f]{32}", token) is None:
        raise OwnershipError("invalid guest ownership run identity")
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        if not required:
            return
        raise OwnershipError("guest ownership acknowledgement is missing") from None
    try:
        details = os.fstat(descriptor)
        if not stat.S_ISREG(details.st_mode) or details.st_size > MAX_JOURNAL_BYTES:
            raise OwnershipError("invalid guest ownership journal file")
        payload = os.read(descriptor, MAX_JOURNAL_BYTES + 1)
    finally:
        os.close(descriptor)
    if len(payload) > MAX_JOURNAL_BYTES:
        raise OwnershipError("guest ownership journal exceeds its bound")

    def unique_fields(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result = dict(pairs)
        if len(result) != len(pairs):
            raise ValueError("duplicate journal field")
        return result

    try:
        payload = payload.decode("ascii")
        records = [json.loads(line, object_pairs_hook=unique_fields)
                   for line in payload.splitlines()]
    except (ValueError, UnicodeError) as error:
        raise OwnershipError("invalid guest ownership journal encoding") from error
    if (not payload.endswith("\n") or len(records) not in (2, 3)
            or not all(isinstance(record, dict) for record in records)):
        raise OwnershipError("guest ownership acknowledgement is incomplete")
    first = records[0]
    if (set(first) != {"event", "token", "owner_pid"}
            or first["event"] != "starting" or first["token"] != token
            or type(first["owner_pid"]) is not int or first["owner_pid"] <= 1):
        raise OwnershipError("guest ownership journal belongs to another run")
    if len(records) == 2 and records[1] == {"event": "not-started"} and not required:
        return
    if len(records) != 3:
        raise OwnershipError("guest ownership acknowledgement is incomplete")
    started, reaped = records[1:]
    if (set(started) != {"event", "pid"} or started["event"] != "started"
            or type(started["pid"]) is not int or started["pid"] <= 1
            or set(reaped) != {"event", "pid", "return_code"}
            or reaped["event"] != "reaped" or type(reaped["pid"]) is not int
            or reaped["pid"] != started["pid"]
            or type(reaped["return_code"]) is not int):
        raise OwnershipError("guest ownership acknowledgement does not match its child")
