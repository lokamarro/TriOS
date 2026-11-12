#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
from pathlib import Path

import pinned_source
# pinned_source puts scripts/ on sys.path.
import patch_series  # noqa: E402


PROJECT_HEADER = pinned_source.AOSP_ROUTE_HEADER
BASE_COMMIT, BASE_TREE, FINAL_TREE, SERIES_DIR = (
    PROJECT_HEADER.index(column)
    for column in ("base_commit", "base_tree", "final_tree", "series_dir")
)
HEX40 = pinned_source.HEX40
SAFE_ROUTE = re.compile(r"p[0-9]{4}")


class RegenerationError(ValueError):
    pass


def run(*command: str, cwd: Path | None = None) -> str:
    return pinned_source.run(*command, cwd=cwd, error=RegenerationError)


def read_project_rows(path: Path) -> tuple[list[str], list[list[str]]]:
    lines = path.read_text().splitlines()
    header_index = next(
        (index for index, line in enumerate(lines) if line and not line.startswith("#")),
        None,
    )
    if header_index is None or tuple(lines[header_index].split("\t")) != PROJECT_HEADER:
        raise RegenerationError("projects.tsv has the wrong header")
    rows: list[list[str]] = []
    for line in lines[header_index + 1 :]:
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != len(PROJECT_HEADER):
            raise RegenerationError("projects.tsv contains a malformed route")
        rows.append(fields)
    return lines[: header_index + 1], rows


def safe_series_path(android_root: Path, value: str, route_id: str) -> Path:
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        raise RegenerationError("route series path is unsafe")
    expected = Path("patches/projects") / route_id / "series"
    if relative != expected:
        raise RegenerationError("route series path does not match its route ID")
    result = (android_root / relative).resolve()
    try:
        result.relative_to(android_root)
    except ValueError as error:
        raise RegenerationError("route series path escapes the Android root") from error
    return result


def verify_topic(topic: Path, base_tree: str, base_commit: str) -> None:
    if topic.is_symlink() or not topic.is_dir():
        raise RegenerationError("topic checkout must be a non-symlink directory")
    top = Path(run("git", "rev-parse", "--show-toplevel", cwd=topic)).resolve()
    if top != topic:
        raise RegenerationError("topic path is not the root of its Git checkout")
    if run("git", "rev-parse", f"{base_commit}^{{tree}}", cwd=topic) != base_tree:
        raise RegenerationError("topic base commit has the wrong tree")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Regenerate one routed AOSP project's generated patch series"
    )
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--route-id", required=True)
    parser.add_argument("--topic-checkout", type=Path, required=True)
    args = parser.parse_args()

    try:
        android_root = args.android_root.resolve()
        topic = args.topic_checkout.resolve()
        if SAFE_ROUTE.fullmatch(args.route_id) is None:
            raise RegenerationError("route ID must use the pNNNN form")
        projects_path = android_root / "patches/projects.tsv"
        prefix, rows = read_project_rows(projects_path)
        matching = [row for row in rows if row[1] == args.route_id]
        if len(matching) != 1:
            raise RegenerationError("route ID is not present exactly once")
        row = matching[0]
        base_commit, base_tree = row[BASE_COMMIT], row[BASE_TREE]
        if HEX40.fullmatch(base_commit) is None or HEX40.fullmatch(base_tree) is None:
            raise RegenerationError("route base identity is malformed")
        verify_topic(topic, base_tree, base_commit)
        series_dir = safe_series_path(android_root, row[SERIES_DIR], args.route_id)
        final_tree = patch_series.export_series(
            topic, base_commit, "HEAD", series_dir, license_id="Apache-2.0"
        )
        row[FINAL_TREE] = final_tree
        projects_path.write_text(
            "\n".join(prefix + ["\t".join(item) for item in rows]) + "\n"
        )
        verifier = android_root / "tools/verify-aosp-patches.py"
        run(str(verifier), "--android-root", str(android_root))
        print(f"regenerated {args.route_id}: final tree {final_tree}")
    except (OSError, UnicodeError, RegenerationError, patch_series.SeriesError) as error:
        raise SystemExit(f"AOSP patch regenerator: {error}") from error


if __name__ == "__main__":
    main()
