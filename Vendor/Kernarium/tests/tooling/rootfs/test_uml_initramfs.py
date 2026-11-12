#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the uml-initramfs host tool's archives and refusals.

The tool writes the per-run segment, or the whole initrd when given a base.
Its segment must be a newc archive whose /init is executable, whose parents
precede their children and whose files are single links. With a base, the
output is the base zero-padded to four bytes and then that segment. The tool
must never replace an existing output and must leave nothing behind when an
argument is invalid.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import gzip
import subprocess
import sys
import tempfile
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--boot-ramdisk", required=True, type=Path)
    arguments = parser.parse_args()
    tool = arguments.tool.resolve()
    sys.path.insert(0, str(arguments.boot_ramdisk.resolve()))
    import verify_base  # pylint: disable=import-outside-toplevel

    def run(*options: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(tool), *options],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )

    with tempfile.TemporaryDirectory(prefix="uml-initramfs-tool-test-") as name:
        work = Path(name)
        init = work / "init"
        init.write_bytes(b"#!/bin/sh\nexit 0\n")
        init.chmod(0o600)
        token = work / "token"
        token.write_bytes(b"token\n")
        token.chmod(0o640)

        segment = work / "segment.cpio"
        result = run(
            f"--output={segment}",
            f"--init={init}",
            f"--file=/payload/a/b/token={token}",
            f"--file=/boot.env={token}",
        )
        require(result.returncode == 0, f"segment: {result.stderr}")
        data = segment.read_bytes()
        require(len(data) % 4 == 0, "the segment is not padded to four bytes")
        require(segment.stat().st_mode & 0o777 == 0o600, "the output is not private")
        entries = verify_base.parse_newc(data)
        names = [entry.name for entry in entries]
        require(
            names == ["init", "payload", "payload/a", "payload/a/b",
                      "payload/a/b/token", "boot.env"],
            f"unexpected entries {names}",
        )
        by_name = {entry.name: entry for entry in entries}
        require(by_name["init"].mode == 0o100755, "/init is not mode 0755")
        require(by_name["payload/a"].mode == 0o040755, "a parent is not a directory")
        require(by_name["payload/a/b/token"].mode == 0o100640, "the file mode changed")
        require(by_name["payload/a/b/token"].contents == b"token\n", "wrong contents")
        require(
            all(entry.links == 1 for entry in entries if entry.mode & 0o170000 == 0o100000),
            "a file is not a single link",
        )

        base = work / "base.cpio.gz"
        base.write_bytes(gzip.compress(b"base archive", mtime=0) + b"\0")
        require(len(base.read_bytes()) % 4 != 0, "the fixture base is aligned")
        initrd = work / "initrd.cpio"
        result = run(f"--output={initrd}", f"--base={base}", f"--init={init}")
        require(result.returncode == 0, f"initrd: {result.stderr}")
        base_bytes = base.read_bytes()
        composed = initrd.read_bytes()
        padded = len(base_bytes) + (-len(base_bytes) % 4)
        require(composed[: len(base_bytes)] == base_bytes, "the base changed")
        require(
            composed[len(base_bytes) : padded] == b"\0" * (padded - len(base_bytes)),
            "the base is not zero-padded",
        )
        require(
            [entry.name for entry in verify_base.parse_newc(composed[padded:])] == ["init"],
            "the segment does not follow the padded base",
        )

        before = segment.read_bytes()
        result = run(f"--output={segment}", f"--init={init}")
        require(result.returncode != 0, "an existing output was replaced")
        require(segment.read_bytes() == before, "an existing output changed")

        refused = work / "refused.cpio"
        invalid = [
            (f"--file=payload/relative={token}",),
            (f"--file=/payload/../escape={token}",),
            (f"--file=/payload/x={token}", f"--file=/payload/x={token}"),
            (f"--file=/payload/x={token}", f"--file=/payload/x/y={token}"),
            ("--file=/payload/x",),
            (f"--file=/payload/x={work / 'missing'}",),
            (f"--file=/payload/x={work}",),
            (f"--base={token}",),
            ("--unknown",),
        ]
        for options in invalid:
            result = run(f"--output={refused}", *options)
            require(result.returncode != 0, f"accepted {options}")
            require(not refused.exists(), f"{options} left an output behind")

        result = run("--help")
        require(result.returncode == 0 and "--output" in result.stderr, "no usage")
        result = run(f"--init={init}")
        require(result.returncode != 0, "ran without an output")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
