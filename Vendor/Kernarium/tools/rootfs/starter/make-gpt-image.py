#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack a root directory into a minimal one-partition GPT/ext4 disk."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from gpt import SECTOR_SIZE, inspect

MIB = 1024 * 1024
EXT4_BLOCK_SIZE = 4096
PARTITION_START_LBA = 2048
GPT_TRAILING_SECTORS = 33


def fail(message: str) -> None:
    raise SystemExit(f"starter GPT image builder: {message}")


def run(*arguments: str, capture: bool = False) -> str:
    result = subprocess.run(
        arguments,
        check=False,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() if capture and result.stderr else ""
        command = " ".join(arguments)
        suffix = f": {detail}" if detail else ""
        fail(f"command failed ({result.returncode}): {command}{suffix}")
    return result.stdout if capture else ""


def aligned(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def directory_bytes(root: Path) -> int:
    output = run("du", "-sx", "-B1", str(root), capture=True)
    try:
        return int(output.split()[0])
    except (IndexError, ValueError) as error:
        fail(f"cannot parse root-directory size: {output.strip()}")
        raise AssertionError from error


def ext4_geometry(path: Path) -> tuple[int, int]:
    output = run("dumpe2fs", "-h", str(path), capture=True)
    values: dict[str, int] = {}
    for line in output.splitlines():
        key, separator, value = line.partition(":")
        if separator and key in {"Block count", "Block size"}:
            values[key] = int(value.strip())
    if set(values) != {"Block count", "Block size"}:
        fail("dumpe2fs did not report ext4 block geometry")
    return values["Block count"], values["Block size"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--label", required=True)
    parser.add_argument("--headroom-mib", required=True, type=int)
    arguments = parser.parse_args()

    root = arguments.root.resolve()
    output = arguments.output.resolve()
    if not root.is_dir() or root.is_symlink():
        fail(f"root is not a real directory: {root}")
    if output.exists() or output.is_symlink():
        fail(f"refusing to replace output: {output}")
    if not arguments.label or len(arguments.label.encode("utf-8")) > 16:
        fail("ext4 label must occupy 1 through 16 UTF-8 bytes")
    if arguments.headroom_mib < 1:
        fail("headroom must be positive")
    for command in ("dd", "du", "dumpe2fs", "e2fsck", "mkfs.ext4", "resize2fs", "sgdisk"):
        if shutil.which(command) is None:
            fail(f"required command is unavailable: {command}")

    output.parent.mkdir(parents=True, exist_ok=True)
    tree_bytes = directory_bytes(root)
    initial_bytes = aligned(
        max(256 * MIB, (tree_bytes * 3) // 2 + 128 * MIB), MIB
    )
    with tempfile.TemporaryDirectory(prefix=f".{output.name}.", dir=output.parent) as name:
        temporary = Path(name)
        filesystem = temporary / "root.ext4"
        staged = temporary / output.name
        with filesystem.open("wb") as target:
            target.truncate(initial_bytes)
        run(
            "mkfs.ext4",
            "-F",
            "-q",
            "-b",
            str(EXT4_BLOCK_SIZE),
            "-m",
            "0",
            "-L",
            arguments.label,
            "-E",
            "lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0",
            "-d",
            str(root),
            str(filesystem),
        )
        run("e2fsck", "-f", "-y", str(filesystem))
        run("resize2fs", "-M", str(filesystem))
        minimum_blocks, block_size = ext4_geometry(filesystem)
        if block_size != EXT4_BLOCK_SIZE:
            fail(f"unexpected ext4 block size: {block_size}")
        headroom_blocks = aligned(arguments.headroom_mib * MIB, block_size) // block_size
        target_blocks = minimum_blocks + headroom_blocks
        with filesystem.open("r+b") as target:
            target.truncate(target_blocks * block_size)
        run("resize2fs", str(filesystem), str(target_blocks))
        run("e2fsck", "-f", "-y", str(filesystem))

        partition_sectors = target_blocks * block_size // SECTOR_SIZE
        disk_sectors = PARTITION_START_LBA + partition_sectors + GPT_TRAILING_SECTORS
        with staged.open("wb") as target:
            target.truncate(disk_sectors * SECTOR_SIZE)
        run(
            "sgdisk",
            "--clear",
            "--set-alignment=1",
            f"--new=1:{PARTITION_START_LBA}:0",
            "--typecode=1:8300",
            "--change-name=1:rootfs",
            str(staged),
        )
        run(
            "dd",
            f"if={filesystem}",
            f"of={staged}",
            f"bs={SECTOR_SIZE}",
            f"seek={PARTITION_START_LBA}",
            "conv=notrunc,sparse",
            "status=none",
        )
        run("sgdisk", "--verify", str(staged))
        result = inspect(staged)
        os.replace(staged, output)
    result["image"] = str(output)
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
