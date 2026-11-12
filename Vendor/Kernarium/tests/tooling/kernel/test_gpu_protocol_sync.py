#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the Virtio-GPU memory-report synchronizer against fixture kernel trees.

The tool copies the wire header into a kernel tree and refuses a tree whose own
virtio_gpu.h already uses one of the header's feature, event, command or
response codes. The fixtures cover the forms that header uses (defines, an
enum with explicit and implicit values, shifts) and each way the tool must
refuse: a collision of every class, a stale copy, a missing upstream header and
a directory that is not a kernel tree.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

UPSTREAM = """\
#define VIRTIO_GPU_F_VIRGL               0
#define VIRTIO_GPU_F_EDID                1
#define VIRTIO_GPU_F_CONTEXT_INIT        4
enum virtio_gpu_ctrl_type {
	VIRTIO_GPU_UNDEFINED = 0,
	/* 2d commands */
	VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
	VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
	VIRTIO_GPU_CMD_RESOURCE_UNREF,
	/* cursor commands */
	VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
	/* success responses */
	VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
	VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
	/* error responses */
	VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
};
#define VIRTIO_GPU_EVENT_DISPLAY (1 << 0)
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=pathlib.Path)
    parser.add_argument("--header", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    failures = []

    def check(name, passed, detail=""):
        if not passed:
            failures.append(f"{name}: {detail}")

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        # The tool finds its header beside itself.
        tool_dir = root / "gpu"
        tool_dir.mkdir()
        tool = tool_dir / "sync-protocol.py"
        shutil.copy(arguments.tool, tool)
        header = tool_dir / "virtio_gpu_uml.h"
        shutil.copy(arguments.header, header)

        def tree(name, upstream=UPSTREAM):
            path = root / name
            (path / "include/uapi/linux").mkdir(parents=True)
            (path / "drivers/gpu/drm/virtio").mkdir(parents=True)
            (path / "Makefile").write_text("")
            if upstream is not None:
                (path / "include/uapi/linux/virtio_gpu.h").write_text(upstream)
            return path

        def run(path, *flags):
            return subprocess.run([sys.executable, "-B", str(tool), "--kernel-source", str(path), *flags],
                                  stdin=subprocess.DEVNULL, capture_output=True, text=True)

        good = tree("good")
        copied = good / "drivers/gpu/drm/virtio/virtgpu_uml.h"

        result = run(good)
        check("sync", result.returncode == 0, result.stderr)
        check("sync copies the header", copied.is_file() and copied.read_bytes() == header.read_bytes())

        result = run(good, "--verify")
        check("verify", result.returncode == 0, result.stderr)

        copied.write_bytes(copied.read_bytes() + b"/* local edit */\n")
        result = run(good, "--verify")
        check("stale copy", result.returncode != 0 and "stale" in result.stderr, result.stderr)
        result = run(good)
        check("sync repairs a stale copy", result.returncode == 0, result.stderr)

        # Each class of code the header owns, taken by the tree.
        for name, line, code in (
            ("feature bit", "#define VIRTIO_GPU_F_OTHER 23\n", "VIRTIO_GPU_F_UML_MEMORY_REPORT"),
            ("event bit", "#define VIRTIO_GPU_EVENT_OTHER (1 << 1)\n", "VIRTIO_GPU_EVENT_UML_MEMORY"),
            ("command", "#define VIRTIO_GPU_CMD_OTHER 0x0f00\n", "VIRTIO_GPU_CMD_UML_GET_MEMORY"),
            ("response", "#define VIRTIO_GPU_RESP_OK_OTHER 0x11f0\n", "VIRTIO_GPU_RESP_OK_UML_MEMORY"),
        ):
            result = run(tree(f"collides-{name.replace(' ', '-')}", UPSTREAM + line))
            check(f"{name} collision", result.returncode != 0 and code in result.stderr
                  and "collide" in result.stderr, result.stderr)
        # An enumerator that reaches the code by counting up collides too.
        counting = UPSTREAM.replace("VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200", "VIRTIO_GPU_RESP_OTHER = 0x11ef,\n"
                                    "\tVIRTIO_GPU_RESP_ERR_UNSPEC")
        result = run(tree("collides-counting", counting))
        check("enumerator that counts up to a code", result.returncode != 0
              and "VIRTIO_GPU_RESP_OK_UML_MEMORY" in result.stderr, result.stderr)
        # A code of another class with the same number is not a collision.
        result = run(tree("same-number-elsewhere", UPSTREAM + "#define VIRTIO_GPU_SHM_ID_OTHER 23\n"))
        check("a number in another class", result.returncode == 0, result.stderr)

        result = run(tree("no-upstream", upstream=None))
        check("missing upstream header", result.returncode != 0 and "virtio_gpu.h" in result.stderr, result.stderr)
        result = run(root / "not-a-tree")
        check("not a kernel tree", result.returncode != 0 and "prepared Linux source tree" in result.stderr,
              result.stderr)

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
