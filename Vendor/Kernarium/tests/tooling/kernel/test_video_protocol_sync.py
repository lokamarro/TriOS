#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the Virtio video protocol synchronizer against fixture kernel trees.

The tool copies the wire header into a kernel tree and checks every V4L2 value
of its table against the tree's UAPI headers. The fixtures cover the forms the
UAPI headers use (a macro built from other macros, enumerators with explicit
and implicit values, comments, preprocessor lines inside an enum) and each way
the tool must refuse: a wrong value, a symbol the kernel lacks, a table line
the checker cannot read, a stale header copy and a missing UAPI header.
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

TABLE = """\
#define UML_V4L2_CID_FIXTURE_BITRATE 0x009909cfu
#define UML_V4L2_CID_FIXTURE_JPEG 0x009d0903u
#define UML_V4L2_FIXTURE_MODE_FIRST 0u
#define UML_V4L2_FIXTURE_MODE_SECOND 1u
#define UML_V4L2_FIXTURE_MODE_THIRD 2u
#define UML_V4L2_FIXTURE_TYPE_WIDE 0x0110u
#define UML_V4L2_FIXTURE_SHIFTED 20u
"""

CONTROLS = """\
#define V4L2_CTRL_CLASS_CODEC	0x00990000	/* Stateful codec controls */
#define V4L2_CTRL_CLASS_JPEG	0x009d0000
#define V4L2_CID_CODEC_BASE	(V4L2_CTRL_CLASS_CODEC | 0x900)
#define V4L2_CID_JPEG_CLASS_BASE	(V4L2_CTRL_CLASS_JPEG | 0x900)
#define V4L2_CID_FIXTURE_BITRATE	(V4L2_CID_CODEC_BASE+207)
#define V4L2_CID_FIXTURE_JPEG	(V4L2_CID_JPEG_CLASS_BASE + 3)
#define V4L2_FIXTURE_FUNCTION(x)	((x) + 1)
enum v4l2_fixture_mode {
	V4L2_FIXTURE_MODE_FIRST = 0,
	V4L2_FIXTURE_MODE_SECOND,	/* implicit: one after the previous */
#ifdef __KERNEL__
	V4L2_FIXTURE_MODE_THIRD,
#endif
};
// Shifts and unsigned suffixes appear in real headers.
#define V4L2_FIXTURE_SHIFTED (1U << 4 | 4)
"""

VIDEODEV = """\
enum v4l2_ctrl_type {
	V4L2_CTRL_TYPE_INTEGER	     = 1,
	/* Compound types are >= 0x0100 */
	V4L2_FIXTURE_TYPE_WIDE	     = 0x0110,
};
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
        # The tool finds its header and table beside itself.
        tool_dir = root / "video"
        tool_dir.mkdir()
        tool = tool_dir / "sync-protocol.py"
        shutil.copy(arguments.tool, tool)
        shutil.copy(arguments.header, tool_dir / "virtio_video.h")
        table = tool_dir / "virtio_video_v4l2_ids.h"
        table.write_text(TABLE)

        def tree(name, controls=CONTROLS, videodev=VIDEODEV):
            path = root / name
            (path / "include/uapi/linux").mkdir(parents=True)
            (path / "Makefile").write_text("")
            if controls is not None:
                (path / "include/uapi/linux/v4l2-controls.h").write_text(controls)
            if videodev is not None:
                (path / "include/uapi/linux/videodev2.h").write_text(videodev)
            return path

        def run(path, *flags):
            return subprocess.run([sys.executable, "-B", str(tool), "--kernel-source", str(path), *flags],
                                  stdin=subprocess.DEVNULL, capture_output=True, text=True)

        good = tree("good")
        copied = good / "include/uapi/linux/virtio_video.h"

        result = run(good)
        check("sync", result.returncode == 0, result.stderr)
        check("sync copies the header", copied.is_file() and copied.read_bytes() == (tool_dir / "virtio_video.h").read_bytes())

        result = run(good, "--verify")
        check("verify", result.returncode == 0, result.stderr)

        copied.write_bytes(copied.read_bytes() + b"/* local edit */\n")
        result = run(good, "--verify")
        check("stale copy", result.returncode != 0 and "stale" in result.stderr, result.stderr)
        result = run(good)
        check("sync repairs a stale copy", result.returncode == 0, result.stderr)

        # A value that differs from the kernel is named with both numbers.
        table.write_text(TABLE.replace("0x009909cfu", "0x009909ceu"))
        result = run(good)
        check("wrong value", result.returncode != 0 and "V4L2_CID_FIXTURE_BITRATE" in result.stderr
              and "0x9909ce" in result.stderr and "0x9909cf" in result.stderr, result.stderr)
        check("wrong value is the only complaint", result.stderr.count("V4L2_") == 1, result.stderr)
        result = run(good, "--verify")
        check("verify checks values too", result.returncode != 0 and "V4L2_CID_FIXTURE_BITRATE" in result.stderr,
              result.stderr)

        table.write_text(TABLE + "#define UML_V4L2_FIXTURE_ABSENT 3u\n")
        result = run(good)
        check("symbol the kernel lacks", result.returncode != 0 and "V4L2_FIXTURE_ABSENT" in result.stderr
              and "not defined" in result.stderr, result.stderr)

        table.write_text(TABLE + "#define UML_V4L2_FIXTURE_MODE_FIRST (0)\n")
        result = run(good)
        check("unreadable table line", result.returncode != 0 and "unreadable" in result.stderr, result.stderr)

        table.write_text("/* nothing */\n")
        result = run(good)
        check("empty table", result.returncode != 0 and "lists no V4L2 value" in result.stderr, result.stderr)

        table.write_text(TABLE)
        result = run(tree("no-videodev", videodev=None))
        check("missing UAPI header", result.returncode != 0 and "videodev2.h" in result.stderr, result.stderr)
        result = run(tree("self-reference", controls="#define V4L2_CID_FIXTURE_BITRATE V4L2_CID_FIXTURE_BITRATE\n"))
        check("self-referential macro", result.returncode != 0 and "terms of itself" in result.stderr, result.stderr)

        result = run(root / "not-a-tree")
        check("not a kernel tree", result.returncode != 0 and "prepared Linux source tree" in result.stderr, result.stderr)

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
