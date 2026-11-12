#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Regenerate both A64 oracle tables and require the checked-in bytes.

The pinned Arm XML archive is not a managed dependency, so this check is
opt-in: CMake passes UML_A64_ARCHITECTURE_ARCHIVE, and the test skips when
that is empty or missing, or when the pinned LLVM MC is not installed. Both
generators refuse an archive whose name or SHA-256 differs from the pin in
a64_architecture_oracle.py before they read it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import sys
import tempfile
from pathlib import Path

import a64_llvm_oracle
from freshness import archive_or_skip, pinned_llvm_mc_or_skip, regenerate, run


TABLES = Path(__file__).resolve().parents[3] / "tests/unit/arm64"
GENERATORS = {
    "a64_instruction_allocations.tsv": "generate_a64_instruction_allocations.py",
    "a64_instruction_decode_partitions.tsv": (
        "generate_a64_instruction_decode_partitions.py"
    ),
}
GENERATOR_TIMEOUT_SECONDS = 600


def main() -> int:
    archive = archive_or_skip("UML_A64_ARCHITECTURE_ARCHIVE")
    tool = pinned_llvm_mc_or_skip(a64_llvm_oracle)

    stale: list[str] = []
    with tempfile.TemporaryDirectory() as scratch:
        output = Path(scratch).resolve(strict=True)
        for table, generator in GENERATORS.items():
            arguments = ["--output", str(output / table)]
            if table == "a64_instruction_allocations.tsv":
                arguments += ["--llvm-mc", str(tool)]
            regenerate(generator, archive, arguments,
                       GENERATOR_TIMEOUT_SECONDS)
            if (output / table).read_bytes() != (TABLES / table).read_bytes():
                stale.append(table)
    if stale:
        print(
            "A64 oracle tables differ from a regeneration: "
            + ", ".join(stale)
            + "; rerun their generators",
            file=sys.stderr,
        )
        return 1
    print("A64 oracle tables match a regeneration from the pinned release")
    return 0


if __name__ == "__main__":
    run(main)
