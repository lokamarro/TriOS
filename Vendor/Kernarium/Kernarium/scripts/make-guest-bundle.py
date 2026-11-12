#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble a Kernarium guest bundle from an installed Android release.

A bundle is a directory the application can boot on its own: the release's
read-only system disk, its partition layout and kernel command line. The
runtime creates the guest's writable data disk (guest/data.img) at the first
launch and only grows it afterwards. The kernel is not part of the bundle: the
application carries an independently accepted Android kernel artifact in its
signed runtime framework.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

BUNDLE_SUFFIX = ".kern"
MANIFEST = "release-manifest.json"
COMMAND_LINE = "boot/kernel-command-line.txt"
SYSTEM_DISK = "guest/system.img"
LAYOUT = "guest/partition-layout.json"
ROOT = Path(__file__).resolve().parents[2]
ACCEPTANCE = ROOT / "android/lineage-24/metadata/accepted-release.env"
_ARTIFACTS = str(ROOT / "kernel/common/artifacts")
if _ARTIFACTS not in sys.path:
    sys.path.insert(0, _ARTIFACTS)
import contract_checks  # noqa: E402


def fail(message: str) -> None:
    raise SystemExit(f"Kernarium/scripts/make-guest-bundle.py: {message}")


def accepted_directory_name() -> str:
    return contract_checks.accepted_directory_name(
        ACCEPTANCE,
        "LINEAGE24_ACCEPTED_DIRECTORY_NAME",
        lambda message: SystemExit(
            f"Kernarium/scripts/make-guest-bundle.py: {message}"
        ),
    )


def copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    # A clone shares the release's blocks instead of copying them.
    cloned = subprocess.run(
        ["cp", "-c", str(source), str(destination)],
        capture_output=True,
        check=False,
    )
    if cloned.returncode != 0:
        shutil.copyfile(source, destination)
    print(f"  {source.name} -> {destination}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--release", type=Path)
    parser.add_argument("--replace", action="store_true")
    arguments = parser.parse_args()

    release = arguments.release
    if release is None:
        release = ROOT / ".build/android/releases" / accepted_directory_name()
    release = release.resolve()
    if not release.is_dir():
        fail(f"the Android release is not installed: {release}")

    output = arguments.output
    if output.suffix != BUNDLE_SUFFIX:
        fail(f"a guest bundle must end in {BUNDLE_SUFFIX}: {output}")
    if output.exists():
        if not arguments.replace:
            fail(f"{output} already exists; pass --replace to rebuild it")
        # Replacing discards the data disk with the payload it belongs to.
        shutil.rmtree(output)

    manifest = json.loads((release / MANIFEST).read_text(encoding="utf-8"))
    for relative in (MANIFEST, COMMAND_LINE, SYSTEM_DISK, LAYOUT):
        if not (release / relative).is_file():
            fail(f"the release lacks {relative}: {release}")
    print(
        f"assembling {output.name} from {manifest['product_family']} "
        f"{manifest['release_id']}"
    )
    output.mkdir(parents=True)
    for relative in (MANIFEST, COMMAND_LINE, SYSTEM_DISK, LAYOUT):
        copy(release / relative, output / relative)
    print(
        f"guest bundle ready: {output}\n"
        "  the application creates guest/data.img at the first boot"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
