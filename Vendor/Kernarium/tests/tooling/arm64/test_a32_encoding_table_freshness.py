#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate the AArch32 encoding table and require the checked-in bytes.

Like arm64-instruction-oracle-freshness, this check is opt-in: CMake passes
UML_A32_ARCHITECTURE_ARCHIVE, and the test skips when that is empty or
missing, or when the pinned LLVM MC is not installed. The generator refuses
an archive whose name or SHA-256 differs from the pin before it reads it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import sys
import tempfile
from pathlib import Path

import a32_llvm_oracle
from freshness import archive_or_skip, pinned_llvm_mc_or_skip, regenerate, run
from generate_a32_encoding_table import OUTPUTS


REPOSITORY = Path(__file__).resolve().parents[3]
GENERATOR_TIMEOUT_SECONDS = 1500


def main() -> int:
    archive = archive_or_skip("UML_A32_ARCHITECTURE_ARCHIVE")
    tool = pinned_llvm_mc_or_skip(a32_llvm_oracle)

    with tempfile.TemporaryDirectory() as scratch:
        root = Path(scratch).resolve(strict=True)
        for name in OUTPUTS:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
        regenerate(
            "generate_a32_encoding_table.py",
            archive,
            ["--llvm-mc", str(tool), "--output-root", str(root)],
            GENERATOR_TIMEOUT_SECONDS,
        )
        stale = [
            name
            for name in OUTPUTS
            if (root / name).read_bytes() != (REPOSITORY / name).read_bytes()
        ]
    if stale:
        print(
            f"{', '.join(stale)} differ from a regeneration; rerun "
            "generate_a32_encoding_table.py",
            file=sys.stderr,
        )
        return 1
    print("AArch32 encoding table matches a regeneration from the pinned release")
    return 0


if __name__ == "__main__":
    run(main)
