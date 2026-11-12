#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage the accepted Android release as the system image Andarium carries.

The application bundles the staged directory as it stands, laid out as the
installed release: the read-only system disk `guest/system.img`, its
`guest/partition-layout.json`, `boot/kernel-command-line.txt` and
`release-manifest.json`. The runtime boots from that directory and creates the
writable data disk beside the guest's own files. The staged files are copies of
an accepted release, which is checked against its own manifest and layout; this
script neither builds nor signs anything.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ACCEPTANCE = ROOT / "android/lineage-24/metadata/accepted-release.env"
DEFAULT_RELEASES = ROOT / ".build/android/releases"
DEFAULT_OUTPUT = ROOT / ".build/andarium-system"
_ARTIFACTS = str(ROOT / "kernel/common/artifacts")
if _ARTIFACTS not in sys.path:
    sys.path.insert(0, _ARTIFACTS)
import contract_checks  # noqa: E402

MANIFEST = "release-manifest.json"
COMMAND_LINE = "boot/kernel-command-line.txt"
PARTITION_LAYOUT = "guest/partition-layout.json"
SYSTEM_DISK = "guest/system.img"
SYSTEM_DISK_ROLE = "system-disk"
FILES = (MANIFEST, COMMAND_LINE, PARTITION_LAYOUT, SYSTEM_DISK)
LAYOUT_SCHEMA = 2
GUEST_PAGE_SIZE = 4096


class StageError(Exception):
    pass


def fail(message: str) -> None:
    raise SystemExit(f"Andarium/scripts/stage-system-image.py: {message}")


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise StageError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise StageError(f"{path} is not a JSON object")
    return value


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 22), b""):
            digest.update(block)
    return digest.hexdigest()


def check_release(release: Path, manifest: dict, accepted: dict[str, str] | None) -> None:
    """Checks the release against its manifest and layout; raises StageError."""
    if manifest.get("acceptance") != "accepted":
        raise StageError("the release is not accepted")
    android = manifest.get("android")
    kernel = manifest.get("kernel")
    if not isinstance(android, dict) or not isinstance(kernel, dict):
        raise StageError("the manifest has no android or kernel section")
    if kernel.get("page_size") != GUEST_PAGE_SIZE:
        raise StageError(f"the release is not a {GUEST_PAGE_SIZE}-byte-page release")
    if not str(manifest.get("product_family", "")).startswith("lineage-"):
        raise StageError("the release is not a LineageOS product family")
    if accepted is not None:
        expected = {
            "release_id": accepted["LINEAGE24_ACCEPTED_RELEASE_ID"],
            "product_family": accepted["LINEAGE24_ACCEPTED_PRODUCT_FAMILY"],
        }
        for key, value in expected.items():
            if manifest.get(key) != value:
                raise StageError(f"{key} {manifest.get(key)!r} is not the accepted {value!r}")
        if str(android.get("major")) != accepted["LINEAGE24_ACCEPTED_ANDROID_MAJOR"]:
            raise StageError("the Android major version is not the accepted one")

    inventory = manifest.get("inventory")
    if not isinstance(inventory, list):
        raise StageError("the manifest has no inventory")
    by_path = {entry.get("path"): entry for entry in inventory if isinstance(entry, dict)}
    disks = [entry for entry in by_path.values() if entry.get("role") == SYSTEM_DISK_ROLE]
    if len(disks) != 1 or disks[0].get("path") != SYSTEM_DISK:
        raise StageError(f"expected one {SYSTEM_DISK_ROLE} at {SYSTEM_DISK} in the inventory")
    for relative in (COMMAND_LINE, PARTITION_LAYOUT, SYSTEM_DISK):
        path = release / relative
        if not path.is_file() or path.is_symlink():
            raise StageError(f"missing or unsafe release file: {path}")
        entry = by_path.get(relative)
        if entry is None:
            raise StageError(f"{relative} is not in the manifest inventory")
        if path.stat().st_size != entry.get("size"):
            raise StageError(f"{relative} does not match the size in the manifest")

    layout = read_json(release / PARTITION_LAYOUT)
    if layout.get("schema_version") != LAYOUT_SCHEMA:
        raise StageError(f"the partition layout is not schema {LAYOUT_SCHEMA}")
    system = layout.get("system_disk")
    if not isinstance(system, dict):
        raise StageError("the partition layout has no system_disk")
    if (release / SYSTEM_DISK).stat().st_size != system.get("size_bytes"):
        raise StageError("the system disk does not match the size in the partition layout")
    if sha256_of(release / SYSTEM_DISK) != system.get("sha256"):
        raise StageError("the system disk does not match the sha256 in the partition layout")


def copy(source: Path, destination: Path) -> None:
    # A clone costs nothing on APFS; elsewhere the bytes are copied.
    cloned = subprocess.run(
        ["cp", "-c", str(source), str(destination)], capture_output=True, check=False
    )
    if cloned.returncode != 0:
        shutil.copyfile(source, destination)


def stage(release: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent))
    try:
        for relative in FILES:
            (scratch / relative).parent.mkdir(parents=True, exist_ok=True)
            copy(release / relative, scratch / relative)
        previous = None
        if output.exists():
            previous = Path(tempfile.mkdtemp(prefix=f".{output.name}.old.", dir=output.parent))
            os.rmdir(previous)
            os.rename(output, previous)
        try:
            os.rename(scratch, output)
        except OSError:
            if previous is not None:
                os.rename(previous, output)
            raise
        if previous is not None:
            shutil.rmtree(previous, ignore_errors=True)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--release", type=Path, help="a release directory (default: the accepted release)")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--acceptance", type=Path, default=DEFAULT_ACCEPTANCE, help=argparse.SUPPRESS)
    parser.add_argument("--releases", type=Path, default=DEFAULT_RELEASES, help=argparse.SUPPRESS)
    arguments = parser.parse_args(argv)

    try:
        accepted = None
        release = arguments.release
        if release is None:
            accepted = contract_checks.parse_assignments(arguments.acceptance, StageError)
            release = arguments.releases / accepted["LINEAGE24_ACCEPTED_DIRECTORY_NAME"]
        release = release.resolve()
        if not release.is_dir():
            raise StageError(f"the Android release is not installed: {release}")
        manifest = read_json(release / MANIFEST)
        check_release(release, manifest, accepted)
        stage(release, arguments.output)
    except StageError as error:
        fail(str(error))
    print(f"staged {manifest['product_family']} {manifest['release_id']}: {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
