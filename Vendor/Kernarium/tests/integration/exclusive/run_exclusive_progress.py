#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run load/store-exclusive loops against colliding TLB sets in a guest.

Weak compare-and-swap loops of every exclusive width, ordering and the pair
forms make progress when the page they store to shares the target's TLB set,
and when a copy-on-write break lands between the pair. Both engines also run
the loops with a store between the pair.
"""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/integration/support"))
import fixture_transfer  # noqa: E402


def main() -> int:
    arguments = fixture_transfer.parse_arguments(__doc__, ("gadgets", "jit"))
    fixture_transfer.run_probe(
        arguments,
        subject="exclusive progress",
        kernel_arguments=("ncpus=2", "panic=1"),
        engine=arguments.engine,
        probe_arguments=("inside",),
        probe_timeout_seconds=60,
        command_timeout_seconds=100,
        pass_marker="UML_EXCLUSIVE_PROGRESS: PASS",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
