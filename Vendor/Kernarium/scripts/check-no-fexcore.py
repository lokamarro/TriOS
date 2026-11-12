#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fail if an iOS product carries FEXCore.

The x86-64 JIT embeds FEXCore and is a macOS product only. Every Mach-O
binary and static archive below each path must name no FEXCore symbol and
hold none of FEXCore's own allocation names, which survive symbol stripping.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
import sys
from pathlib import Path


# Mach-O magics of both byte orders, thin and universal, and the ar magic.
MAGICS = (
    bytes.fromhex("feedfacf"),
    bytes.fromhex("cffaedfe"),
    bytes.fromhex("feedface"),
    bytes.fromhex("cefaedfe"),
    bytes.fromhex("cafebabe"),
    bytes.fromhex("bebafeca"),
    b"!<arch>\n",
)
# The names FEXCore gives its code and backend mappings.
MARKERS = (b"FEXMemJIT", b"FEXMem_CPUBackend")
NM_TIMEOUT_SECONDS = 300


def binaries(root: Path) -> list[Path]:
    candidates = [root] if root.is_file() else sorted(root.rglob("*"))
    found = []
    for path in candidates:
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as handle:
            head = handle.read(8)
        if any(head.startswith(magic) for magic in MAGICS):
            found.append(path)
    return found


def problems(path: Path, nm: list[str]) -> list[str]:
    found = []
    data = path.read_bytes()
    for marker in MARKERS:
        if marker in data:
            found.append(f"{path}: holds FEXCore's {marker.decode()} name")
    listed = subprocess.run(
        [*nm, "-a", "-P", str(path)],
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
        timeout=NM_TIMEOUT_SECONDS,
    )
    if listed.returncode != 0:
        found.append(f"{path}: nm failed: {listed.stderr.strip()}")
        return found
    symbols = [
        line.split()[0]
        for line in listed.stdout.splitlines()
        if line.split() and "FEXCore" in line.split()[0]
    ]
    if symbols:
        found.append(
            f"{path}: names {len(symbols)} FEXCore symbol(s), "
            f"first {symbols[0]}"
        )
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--nm",
        default="xcrun nm",
        help="nm command that lists Mach-O files and archives",
    )
    parser.add_argument("paths", nargs="+", type=Path)
    arguments = parser.parse_args()

    nm = arguments.nm.split()
    checked = 0
    failures = []
    for root in arguments.paths:
        if not root.exists():
            failures.append(f"{root}: does not exist")
            continue
        found = binaries(root)
        if not found:
            failures.append(f"{root}: holds no Mach-O binary or archive")
        for path in found:
            checked += 1
            failures.extend(problems(path, nm))
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"no FEXCore in {checked} binaries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
