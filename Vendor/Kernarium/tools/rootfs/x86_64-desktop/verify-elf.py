#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Inventory ELF files and reject payloads outside the x86 family."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
from pathlib import Path
import stat
import sys


ELF_MAGIC = b"\x7fELF"
ELF_HEADER_MACHINE_END = 20
ELFDATA2LSB = 1
MACHINES = {
    3: "Intel 80386",
    62: "Advanced Micro Devices X86-64",
}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def inventory(root: Path) -> list[tuple[str, str]]:
    entries: list[tuple[str, str]] = []
    for directory, _, filenames in os.walk(root, followlinks=False):
        for filename in filenames:
            path = Path(directory, filename)
            try:
                mode = path.stat(follow_symlinks=False).st_mode
            except FileNotFoundError:
                continue
            if not stat.S_ISREG(mode):
                continue
            with path.open("rb") as stream:
                header = stream.read(ELF_HEADER_MACHINE_END)
            if not header.startswith(ELF_MAGIC):
                continue
            relative = str(path.relative_to(root))
            if "\t" in relative or "\n" in relative:
                raise RuntimeError(f"ELF path is not TSV-safe: {relative!r}")
            if len(header) < ELF_HEADER_MACHINE_END:
                raise RuntimeError(f"truncated ELF header: {relative}")
            if header[5] != ELFDATA2LSB:
                raise RuntimeError(f"non-little-endian ELF payload: {relative}")
            machine_number = int.from_bytes(header[18:20], "little")
            try:
                machine = MACHINES[machine_number]
            except KeyError as error:
                raise RuntimeError(
                    f"non-x86 ELF payload: {relative}: e_machine={machine_number}"
                ) from error
            entries.append((relative, machine))
    return sorted(entries)


def main() -> int:
    options = parse_arguments()
    root = options.root.resolve(strict=True)
    if not root.is_dir():
        raise RuntimeError(f"root is not a directory: {root}")
    output = options.output.resolve(strict=False)
    if output.is_symlink():
        raise RuntimeError(f"refusing symlink output: {output}")
    entries = inventory(root)
    if not entries:
        raise RuntimeError(f"root contains no ELF payloads: {root}")
    with output.open("w", encoding="utf-8", newline="\n") as stream:
        for relative, machine in entries:
            stream.write(f"{relative}\t{machine}\n")
    print(f"Verified {len(entries)} x86-family ELF payloads")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"{Path(sys.argv[0]).name}: {error}", file=sys.stderr)
        raise SystemExit(1) from error
