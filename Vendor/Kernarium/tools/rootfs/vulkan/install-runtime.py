#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install a receipt-verified guest Vulkan runtime into an image root.

The receipt is the one `stage-runtime.py` emitted for the same staged runtime
root.  Every installed file is checked against it before the image is touched,
the runtime's dynamic dependencies must resolve inside the image, and only the
Virtio ICD is left selectable so a software driver cannot mask a Venus failure.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any


ARTIFACT = "uml-vulkan-guest-runtime"
SCHEMA_VERSION = 1
METADATA = "/usr/share/uml-vulkan/guest-runtime.json"


class RuntimeImportError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise RuntimeImportError(message)


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def guest_path(root: Path, value: str) -> Path:
    relative = Path(value[1:]) if value.startswith("/") else Path(value)
    if not value.startswith("/") or any(
        part in {"", ".", ".."} for part in relative.parts
    ):
        fail(f"not a normalized guest path: {value!r}")
    return root / relative


def read_receipt(path: Path, architecture: str | None) -> dict[str, Any]:
    receipt = json.loads(path.read_text(encoding="utf-8"))
    if (receipt.get("schema_version") != SCHEMA_VERSION
            or receipt.get("artifact") != ARTIFACT):
        fail(f"not a guest Vulkan runtime receipt: {path}")
    described = receipt["architecture"]["debian"]
    if architecture is not None and described != architecture:
        fail(f"receipt describes {described}, not {architecture}")
    if receipt["mesa"]["portability_driver"]:
        fail("receipt describes a portability driver")
    return receipt


def install(source: Path, receipt_path: Path, root: Path,
            architecture: str | None) -> dict[str, Any]:
    receipt = read_receipt(receipt_path, architecture)
    records = receipt["mesa"]["files"] + [receipt["vulkan_loader"]["runtime"]]
    # Authenticate the complete payload before changing the image.
    for record in records:
        path = guest_path(source, record["path"])
        if (path.is_symlink() or not path.is_file()
                or path.stat().st_size != record["size"]
                or digest(path) != record["sha256"]):
            fail(f"Vulkan runtime does not match its receipt: {record['path']}")
    for record in records:
        destination = guest_path(root, record["path"])
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.is_symlink():
            destination.unlink()
        shutil.copy2(guest_path(source, record["path"]), destination)
    for path_in_guest, target in sorted(receipt["symlinks"].items()):
        link = guest_path(root, path_in_guest)
        link.parent.mkdir(parents=True, exist_ok=True)
        link.unlink(missing_ok=True)
        link.symlink_to(target)
    # Force the intended device: a software ICD must not mask Venus failure.
    selected = Path(receipt["selected_icd_manifest"]).name
    for path in sorted(guest_path(root, "/usr/share/vulkan/icd.d").glob("*.json")):
        if path.name != selected:
            path.unlink()
    subprocess.run(["chroot", str(root), "ldconfig"], check=True)
    for record in records:
        if ".so" not in record["path"]:
            continue
        result = subprocess.run(["chroot", str(root), "ldd", record["path"]],
                                text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=True)
        if "not found" in result.stdout:
            fail(f"Vulkan runtime dependency missing: {result.stdout}")
    metadata = guest_path(root, METADATA)
    metadata.parent.mkdir(parents=True, exist_ok=True)
    value = {
        "source_receipt_sha256": digest(receipt_path),
        "architecture": receipt["architecture"],
        "mesa": receipt["mesa"],
        "protocol": receipt["protocol"],
        "vulkan_loader": receipt["vulkan_loader"],
        "installed_files": records,
        "symlinks": receipt["symlinks"],
    }
    metadata.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    return {"installed_files": len(records),
            "symlinks": len(receipt["symlinks"]),
            "architecture": receipt["architecture"]["debian"],
            "metadata": METADATA}


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--architecture")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    options = parse_arguments(argv)
    try:
        print(json.dumps(install(options.source.resolve(strict=True),
                                 options.receipt.resolve(strict=True),
                                 options.root.resolve(strict=True),
                                 options.architecture), indent=2))
    except (OSError, UnicodeError, KeyError, json.JSONDecodeError,
            subprocess.CalledProcessError, RuntimeImportError) as error:
        print(f"install-runtime.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
