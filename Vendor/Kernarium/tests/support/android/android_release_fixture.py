#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble a small Android release for the host block-layer tests.

The release is what android/lineage-24/tools/ubd-disk.py produces from the
tracked disk policy, with synthetic partition contents: kernelless boot image
v4 containers for init_boot and recovery, and filler for the filesystems. A
reference data disk made by the same tool lets the tests compare the host's
partition tables with the Python builder's.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path

RAMDISK_SIZES = {"init_boot": 8192, "recovery": 12288}
FILESYSTEM_SIZES = {
    "system": 65537,
    "system_ext": 40960,
    "product": 12289,
    "vendor": 20480,
    "odm": 4097,
}
COMMAND_LINE = (
    "console=ttyS0 androidboot.hardware=kernarium "
    "androidboot.boot_devices=uml-blkdev.0,uml-blkdev.1\n"
)


def boot_image(ramdisk_size: int) -> bytes:
    header = bytearray(4096)
    header[0:8] = b"ANDROID!"
    struct.pack_into("<I", header, 12, ramdisk_size)
    struct.pack_into("<I", header, 20, 1584)
    struct.pack_into("<I", header, 40, 4)
    return bytes(header) + bytes((index * 7 + 1) & 0xFF for index in range(ramdisk_size))


def filler(name: str, size: int) -> bytes:
    seed = sum(name.encode())
    return bytes((seed + index * 13) & 0xFF for index in range(size))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    root = args.android_root.resolve()
    output = args.output.resolve()
    if output.exists():
        shutil.rmtree(output)
    images = output / "images"
    images.mkdir(parents=True)
    for name, size in RAMDISK_SIZES.items():
        (images / f"{name}.img").write_bytes(boot_image(size))
    for name, size in FILESYSTEM_SIZES.items():
        (images / f"{name}.img").write_bytes(filler(name, size))

    tool = root / "tools/ubd-disk.py"
    release_guest = output / "release/guest"
    subprocess.run(
        [
            sys.executable, "-B", str(tool), "assemble",
            "--policy", str(root / "support/image/disk-policy.json"),
            "--images-dir", str(images),
            "--output-dir", str(release_guest),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL,
    )
    (output / "release/boot").mkdir()
    (output / "release/boot/kernel-command-line.txt").write_text(COMMAND_LINE)
    subprocess.run(
        [
            sys.executable, "-B", str(tool), "data",
            "--layout", str(release_guest / "partition-layout.json"),
            "--capacity-gib", "4",
            "--output", str(output / "reference-data-4.img"),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL,
    )


if __name__ == "__main__":
    main()
