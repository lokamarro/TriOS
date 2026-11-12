#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the ELF geometry of a 4 KiB Android kernel image.

This is the kernel artifact verifier's geometry check. Its bridge imports are
checked by the kernel build, and a release keeps the bridge identity it was
built with, so this check leaves them alone.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "kernel/common/artifacts"))
from kernel_artifact import verify_elf_geometry  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("kernel", type=Path)
    args = parser.parse_args()
    verify_elf_geometry(
        args.kernel.read_bytes(),
        lambda message: SystemExit(f"Android kernel ELF verifier: {message}"),
    )
    print("verified 4 KiB AArch64 ET_DYN Android kernel ELF")


if __name__ == "__main__":
    main()
