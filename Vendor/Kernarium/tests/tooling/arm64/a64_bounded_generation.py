#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded workspaces and subprocess supervision for A64 generators."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import math
import os
import shutil
import signal
import stat
import subprocess
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping, Sequence

from a64_publication_paths import (
    canonical_existing_directory,
    path_is_ancestor,
    paths_alias,
    require_no_symlink_components,
)


MIB = 1024 * 1024
DEFAULT_FREE_SPACE_RESERVE_MIB = 512
DEFAULT_MAX_WORKSPACE_MIB = 2048
DEFAULT_MAX_ARTIFACT_MIB = 512
DEFAULT_MAX_LOG_MIB = 16
DEFAULT_MAX_WORKSPACE_FILES = 100_000
MONITOR_INTERVAL_SECONDS = 0.05
TERMINATION_GRACE_SECONDS = 0.5


class GenerationLimitError(ValueError):
    """A generator exceeded a declared resource or process boundary."""


@dataclass(frozen=True)
class GenerationLimits:
    free_space_reserve_bytes: int
    max_workspace_bytes: int
    max_artifact_bytes: int
    max_log_bytes: int
    max_workspace_files: int

    def __post_init__(self) -> None:
        for name, value in (
            ("free-space reserve", self.free_space_reserve_bytes),
            ("workspace cap", self.max_workspace_bytes),
            ("artifact cap", self.max_artifact_bytes),
            ("log cap", self.max_log_bytes),
            ("workspace file cap", self.max_workspace_files),
        ):
            if type(value) is not int or value <= 0:
                raise ValueError(f"generation {name} must be positive")
        if self.max_artifact_bytes > self.max_workspace_bytes:
            raise ValueError("generation artifact cap exceeds workspace cap")


def add_bounded_generation_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--work-root",
        type=Path,
        help="Existing nonsymlink directory for bounded temporary workspaces.",
    )
    parser.add_argument(
        "--free-space-reserve-mib",
        type=int,
        default=DEFAULT_FREE_SPACE_RESERVE_MIB,
    )
    parser.add_argument(
        "--max-workspace-mib",
        type=int,
        default=DEFAULT_MAX_WORKSPACE_MIB,
    )
    parser.add_argument(
        "--max-artifact-mib",
        type=int,
        default=DEFAULT_MAX_ARTIFACT_MIB,
    )
    parser.add_argument(
        "--max-log-mib",
        type=int,
        default=DEFAULT_MAX_LOG_MIB,
    )
    parser.add_argument(
        "--max-workspace-files",
        type=int,
        default=DEFAULT_MAX_WORKSPACE_FILES,
    )


def limits_from_options(options: argparse.Namespace) -> GenerationLimits:
    return GenerationLimits(
        free_space_reserve_bytes=options.free_space_reserve_mib * MIB,
        max_workspace_bytes=options.max_workspace_mib * MIB,
        max_artifact_bytes=options.max_artifact_mib * MIB,
        max_log_bytes=options.max_log_mib * MIB,
        max_workspace_files=options.max_workspace_files,
    )


