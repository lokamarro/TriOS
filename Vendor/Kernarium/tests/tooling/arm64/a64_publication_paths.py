#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Symlink-safe path checks and atomic publication for A64 table generators."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
import stat
import tempfile
from pathlib import Path
from typing import Sequence


def lexical_absolute_path(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def require_no_symlink_components(path: Path, label: str) -> Path:
    """Traverse without following links and canonicalize the existing prefix."""
    absolute = lexical_absolute_path(path)
    flags = os.O_RDONLY
    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(absolute.anchor, flags)
    current = Path(absolute.anchor)
    try:
        for index, part in enumerate(absolute.parts[1:]):
            try:
                status = os.stat(part, dir_fd=descriptor, follow_symlinks=False)
            except FileNotFoundError:
                canonical = current.resolve(strict=True)
                return canonical.joinpath(*absolute.parts[index + 1 :])
            current /= part
            if stat.S_ISLNK(status.st_mode):
                raise ValueError(f"{label} has a symlinked path component: {current}")
            if index + 2 == len(absolute.parts):
                return current.resolve(strict=True)
            if not stat.S_ISDIR(status.st_mode):
                raise ValueError(
                    f"{label} has a nondirectory path component: {current}"
                )
            next_descriptor = os.open(part, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = next_descriptor
        return current.resolve(strict=True)
    finally:
        os.close(descriptor)


def canonical_existing_directory(path: Path, label: str) -> Path:
    absolute = require_no_symlink_components(path, label)
    if not absolute.is_dir():
        raise ValueError(f"{label} is not a regular directory: {absolute}")
    return absolute.resolve(strict=True)


def _nearest_existing_directory(path: Path) -> Path:
    candidate = path if path.is_dir() else path.parent
    while not candidate.exists():
        if candidate == candidate.parent:
            raise ValueError(f"path has no existing directory ancestor: {path}")
        candidate = candidate.parent
    if not candidate.is_dir():
        candidate = candidate.parent
    return candidate


def filesystem_is_case_sensitive(path: Path) -> bool:
    """Read-only case-sensitivity probe for the target's current volume."""
    directory = _nearest_existing_directory(path)
    for candidate in (directory, *directory.parents):
        name = candidate.name
        for index, character in enumerate(name):
            if not character.isascii() or not character.isalpha():
                continue
            replacement = character.swapcase()
            variant = candidate.with_name(
                name[:index] + replacement + name[index + 1 :]
            )
            try:
                return not os.path.samefile(candidate, variant)
            except FileNotFoundError:
                return True
            except OSError:
                break
    # A volume whose entire path contains no alphabetic component is unusual;
    # preserving case distinctions is the conservative read-only fallback.
    return True


def _comparison_parts(left: Path, right: Path) -> tuple[tuple[str, ...], tuple[str, ...]]:
    left = lexical_absolute_path(left)
    right = lexical_absolute_path(right)
    left_directory = _nearest_existing_directory(left)
    right_directory = _nearest_existing_directory(right)
    same_volume = left_directory.stat().st_dev == right_directory.stat().st_dev
    insensitive = same_volume and (
        not filesystem_is_case_sensitive(left_directory)
        or not filesystem_is_case_sensitive(right_directory)
    )
    left_parts = left.parts
    right_parts = right.parts
    if insensitive:
        left_parts = tuple(part.casefold() for part in left_parts)
        right_parts = tuple(part.casefold() for part in right_parts)
    return tuple(left_parts), tuple(right_parts)


def paths_alias(left: Path, right: Path) -> bool:
    if left.exists() and right.exists():
        try:
            if os.path.samefile(left, right):
                return True
        except OSError:
            pass
    left_parts, right_parts = _comparison_parts(left, right)
    return left_parts == right_parts


def path_is_ancestor(ancestor: Path, descendant: Path) -> bool:
    ancestor_parts, descendant_parts = _comparison_parts(ancestor, descendant)
    return (
        len(ancestor_parts) < len(descendant_parts)
        and descendant_parts[: len(ancestor_parts)] == ancestor_parts
    )


def validate_publication_topology(
    file_targets: Sequence[Path],
    *,
    directory_targets: Sequence[Path] = (),
    read_inputs: Sequence[Path] = (),
) -> tuple[tuple[Path, ...], tuple[Path, ...], tuple[Path, ...]]:
    """Fail before staging when read/write path shapes cannot coexist."""
    files = tuple(
        require_no_symlink_components(path, "publication target")
        for path in file_targets
    )
    directories = tuple(
        require_no_symlink_components(path, "publication directory target")
        for path in directory_targets
    )
    reads = tuple(
        require_no_symlink_components(path, "publication read input")
        for path in read_inputs
    )
    writes = (*files, *directories)
    for index, left in enumerate(writes):
        for right in writes[index + 1 :]:
            if paths_alias(left, right):
                raise ValueError("publication write targets alias each other")
            if path_is_ancestor(left, right) or path_is_ancestor(right, left):
                raise ValueError("publication write targets have impossible ancestry")
    for path in files:
        if path.exists() and not path.is_file():
            raise ValueError(f"publication target is not a regular file: {path}")
    for path in directories:
        if path.exists() and not path.is_dir():
            raise ValueError(f"publication directory target is not a directory: {path}")
    for write in writes:
        for read in reads:
            if paths_alias(write, read):
                raise ValueError("publication target aliases a read input")
            if path_is_ancestor(write, read) or path_is_ancestor(read, write):
                raise ValueError(
                    "publication target has impossible read-input ancestry"
                )
    for path in writes:
        canonical_existing_directory(path.parent, "publication target parent")
    return files, directories, reads


def atomic_publish_bytes(outputs: Sequence[tuple[Path, bytes]]) -> None:
    """Write validated bytes through temporary files and atomic replacement."""
    staged: list[tuple[Path, Path]] = []
    try:
        for original, payload in outputs:
            path = require_no_symlink_components(original, "publication target")
            canonical_existing_directory(path.parent, "publication target parent")
            descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
            )
            temporary = Path(temporary_name)
            staged.append((path, temporary))
            with os.fdopen(descriptor, "wb") as destination:
                destination.write(payload)
                destination.flush()
        for path, temporary in staged:
            os.replace(temporary, path)
    finally:
        for _path, temporary in staged:
            if temporary.exists():
                temporary.unlink()
