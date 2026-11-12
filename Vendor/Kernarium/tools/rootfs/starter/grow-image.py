#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Transactionally grow a starter disk and its sole GPT partition on the host."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import shutil
import stat
import subprocess
import tempfile
from pathlib import Path

from gpt import ImageError, inspect

GIB = 1024 * 1024 * 1024


def fail(message: str) -> None:
    raise SystemExit(f"starter image grower: {message}")


def run(*arguments: str) -> None:
    result = subprocess.run(
        arguments,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        fail(f"command failed ({result.returncode}): {' '.join(arguments)}: {detail}")


def clone(source: Path, destination: Path) -> None:
    attempts = []
    if shutil.which("cp") is not None:
        attempts = [
            ["cp", "-c", str(source), str(destination)],
            ["cp", "--reflink=auto", "--sparse=always", str(source), str(destination)],
        ]
    for command in attempts:
        result = subprocess.run(command, check=False, capture_output=True)
        if result.returncode == 0:
            break
        destination.unlink(missing_ok=True)
    else:
        shutil.copyfile(source, destination)
    # cp keeps the source mode, and published images are read-only. The copy
    # is the disk being enlarged, so its owner must be able to write it.
    destination.chmod(stat.S_IMODE(destination.stat().st_mode) | stat.S_IWUSR)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--size-gib", required=True, type=int)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()

    source = arguments.image.resolve()
    try:
        before = inspect(source)
    except (ImageError, OSError) as error:
        fail(str(error))
    if arguments.size_gib < 1:
        fail("target size must be positive")
    requested = arguments.size_gib * GIB
    current = int(before["size"])
    if requested <= current:
        fail(f"target must grow the image beyond {current} bytes")
    if shutil.which("sgdisk") is None:
        fail("sgdisk is required on the host")

    destination = arguments.output.resolve() if arguments.output else source
    if arguments.output and (destination.exists() or destination.is_symlink()):
        fail(f"refusing to replace output: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    partition = before["partition"]
    assert isinstance(partition, dict)

    temporary_directory = Path(
        tempfile.mkdtemp(prefix=f".{destination.name}.grow.", dir=destination.parent)
    )
    staged = temporary_directory / destination.name
    try:
        clone(source, staged)
        with staged.open("r+b") as image:
            image.truncate(requested)
            image.flush()
            os.fsync(image.fileno())
        run("sgdisk", "--move-second-header", str(staged))
        name = str(partition["name"])
        run(
            "sgdisk",
            "--delete=1",
            f"--new=1:{partition['first_lba']}:0",
            f"--typecode=1:{partition['type_guid']}",
            f"--partition-guid=1:{partition['unique_guid']}",
            f"--change-name=1:{name}",
            str(staged),
        )
        run("sgdisk", "--verify", str(staged))
        after = inspect(staged)
        after_partition = after["partition"]
        assert isinstance(after_partition, dict)
        after_filesystem = after["filesystem"]
        before_filesystem = before["filesystem"]
        assert isinstance(after_filesystem, dict) and isinstance(before_filesystem, dict)
        if after_partition["first_lba"] != partition["first_lba"]:
            fail("GPT growth moved the partition start")
        if after_filesystem["bytes"] != before_filesystem["bytes"]:
            fail("host growth changed the ext4 filesystem")
        os.replace(staged, destination)
        temporary_directory.rmdir()
    except BaseException:
        shutil.rmtree(temporary_directory, ignore_errors=True)
        raise
    after["image"] = str(destination)
    print(json.dumps(after, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