class BoundedWorkspace:
    """A nonsymlink temporary tree with explicit space and artifact limits."""

    def __init__(
        self,
        *,
        source_root: Path,
        publication_targets: Sequence[Path],
        work_root: Path | None,
        limits: GenerationLimits,
        prefix: str,
        existing_path: Path | None = None,
    ):
        if work_root is not None and existing_path is not None:
            raise ValueError(
                "bounded workspace cannot select both a root and an existing path"
            )
        if existing_path is None and (
            not prefix
            or Path(prefix).name != prefix
            or prefix in {".", ".."}
        ):
            raise ValueError("bounded workspace prefix is not a plain name")
        self.source_root = canonical_existing_directory(
            source_root, "generator source root"
        )
        self.publication_targets = tuple(publication_targets)
        self.work_root = work_root
        self.limits = limits
        self.prefix = prefix
        self.existing_path = existing_path
        self.path: Path | None = None
        self._temporary: tempfile.TemporaryDirectory[str] | None = None
        self._identity: tuple[int, int] | None = None

    @classmethod
    def adopt(
        cls,
        path: Path,
        *,
        source_root: Path,
        publication_targets: Sequence[Path],
        limits: GenerationLimits,
    ) -> "BoundedWorkspace":
        """Supervise a caller-owned existing directory without deleting it."""
        return cls(
            source_root=source_root,
            publication_targets=publication_targets,
            work_root=None,
            limits=limits,
            prefix="",
            existing_path=path,
        )

    def __enter__(self) -> "BoundedWorkspace":
        if self.path is not None or self._temporary is not None:
            raise RuntimeError("bounded workspace is already active")
        if self.existing_path is not None:
            root = canonical_existing_directory(
                self.existing_path, "existing generator workspace"
            )
        elif self.work_root is None:
            root = Path(tempfile.gettempdir()).resolve(strict=True)
        else:
            root = canonical_existing_directory(
                self.work_root, "generator work root"
            )
        if paths_alias(self.source_root, root) or path_is_ancestor(
            self.source_root, root
        ):
            raise ValueError("generator work root is inside the source tree")
        self._require_free_space(root, 0)
        if self.existing_path is not None:
            self.path = root
        else:
            self._temporary = tempfile.TemporaryDirectory(
                prefix=self.prefix, dir=root
            )
            self.path = Path(self._temporary.name).resolve(strict=True)
        try:
            assert self.path is not None
            source_overlap = (
                paths_alias(self.source_root, self.path)
                or path_is_ancestor(self.source_root, self.path)
                or path_is_ancestor(self.path, self.source_root)
            )
            output_overlap = False
            for target in self.publication_targets:
                safe_target = require_no_symlink_components(
                    target, "generator publication target"
                )
                if (
                    paths_alias(safe_target, self.path)
                    or path_is_ancestor(safe_target, self.path)
                    or path_is_ancestor(self.path, safe_target)
                ):
                    output_overlap = True
                    break
            if source_overlap or output_overlap:
                label = (
                    "source tree" if source_overlap else "publication target"
                )
                raise ValueError(f"generator workspace overlaps the {label}")
            status = self.path.stat()
            self._identity = (status.st_dev, status.st_ino)
            self.check()
        except BaseException:
            self._release_workspace()
            raise
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
        self._release_workspace()

    def _release_workspace(self) -> None:
        temporary = self._temporary
        self._temporary = None
        self._identity = None
        self.path = None
        if temporary is not None:
            temporary.cleanup()

    def require_path(self) -> Path:
        if self.path is None:
            raise RuntimeError("bounded workspace is not active")
        return self.path

    def _require_inside(self, path: Path) -> Path:
        root = self.require_path()
        candidate = require_no_symlink_components(path, "workspace artifact")
        if not paths_alias(candidate, root) and not path_is_ancestor(root, candidate):
            raise ValueError(f"workspace artifact escapes owned root: {candidate}")
        return candidate

    def _require_free_space(self, path: Path, planned_bytes: int) -> None:
        free = shutil.disk_usage(path).free
        required = self.limits.free_space_reserve_bytes + planned_bytes
        if free < required:
            raise GenerationLimitError(
                f"generator free space {free} is below required {required} bytes"
            )

    def usage(self) -> tuple[int, int]:
        root = self.require_path()
        identity = self._identity
        if identity is None:
            raise RuntimeError("bounded workspace identity is not active")
        total = 0
        files = 0
        flags = os.O_RDONLY
        if hasattr(os, "O_DIRECTORY"):
            flags |= os.O_DIRECTORY
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        root_descriptor = os.open(root, flags)
        root_status = os.fstat(root_descriptor)
        if (
            not stat.S_ISDIR(root_status.st_mode)
            or (root_status.st_dev, root_status.st_ino) != identity
        ):
            os.close(root_descriptor)
            raise GenerationLimitError("generator workspace identity changed")
        pending = [(root, root_descriptor)]
        try:
            while pending:
                directory, descriptor = pending.pop()
                try:
                    with os.scandir(descriptor) as entries:
                        for entry in entries:
                            status = entry.stat(follow_symlinks=False)
                            path = directory / entry.name
                            if stat.S_ISLNK(status.st_mode):
                                raise GenerationLimitError(
                                    "generator workspace contains a symlink: "
                                    f"{path}"
                                )
                            files += 1
                            if files > self.limits.max_workspace_files:
                                raise GenerationLimitError(
                                    "generator workspace exceeds file-count cap"
                                )
                            if stat.S_ISDIR(status.st_mode):
                                child = os.open(
                                    entry.name, flags, dir_fd=descriptor
                                )
                                child_status = os.fstat(child)
                                if (
                                    not stat.S_ISDIR(child_status.st_mode)
                                    or child_status.st_dev != status.st_dev
                                    or child_status.st_ino != status.st_ino
                                ):
                                    os.close(child)
                                    raise GenerationLimitError(
                                        "generator workspace directory changed "
                                        f"during scan: {path}"
                                    )
                                pending.append((path, child))
                                continue
                            if not stat.S_ISREG(status.st_mode):
                                raise GenerationLimitError(
                                    "generator workspace contains a special file: "
                                    f"{path}"
                                )
                            if status.st_size > self.limits.max_artifact_bytes:
                                raise GenerationLimitError(
                                    f"generator artifact exceeds cap: {path}"
                                )
                            total += status.st_size
                            if total > self.limits.max_workspace_bytes:
                                raise GenerationLimitError(
                                    "generator workspace exceeds byte cap"
                                )
                finally:
                    os.close(descriptor)
        finally:
            for _directory, descriptor in pending:
                os.close(descriptor)
        return total, files

    def check(self) -> None:
        total, _files = self.usage()
        self._require_free_space(self.require_path(), 0)
        if total > self.limits.max_workspace_bytes:
            raise GenerationLimitError("generator workspace exceeds byte cap")

    def reserve(self, planned_bytes: int, planned_files: int = 1) -> None:
        if (
            type(planned_bytes) is not int
            or type(planned_files) is not int
            or planned_bytes < 0
            or planned_files < 0
        ):
            raise ValueError("planned workspace growth cannot be negative")
        if planned_bytes > self.limits.max_artifact_bytes:
            raise GenerationLimitError("planned generator artifact exceeds cap")
        total, files = self.usage()
        if total + planned_bytes > self.limits.max_workspace_bytes:
            raise GenerationLimitError("planned generator workspace exceeds cap")
        if files + planned_files > self.limits.max_workspace_files:
            raise GenerationLimitError("planned generator file count exceeds cap")
        self._require_free_space(self.require_path(), planned_bytes)

    def validate_archive_members(self, members: Sequence[object]) -> None:
        regular_sizes: list[int] = []
        for member in members:
            isfile = getattr(member, "isfile", None)
            if callable(isfile) and isfile():
                size = int(getattr(member, "size"))
                if size < 0 or size > self.limits.max_artifact_bytes:
                    raise GenerationLimitError(
                        "archive member exceeds generator artifact cap"
                    )
                regular_sizes.append(size)
        total, files = self.usage()
        if total + sum(regular_sizes) > self.limits.max_workspace_bytes:
            raise GenerationLimitError(
                "archive expansion exceeds generator workspace cap"
            )
        if files + len(members) > self.limits.max_workspace_files:
            raise GenerationLimitError(
                "archive expansion exceeds generator file-count cap"
            )
        self._require_free_space(self.require_path(), sum(regular_sizes))

    def write_bytes(self, path: Path, payload: bytes) -> None:
        target = self._require_inside(path)
        self.reserve(len(payload))
        target.write_bytes(payload)
        self.check()

    def write_text(self, path: Path, payload: str) -> None:
        self.write_bytes(path, payload.encode("utf-8"))


