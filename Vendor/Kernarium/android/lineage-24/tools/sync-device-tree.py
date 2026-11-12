#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synchronize the checked Kernarium device overlay into the AOSP workspace.

The copy gives every file whose bytes or mode differ from what the destination
held before a fresh modification time, and restores the time of every file that
did not change. Make decides by modification time whether an output is up to
date, and a checkout keeps the time of the commit that wrote each file: an
overlay that switches back to older content would otherwise find the output of
the newer content, installed by an earlier build, newer than its own input and
leave it in place. A file that did not change keeps its time, so the build does
not redo work that depends only on it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import shutil
import stat
import time
from pathlib import Path, PurePosixPath


DESTINATION = PurePosixPath("device/kernarium/andarium")
# The product's earlier device directory. A workspace that still holds it would define
# the retired lineage_darwin product beside the current one, so the sync removes it
# and the verification refuses it.
RETIRED_DESTINATIONS = (PurePosixPath("device/kernarium/darwin"),)


def fail(message: str) -> None:
    raise SystemExit(f"device-tree sync: {message}")


def checked_root(path: Path, label: str) -> Path:
    if not path.is_absolute():
        fail(f"{label} must be absolute")
    resolved = path.resolve()
    if resolved == Path("/") or not resolved.is_dir():
        fail(f"{label} is not a directory: {resolved}")
    return resolved


def inventory(root: Path) -> dict[PurePosixPath, tuple[str, int, bytes | None]]:
    result: dict[PurePosixPath, tuple[str, int, bytes | None]] = {}
    for current, directories, files in os.walk(root, topdown=True, followlinks=False):
        current_path = Path(current)
        relative_directory = current_path.relative_to(root)
        for name in sorted(directories):
            path = current_path / name
            relative = PurePosixPath((relative_directory / name).as_posix())
            if path.is_symlink():
                fail(f"symbolic links are forbidden: {relative}")
            result[relative] = ("directory", stat.S_IMODE(path.stat().st_mode), None)
        for name in sorted(files):
            path = current_path / name
            relative = PurePosixPath((relative_directory / name).as_posix())
            if path.is_symlink() or not path.is_file():
                fail(f"non-regular device entry is forbidden: {relative}")
            result[relative] = (
                "file",
                stat.S_IMODE(path.stat().st_mode),
                path.read_bytes(),
            )
    return result


def verify(source: Path, destination: Path) -> None:
    if not destination.is_dir() or destination.is_symlink():
        fail(f"destination is missing or unsafe: {destination}")
    source_entries = inventory(source)
    destination_entries = inventory(destination)
    if source_entries.keys() != destination_entries.keys():
        missing = sorted(str(path) for path in source_entries.keys() - destination_entries.keys())
        extra = sorted(str(path) for path in destination_entries.keys() - source_entries.keys())
        detail = []
        if missing:
            detail.append(f"missing={','.join(missing[:8])}")
        if extra:
            detail.append(f"extra={','.join(extra[:8])}")
        fail(f"destination inventory differs ({'; '.join(detail)})")
    for relative, source_entry in source_entries.items():
        if destination_entries[relative] != source_entry:
            fail(f"destination entry differs: {relative}")


def file_times(root: Path, entries: dict[PurePosixPath, tuple[str, int, bytes | None]]) -> dict[PurePosixPath, int]:
    """The modification time, in nanoseconds, of every regular file of the inventory."""
    return {
        relative: (root / relative).stat().st_mtime_ns
        for relative, entry in entries.items()
        if entry[0] == "file"
    }


def synchronize(source: Path, destination: Path) -> None:
    # copytree follows links, so reject a linked source entry before copying.
    source_entries = inventory(source)
    before: dict[PurePosixPath, tuple[str, int, bytes | None]] = {}
    times: dict[PurePosixPath, int] = {}
    if destination.exists() or destination.is_symlink():
        if destination.is_symlink() or not destination.is_dir():
            fail(f"destination is not a regular directory: {destination}")
        # Inspect before removing so a stale symbolic link cannot redirect any
        # operation outside the one exact device subtree.
        before = inventory(destination)
        times = file_times(destination, before)
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination, copy_function=shutil.copy2)
    verify(source, destination)
    # A file that changed is new to the build whatever its checkout time says; one
    # that did not keeps the time the build already saw.
    now = time.time_ns()
    for relative, entry in source_entries.items():
        if entry[0] != "file":
            continue
        stamp = times[relative] if before.get(relative) == entry else now
        os.utime(destination / relative, ns=(stamp, stamp))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()

    android_root = checked_root(args.android_root, "Android policy root")
    workspace = checked_root(args.workspace, "AOSP source")
    if not (workspace / ".repo/manifests").is_dir():
        fail("AOSP source lacks Repo metadata")
    source = android_root / "overlay"
    if not source.is_dir() or source.is_symlink():
        fail("checked device overlay is missing or unsafe")
    destination = workspace / Path(DESTINATION)
    try:
        destination.parent.resolve().relative_to(workspace)
    except ValueError:
        fail("device destination escapes the AOSP source")

    retired = [workspace / Path(name) for name in RETIRED_DESTINATIONS]
    if args.verify:
        for name in retired:
            if name.exists() or name.is_symlink():
                fail(f"retired device directory is still present: {name}")
        verify(source, destination)
        print(f"verified exact device overlay: {destination}")
    else:
        for name in retired:
            if name.is_symlink() or (name.exists() and not name.is_dir()):
                fail(f"retired device directory is not a regular directory: {name}")
            if name.exists():
                shutil.rmtree(name)
        synchronize(source, destination)
        print(f"synchronized exact device overlay: {destination}")


if __name__ == "__main__":
    main()
