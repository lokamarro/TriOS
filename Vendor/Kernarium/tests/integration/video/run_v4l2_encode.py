#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Encode through every advertised V4L2 codec in a disposable Linux guest."""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/integration/support"))
import fixture_transfer  # noqa: E402

# The encoder and the decoder register one node each. The probe finds them by
# name and decodes every stream it encodes through the decoder.
NODES = (
    "for node in 1 2 3 4 5 6 7 8 9 10; do"
    " test -c /dev/video0 && test -c /dev/video1 && break; sleep 1; done;"
    " test -c /dev/video0 && test -c /dev/video1"
)


def main() -> int:
    arguments = fixture_transfer.parse_arguments(__doc__)
    fixture_transfer.run_probe(
        arguments,
        subject="V4L2 encoder regression",
        kernel_arguments=("--video", "--video-encoder", "ncpus=2", "panic=1"),
        precondition=NODES,
        probe_timeout_seconds=420,
        command_timeout_seconds=480,
        pass_marker="UML_V4L2_ENCODE: PASS",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
