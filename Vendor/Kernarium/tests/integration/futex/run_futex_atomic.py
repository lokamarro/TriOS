#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise atomic futex updates and priority-inheritance locks in a disposable guest."""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/integration/support"))
import fixture_transfer  # noqa: E402


def main() -> int:
    arguments = fixture_transfer.parse_arguments(__doc__)
    fixture_transfer.run_probe(
        arguments,
        subject="futex atomic regression",
        kernel_arguments=("ncpus=4", "panic=1"),
        engine="gadgets",
        probe_timeout_seconds=95,
        command_timeout_seconds=135,
        pass_marker="UML_FUTEX_ATOMIC: PASS",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
