#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import struct
from dataclasses import asdict
from pathlib import Path
from typing import BinaryIO

import android_image


COPY_CHUNK = 1024 * 1024


class BootImageError(ValueError):
    pass


def compare_ramdisk(
    source: BinaryIO, image: android_image.BootImage, expected_path: Path
) -> None:
    if expected_path.is_symlink() or not expected_path.is_file():
        raise BootImageError(f"ramdisk must be a regular non-symlink file: {expected_path}")
    if expected_path.stat().st_size != image.ramdisk_size:
        raise BootImageError("embedded ramdisk size differs from the expected input")
    source.seek(image.ramdisk_offset)
    with expected_path.open("rb") as expected:
        remaining = image.ramdisk_size
        while remaining:
            size = min(remaining, COPY_CHUNK)
            actual_block = source.read(size)
            expected_block = expected.read(size)
            if actual_block != expected_block or len(actual_block) != size:
                raise BootImageError("embedded ramdisk differs from the expected input")
            remaining -= size
        if expected.read(1):
            raise BootImageError("expected ramdisk grew while it was compared")


def regular_file(path: Path, label: str) -> Path:
    if path.is_symlink() or not path.is_file():
        raise BootImageError(f"{label} must be a regular non-symlink file: {path}")
    return path.resolve()


def inspect(args: argparse.Namespace) -> tuple[Path, android_image.BootImage]:
    if args.image is not None:
        if args.disk is not None or args.layout is not None or args.partition is not None:
            raise BootImageError("--image cannot be combined with disk partition arguments")
        path = regular_file(args.image, "boot image")
        offset, size = 0, path.stat().st_size
    else:
        if args.disk is None or args.layout is None or args.partition is None:
            raise BootImageError("disk inspection requires --disk, --layout, and --partition")
        path = regular_file(args.disk, "disk image")
        layout_path = regular_file(args.layout, "partition layout")
        offset, size = android_image.partition_range(
            layout_path, path, args.partition, BootImageError
        )
    with path.open("rb") as source:
        image = android_image.parse_boot_image(source, offset, size, BootImageError)
        if args.ramdisk is not None:
            compare_ramdisk(source, image, args.ramdisk)
    return path, image


def add_source_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--image", type=Path)
    parser.add_argument("--disk", type=Path)
    parser.add_argument("--layout", type=Path)
    parser.add_argument("--partition")
    parser.add_argument("--ramdisk", type=Path)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Inspect kernelless Android boot-image v4 containers"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    verify_parser = subparsers.add_parser("verify")
    add_source_arguments(verify_parser)
    range_parser = subparsers.add_parser("range")
    add_source_arguments(range_parser)
    args = parser.parse_args()
    try:
        path, image = inspect(args)
        if args.command == "range":
            print(json.dumps(asdict(image), sort_keys=True, separators=(",", ":")))
        else:
            print(
                f"verified kernelless Android boot image v4: {path}, "
                f"ramdisk={image.ramdisk_offset}+{image.ramdisk_size}"
            )
    except (BootImageError, OSError, struct.error) as error:
        raise SystemExit(f"Android boot image: {error}") from error


if __name__ == "__main__":
    main()
