#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Allocate V4L2 MMAP capture buffers on an idle guest that has a GPU."""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/integration/support"))
import fixture_transfer  # noqa: E402

# The allocation takes milliseconds once the host has answered. Seconds is a
# stall: on an idle guest nothing but this call sends the command that makes
# the host answer.
ALLOCATION_BOUND_SECONDS = 10
# The framebuffer console redraws its cursor through the GPU every half second,
# which sends whatever is waiting in the control queue and hides a command that
# was queued and never sent. The test runs with the console detached, so that
# the allocation is the only thing that can send its own commands.
QUIET_GPU = (
    "echo 0 >/sys/class/graphics/fbcon/cursor_blink 2>/dev/null || true; "
    "for v in /sys/class/vtconsole/vtcon*; do "
    "grep -q 'frame buffer' $v/name 2>/dev/null && echo 0 >$v/bind; done || true; "
    "sleep 2"
)


def main() -> int:
    arguments = fixture_transfer.parse_arguments(__doc__)
    fixture_transfer.run_probe(
        arguments,
        subject="V4L2 MMAP capture allocation",
        kernel_arguments=(
            "--gpu",
            "--gpu-display=vnc",
            "--gpu-vnc-bind=127.0.0.1",
            "--gpu-vnc-port=0",
            "--video",
            "ncpus=2",
            "panic=1",
        ),
        precondition=f"test -c /dev/video0; {QUIET_GPU}",
        probe_arguments=("--require-blob", "/dev/video0", str(ALLOCATION_BOUND_SECONDS)),
        probe_timeout_seconds=ALLOCATION_BOUND_SECONDS * 4,
        command_timeout_seconds=ALLOCATION_BOUND_SECONDS * 4 + 20,
        pass_marker="UML_V4L2_MMAP_CAPTURE: PASS",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
