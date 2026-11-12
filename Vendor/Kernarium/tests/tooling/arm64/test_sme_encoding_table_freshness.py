#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate the SVE/SME encoding table and require the checked-in bytes.

Like arm64-instruction-oracle-freshness, this check is opt-in: CMake passes
UML_A64_ARCHITECTURE_ARCHIVE, and the test skips when that is empty or
missing, or when the pinned LLVM MC is not installed. The generator refuses
an archive whose name or SHA-256 differs from the pin before it reads it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import sys
import tempfile
from pathlib import Path

import a64_llvm_oracle
from freshness import archive_or_skip, pinned_llvm_mc_or_skip, regenerate, run


TABLE = Path(__file__).resolve().parents[3] / "darwin/cpu/arm64/detail/sme_encodings.hpp"
GENERATOR_TIMEOUT_SECONDS = 600


def main() -> int:
    archive = archive_or_skip("UML_A64_ARCHITECTURE_ARCHIVE")
    tool = pinned_llvm_mc_or_skip(a64_llvm_oracle)

    with tempfile.TemporaryDirectory() as scratch:
        output = Path(scratch).resolve(strict=True) / TABLE.name
        regenerate(
            "generate_sme_encoding_table.py",
            archive,
            ["--llvm-mc", str(tool), "--output", str(output)],
            GENERATOR_TIMEOUT_SECONDS,
        )
        if output.read_bytes() != TABLE.read_bytes():
            print(
                f"{TABLE.name} differs from a regeneration; rerun "
                "generate_sme_encoding_table.py",
                file=sys.stderr,
            )
            return 1
    print("SVE/SME encoding table matches a regeneration from the pinned release")
    return 0


if __name__ == "__main__":
    run(main)