def workspace_from_options(
    options: argparse.Namespace,
    *,
    source_root: Path,
    publication_targets: Sequence[Path],
    prefix: str,
) -> BoundedWorkspace:
    return BoundedWorkspace(
        source_root=source_root,
        publication_targets=publication_targets,
        work_root=options.work_root,
        limits=limits_from_options(options),
        prefix=prefix,
    )


def _group_exists(process_group: int) -> bool:
    try:
        os.killpg(process_group, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _group_gone_within(process_group: int, grace_seconds: float) -> bool:
    """Whether the group has no member after `grace_seconds`.

    Darwin keeps reporting an exited leader's group (EPERM from killpg) for a
    few milliseconds after the leader is reaped, so a group seen right after
    the leader's exit is given the grace period to disappear before it counts
    as surviving descendants.
    """
    deadline = time.monotonic() + grace_seconds
    while _group_exists(process_group):
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.005)
    return True


def _terminate_group(process: subprocess.Popen[bytes]) -> None:
    group = process.pid
    if _group_exists(group):
        try:
            os.killpg(group, signal.SIGTERM)
        except ProcessLookupError:
            pass
        except PermissionError:
            if process.poll() is None:
                process.terminate()
    deadline = time.monotonic() + TERMINATION_GRACE_SECONDS
    while _group_exists(group) and time.monotonic() < deadline:
        time.sleep(0.01)
    if _group_exists(group):
        try:
            os.killpg(group, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except PermissionError:
            if process.poll() is None:
                process.kill()
    try:
        process.wait(timeout=TERMINATION_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def _capture_streams(
    process: subprocess.Popen[bytes],
    maximum_bytes: int,
    violation: list[str],
    violation_event: threading.Event,
) -> tuple[list[bytes], list[bytes], list[threading.Thread]]:
    stdout_chunks: list[bytes] = []
    stderr_chunks: list[bytes] = []
    total = 0
    lock = threading.Lock()

    def reader(stream: object, chunks: list[bytes], label: str) -> None:
        nonlocal total
        try:
            while True:
                block = stream.read(64 * 1024)  # type: ignore[attr-defined]
                if not block:
                    return
                with lock:
                    remaining = maximum_bytes - total
                    if remaining > 0:
                        chunks.append(block[:remaining])
                    total += len(block)
                    if total > maximum_bytes and not violation:
                        violation.append(
                            f"generator combined stdout/stderr exceeds "
                            f"{maximum_bytes} bytes ({label})"
                        )
                        violation_event.set()
                        return
        except (OSError, ValueError) as error:
            with lock:
                if not violation:
                    violation.append(
                        f"generator {label} capture failed: {error}"
                    )
                    violation_event.set()

    assert process.stdout is not None and process.stderr is not None
    threads = [
        threading.Thread(
            target=reader,
            args=(process.stdout, stdout_chunks, "stdout"),
            daemon=True,
        ),
        threading.Thread(
            target=reader,
            args=(process.stderr, stderr_chunks, "stderr"),
            daemon=True,
        ),
    ]
    for thread in threads:
        thread.start()
    return stdout_chunks, stderr_chunks, threads


def run_bounded(
    argv: Sequence[str],
    *,
    workspace: BoundedWorkspace,
    timeout_seconds: float,
    input_bytes: bytes | None = None,
    environment: Mapping[str, str] | None = None,
    check: bool = False,
    cancel_event: threading.Event | None = None,
    process_observer: (
        Callable[[subprocess.Popen[bytes]], str | None] | None
    ) = None,
) -> subprocess.CompletedProcess[str]:
    if (
        not argv
        or isinstance(timeout_seconds, bool)
        or not math.isfinite(timeout_seconds)
        or timeout_seconds <= 0
    ):
        raise ValueError("bounded command and positive timeout are required")
    if cancel_event is not None and cancel_event.is_set():
        raise GenerationLimitError("generator command was cancelled")
    if input_bytes is not None and len(input_bytes) > workspace.limits.max_log_bytes:
        raise GenerationLimitError("generator command input exceeds log cap")
    workspace.check()
    command = [str(value) for value in argv]
    child_environment = dict(os.environ if environment is None else environment)
    temporary = workspace.require_path() / "tmp"
    if temporary.exists():
        if temporary.is_symlink() or not temporary.is_dir():
            raise GenerationLimitError(
                "generator command temporary path is not a directory"
            )
    else:
        workspace.reserve(0)
        temporary.mkdir()
    workspace.check()
    child_environment["TMPDIR"] = str(temporary)
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=workspace.require_path(),
        env=child_environment,
        start_new_session=True,
    )
    violation: list[str] = []
    violation_event = threading.Event()
    stdout_chunks: list[bytes] = []
    stderr_chunks: list[bytes] = []
    readers: list[threading.Thread] = []
    input_thread: threading.Thread | None = None
    pending_error: BaseException | None = None

    def write_input() -> None:
        assert input_bytes is not None and process.stdin is not None
        try:
            process.stdin.write(input_bytes)
            process.stdin.close()
        except BrokenPipeError:
            pass
        except (OSError, ValueError) as error:
            if not violation:
                violation.append(f"generator command input failed: {error}")
                violation_event.set()

    try:
        stdout_chunks, stderr_chunks, readers = _capture_streams(
            process,
            workspace.limits.max_log_bytes,
            violation,
            violation_event,
        )
        if input_bytes is not None:
            input_thread = threading.Thread(target=write_input, daemon=True)
            input_thread.start()
        deadline = time.monotonic() + timeout_seconds
        while True:
            if violation_event.is_set():
                break
            if cancel_event is not None and cancel_event.is_set():
                violation.append("generator command was cancelled")
                break
            returncode = process.poll()
            if returncode is not None:
                if not _group_gone_within(
                    process.pid, TERMINATION_GRACE_SECONDS
                ):
                    violation.append(
                        "generator leader exited while descendants remained"
                    )
                break
            if time.monotonic() >= deadline:
                violation.append(
                    f"generator command exceeded {timeout_seconds} seconds"
                )
                break
            try:
                workspace.check()
            except (OSError, ValueError) as error:
                violation.append(str(error))
                break
            if process_observer is not None:
                try:
                    observation = process_observer(process)
                except Exception as error:
                    violation.append(
                        f"generator process observer failed: {error}"
                    )
                    break
                if observation is not None:
                    if not isinstance(observation, str) or not observation:
                        violation.append(
                            "generator process observer returned an invalid reason"
                        )
                        break
                    violation.append(observation)
                    break
            time.sleep(MONITOR_INTERVAL_SECONDS)
    except BaseException as error:
        pending_error = error
    finally:
        try:
            if violation or pending_error is not None:
                _terminate_group(process)
            else:
                process.wait()
        except BaseException as error:
            if pending_error is None:
                pending_error = error
        if input_thread is not None and input_thread.ident is not None:
            input_thread.join(timeout=TERMINATION_GRACE_SECONDS)
            if input_thread.is_alive():
                if not violation:
                    violation.append(
                        "generator command input writer did not terminate"
                    )
                try:
                    _terminate_group(process)
                except (OSError, subprocess.SubprocessError):
                    pass
                input_thread.join(timeout=TERMINATION_GRACE_SECONDS)
        for thread in readers:
            thread.join(timeout=TERMINATION_GRACE_SECONDS)
        live_readers = [thread for thread in readers if thread.is_alive()]
        if live_readers:
            if not violation:
                violation.append("generator log readers did not reach EOF")
            try:
                _terminate_group(process)
            except (OSError, subprocess.SubprocessError):
                pass
            for thread in live_readers:
                thread.join(timeout=TERMINATION_GRACE_SECONDS)
        try:
            workspace.check()
        except (OSError, ValueError) as error:
            if not violation and pending_error is None:
                violation.append(str(error))

    if pending_error is not None:
        raise pending_error

    stdout = b"".join(stdout_chunks).decode("utf-8", errors="replace")
    stderr = b"".join(stderr_chunks).decode("utf-8", errors="replace")
    if violation:
        raise GenerationLimitError(violation[0])
    completed = subprocess.CompletedProcess(
        command, process.returncode, stdout, stderr
    )
    if check and completed.returncode != 0:
        raise subprocess.CalledProcessError(
            completed.returncode,
            command,
            output=stdout,
            stderr=stderr,
        )
    return completed


class BoundedCommandRunner:
    """Small subprocess.run-compatible adapter for pinned generators."""

    def __init__(self, workspace: BoundedWorkspace, timeout_seconds: float):
        if (
            isinstance(timeout_seconds, bool)
            or not math.isfinite(timeout_seconds)
            or timeout_seconds <= 0
        ):
            raise ValueError("generator timeout must be positive")
        self.workspace = workspace
        self.timeout_seconds = timeout_seconds

    def run(
        self,
        argv: Sequence[str],
        *,
        input: str | bytes | None = None,
        check: bool = False,
        text: bool = True,
        capture_output: bool = False,
        stdout: object | None = None,
        stderr: object | None = None,
        timeout: float | None = None,
        env: Mapping[str, str] | None = None,
        cancel_event: threading.Event | None = None,
        process_observer: (
            Callable[[subprocess.Popen[bytes]], str | None] | None
        ) = None,
    ) -> subprocess.CompletedProcess[str]:
        if not text:
            raise ValueError("bounded command runner requires text output")
        if not capture_output and not (
            stdout is subprocess.PIPE and stderr is subprocess.PIPE
        ):
            raise ValueError("bounded command runner requires captured output")
        if input is None:
            encoded = None
        elif isinstance(input, bytes):
            encoded = input
        else:
            encoded = input.encode("utf-8")
        maximum = self.timeout_seconds if timeout is None else timeout
        if maximum <= 0 or maximum > self.timeout_seconds:
            raise ValueError("command timeout exceeds bounded runner policy")
        return run_bounded(
            argv,
            workspace=self.workspace,
            timeout_seconds=maximum,
            input_bytes=encoded,
            environment=env,
            check=check,
            cancel_event=cancel_event,
            process_observer=process_observer,
        )
