#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Render the Repo local manifest that pins a workspace to the base manifest.

The base manifest is a `repo manifest -r` snapshot. A project the manifest
reads at a branch moves with that branch, so a fresh sync would not reproduce
the snapshot; a project read at a tag cannot move. The rendered local manifest
extends each branch-read project with the snapshot's commit and upstream
branch, which makes `repo sync` check out exactly the snapshot.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import xml.etree.ElementTree as ET
from pathlib import Path

import pinned_source


class RenderError(ValueError):
    pass


def render(base_manifest: Path) -> str:
    projects = pinned_source.manifest_projects(
        ET.parse(base_manifest).getroot(), RenderError
    )
    lines = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>", "<manifest>"]
    for path, project in sorted(projects.items()):
        upstream = project.get("upstream", "")
        revision = project.get("revision", "")
        if pinned_source.HEX40.fullmatch(revision) is None:
            raise RenderError(f"base manifest project is not pinned to a commit: {path}")
        if not upstream or upstream.startswith("refs/tags/"):
            continue
        element = ET.Element("extend-project", {
            "name": project.get("name", ""),
            "path": path,
            "revision": revision,
            "upstream": upstream,
        })
        lines.append("  " + ET.tostring(element, encoding="unicode"))
    lines.append("</manifest>")
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        text = render(args.base_manifest)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    except (OSError, ET.ParseError, RenderError) as error:
        raise SystemExit(f"render-pin-manifest.py: {error}") from error


if __name__ == "__main__":
    main()
