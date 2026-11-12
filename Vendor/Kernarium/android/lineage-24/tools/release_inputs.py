#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The tracked paths that decide an Android release, and their Git object IDs.

support/release/input-paths.tsv names them. A release records the object ID of
each `input` path as of the commit it was built from; the object ID is what the
path's content is, so it survives a history rewrite and a build host that works
from an exported snapshot, where a commit ID would not.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import subprocess
from pathlib import Path

LIST_RELATIVE = "android/lineage-24/support/release/input-paths.tsv"
INPUT = "input"
EXCLUDED = "excluded"
# A path the descriptor can carry in one space-separated `path=id` value.
PATH_RE = re.compile(r"^[A-Za-z0-9._+-]+(?:/[A-Za-z0-9._+-]+)*$")
OBJECT_ID_RE = re.compile(r"^[0-9a-f]{40}$")


class InputError(ValueError):
    pass


def normalized(path: str) -> bool:
    """Whether a path is relative, normalized and safe in a descriptor value."""
    return PATH_RE.fullmatch(path) is not None and not any(
        part in (".", "..") for part in path.split("/")
    )


def read_rows(list_file: Path) -> list[tuple[str, str, str]]:
    """Return the list's (kind, path, why) rows."""
    if not list_file.is_file() or list_file.is_symlink():
        raise InputError(f"release input list is missing or unsafe: {list_file}")
    rows: list[tuple[str, str, str]] = []
    seen: set[str] = set()
    for number, raw in enumerate(list_file.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        fields = raw.split("\t")
        if len(fields) != 3 or not all(field.strip() for field in fields):
            raise InputError(f"{list_file}:{number}: a row is kind, path and why, tab separated")
        kind, path, why = fields
        if kind not in (INPUT, EXCLUDED):
            raise InputError(f"{list_file}:{number}: unknown kind {kind!r}")
        if not normalized(path):
            raise InputError(f"{list_file}:{number}: {path!r} is not a normalized relative path")
        if path in seen:
            raise InputError(f"{list_file}:{number}: duplicate path {path}")
        seen.add(path)
        rows.append((kind, path, why))
    if not any(kind == INPUT for kind, _, _ in rows):
        raise InputError(f"{list_file} lists no input path")
    return rows


def input_paths(list_file: Path) -> list[str]:
    """Return the sorted `input` paths of the list."""
    return sorted(path for kind, path, _ in read_rows(list_file) if kind == INPUT)


def covering_row(
    rows: list[tuple[str, str, str]], file_path: str
) -> tuple[str, str, str] | None:
    """Return the longest row that covers a tracked file, or None."""
    best = None
    for row in rows:
        path = row[1]
        if file_path == path or file_path.startswith(path + "/"):
            if best is None or len(path) > len(best[1]):
                best = row
    return best


def git(repository: Path, *arguments: str, stdin: bytes | None = None) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=False,
        input=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise InputError(
            f"git {arguments[0]} failed in {repository}: "
            f"{result.stderr.decode('utf-8', 'replace').strip()}"
        )
    return result.stdout


def object_ids(repository: Path, paths: list[str], revision: str = "HEAD") -> dict[str, str]:
    """Return the Git object ID of each path at a revision.

    A directory's ID is its tree and a file's its blob. A path the revision does
    not have raises InputError naming it.
    """
    found: dict[str, str] = {}
    missing: list[str] = []
    if paths:
        request = "".join(f"{revision}:{path}\n" for path in paths).encode("utf-8")
        answer = git(repository, "cat-file", "--batch-check", stdin=request)
        for path, line in zip(paths, answer.decode("utf-8").splitlines(), strict=True):
            fields = line.split(" ")
            if len(fields) != 3 or OBJECT_ID_RE.fullmatch(fields[0]) is None:
                missing.append(path)
            else:
                found[path] = fields[0]
    if missing:
        raise InputError(f"{revision} has no {', '.join(missing)}")
    return {path: found[path] for path in sorted(found)}


def render_trees(trees: dict[str, str]) -> str:
    """Return the descriptor's one-line form: sorted, space separated `path=id`."""
    return " ".join(f"{path}={trees[path]}" for path in sorted(trees))


def parse_trees(text: str) -> dict[str, str]:
    """Parse the descriptor's `path=id path=id` form."""
    trees: dict[str, str] = {}
    for item in text.split(" "):
        path, separator, object_id = item.partition("=")
        if (
            not separator
            or not normalized(path)
            or OBJECT_ID_RE.fullmatch(object_id) is None
            or path in trees
        ):
            raise InputError(f"malformed input tree entry: {item!r}")
        trees[path] = object_id
    if render_trees(trees) != text:
        raise InputError("input trees are not sorted by path")
    return trees
