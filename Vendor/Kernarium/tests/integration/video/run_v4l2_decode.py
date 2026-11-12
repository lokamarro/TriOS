#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode every advertised V4L2 profile in a disposable Linux guest."""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/integration/support"))
import fixture_transfer  # noqa: E402

# With the GPU, the decoder's capture buffers are host-visible blobs rather than
# guest pages, which changes how the driver imports a dma-buf. The kernel names
# the GPU's host-visible memory when it has it, so a guest that booted without
# it fails here rather than passing the blob cases without running them.
GPU_ARGUMENTS = (
    "--gpu",
    "--gpu-display=vnc",
    "--gpu-vnc-bind=127.0.0.1",
    "--gpu-vnc-port=0",
)
GPU_PRECONDITION = "dmesg | grep -qF +host_visible"


def main() -> int:
    arguments = fixture_transfer.parse_arguments(__doc__, gpu=True)
    fixtures = {
        "video/" + fixture.name: fixture.read_bytes()
        for fixture in sorted((ROOT / "tests/support/video").iterdir())
        if fixture.suffix in (".bin", ".raw")
    }
    fixture_transfer.run_probe(
        arguments,
        subject="V4L2 decoder regression",
        fixtures=fixtures,
        kernel_arguments=(
            "--video",
            *(GPU_ARGUMENTS if arguments.gpu else ()),
            "ncpus=2",
            "panic=1",
        ),
        precondition="test -c /dev/video0"
        + (f" && {GPU_PRECONDITION}" if arguments.gpu else ""),
        probe_arguments=("video",),
        probe_timeout_seconds=300,
        command_timeout_seconds=360,
        pass_marker="UML_V4L2_DECODE: PASS",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
