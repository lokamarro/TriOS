#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record a synchronized Repo workspace as the tracked source identity.

Run it on a workspace whose projects are at their synchronized revisions,
before any routed patch is applied. It writes the `repo manifest -r` snapshot
as `metadata/base-manifest.xml` and updates the manifest commit, manifest tree
and snapshot digest in `metadata/lineage.env`. The routes then have to be
moved onto the new bases, which `verify-aosp-patches.py` reports.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path

import pinned_source
from contract_checks import parse_assignments, replace_assignment


class SnapshotError(ValueError):
    pass


def run(*command: str, cwd: Path) -> str:
    return pinned_source.run(*command, cwd=cwd, error=SnapshotError)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--aosp-source", type=Path, required=True)
    args = parser.parse_args()
    try:
        android_root = args.android_root.resolve()
        source = args.aosp_source.resolve()
        manifests = source / ".repo/manifests"
        if not manifests.is_dir():
            raise SnapshotError("source is not a Repo workspace")
        dirty = run(
            "repo", "forall", "-c", pinned_source.DIRTY_PROJECTS, cwd=source
        )
        if dirty:
            raise SnapshotError(f"projects have uncommitted changes: {dirty}")
        snapshot = run("repo", "manifest", "-r", cwd=source) + "\n"
        metadata_path = android_root / "metadata/lineage.env"
        parse_assignments(metadata_path, SnapshotError)
        manifest_path = android_root / "metadata/base-manifest.xml"
        manifest_path.write_text(snapshot, encoding="utf-8")
        values = {
            "LINEAGE24_MANIFEST_COMMIT": run("git", "rev-parse", "HEAD", cwd=manifests),
            "LINEAGE24_MANIFEST_TREE": run(
                "git", "rev-parse", "HEAD^{tree}", cwd=manifests
            ),
            "LINEAGE24_BASE_MANIFEST_SHA256": pinned_source.digest(
                manifest_path, SnapshotError
            ),
        }
        text = metadata_path.read_text(encoding="utf-8")
        for key, value in values.items():
            text = replace_assignment(text, key, f"'{value}'", SnapshotError)
        metadata_path.write_text(text, encoding="utf-8")
    except (OSError, UnicodeError, SnapshotError) as error:
        raise SystemExit(f"snapshot-source.py: {error}") from error
    print(f"recorded {snapshot.count('<project ')} projects as the source identity")


if __name__ == "__main__":
    main()
