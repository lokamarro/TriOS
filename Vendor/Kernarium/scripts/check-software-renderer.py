#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check where the software renderer is.

The iOS Simulator's GPU product renders on the CPU, because the Simulator has
no Metal 4 device for KosmicKrisp. Only its slices may carry Mesa's software
rasterizers or the LLVM that llvmpipe JITs through, and a device or macOS
product must never fall back to either. Every Mach-O binary and static archive
below each path is inspected:

  --absent   none may define a software Gallium screen or an LLVM symbol, or
             hold the host provider's marker string, so the product can
             neither select the rasterizer nor JIT;
  --present  at least one must define a software Gallium screen, so a
             Simulator slice that lost its rasterizer is caught.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
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
# The software Gallium screens Mesa can be built with: the two the Simulator
# prefix names (dependencies/build-apple-gpu.sh). A Zink build has neither.
SCREEN_CREATORS = ("softpipe_create_screen", "llvmpipe_create_screen")
# LLVM's C API and its C++ namespace, which gallivm links statically into the
# Simulator's MesaGallium alone. The leading underscores of a Mach-O name are
# removed before the match.
LLVM_SYMBOL = re.compile(r"^(LLVM[A-Za-z]|ZN4llvm)")
# The host GL provider names itself with this line when it is the software
# one (darwin/device/gpu/host_gl_provider.cpp).
PROVIDER_MARKER = b"Virgl software rasterizer provider"
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


def screens(path: Path, nm: list[str]) -> tuple[list[str], list[str]] | str:
    """The software screen creators and LLVM symbols the binary defines (the
    first of each is enough to report), or nm's failure."""
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
        return f"{path}: nm failed: {listed.stderr.strip()}"
    found = []
    llvm = []
    for line in listed.stdout.splitlines():
        fields = line.split()
        # A defined symbol has a type letter other than U (undefined).
        if len(fields) < 2 or fields[1] in ("U", "u"):
            continue
        name = fields[0].lstrip("_")
        if name in SCREEN_CREATORS and name not in found:
            found.append(name)
        elif not llvm and LLVM_SYMBOL.match(name):
            llvm.append(fields[0])
    return found, llvm


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--nm",
        default="xcrun nm",
        help="nm command that lists Mach-O files and archives",
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--absent", action="store_true")
    mode.add_argument("--present", action="store_true")
    parser.add_argument("paths", nargs="+", type=Path)
    arguments = parser.parse_args()

    nm = arguments.nm.split()
    checked = 0
    carriers = 0
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
            symbols = screens(path, nm)
            if isinstance(symbols, str):
                failures.append(symbols)
                continue
            defined, llvm = symbols
            marked = PROVIDER_MARKER in path.read_bytes()
            if defined:
                carriers += 1
            if arguments.absent:
                if defined:
                    failures.append(
                        f"{path}: defines the software renderer's "
                        f"{defined[0]}")
                if llvm:
                    failures.append(f"{path}: defines the LLVM symbol {llvm[0]}")
                if marked:
                    failures.append(
                        f"{path}: holds the software provider's marker")
    if arguments.present and carriers == 0 and not failures:
        failures.append(
            "no binary defines a software Gallium screen: "
            + ", ".join(str(root) for root in arguments.paths))
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    if arguments.absent:
        print(f"no software renderer in {checked} binaries")
    else:
        print(f"the software renderer is in {carriers} of {checked} binaries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
