#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Authenticate a rootfs artifact before provisioning it."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = ROOT / "tools" / "rootfs" / "manifest.json"


class VerificationError(RuntimeError):
    """The image does not match the checked-in rootfs manifest."""


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as image:
        while chunk := image.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def integer(value: Any, field: str) -> int:
    if not isinstance(value, int):
        raise VerificationError(f"manifest field {field} must be an integer")
    return value


def text(value: Any, field: str) -> str:
    if not isinstance(value, str):
        raise VerificationError(f"manifest field {field} must be a string")
    return value


def verify(image: Path, manifest_path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise VerificationError(f"cannot read rootfs manifest {manifest_path}: {error}") from error
    if not isinstance(manifest, dict):
        raise VerificationError("rootfs manifest must be a JSON object")
    if manifest.get("schema_version") != 4:
        raise VerificationError("rootfs manifest has an unsupported schema")
    if text(manifest.get("image"), "image") != "rootfs.erofs":
        raise VerificationError("rootfs manifest does not name rootfs.erofs")
    if not image.is_file():
        raise VerificationError(f"rootfs is not a regular file: {image}")
    details = image.stat()
    expected_size = integer(manifest.get("size"), "size")
    if details.st_size != expected_size:
        raise VerificationError(f"rootfs size is {details.st_size}, expected {expected_size}")
    actual_digest = digest(image)
    expected_digest = text(manifest.get("sha256"), "sha256")
    if actual_digest != expected_digest:
        raise VerificationError("rootfs SHA-256 does not match manifest")
    # The digest fixes every byte of the image, its EROFS superblock included.
    return {
        "image": str(image),
        "sha256": actual_digest,
        "size": details.st_size,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=ROOT / "rootfs.erofs")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--quiet", action="store_true")
    arguments = parser.parse_args()
    try:
        result = verify(arguments.image.resolve(), arguments.manifest.resolve())
    except VerificationError as error:
        print(f"rootfs verification failed: {error}", file=sys.stderr)
        return 1
    if not arguments.quiet:
        print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
