# SPDX-License-Identifier: GPL-2.0-or-later
"""The steps the opt-in architecture-table freshness tests share.

Each test skips (exit 77) unless CMake gave it the pinned Arm XML archive,
and a test that regenerates a table also unless the pinned LLVM MC is
installed. It then runs a generator from this directory and compares bytes.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from types import ModuleType
from typing import Callable

SKIP = 77
TOOLING = Path(__file__).resolve().parent


class Skip(Exception):
    """An input the test needs is not available."""


def archive_or_skip(variable: str) -> Path:
    """The --architecture-archive file; `variable` is the CMake variable
    that carries it."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture-archive", default="")
    option = parser.parse_args().architecture_archive
    if not option:
        raise Skip(f"{variable} is not set")
    archive = Path(option)
    if not archive.is_file():
        raise Skip(f"{archive} does not exist")
    return archive


def pinned_llvm_mc_or_skip(oracle: ModuleType) -> Path:
    """The LLVM MC `oracle` finds, if it is the version it pins."""
    try:
        tool = oracle.find_llvm_mc().resolve(strict=True)
        version = oracle.llvm_version(tool)
    except (OSError, ValueError, subprocess.SubprocessError):
        version = None
    if version != oracle.PINNED_LLVM_VERSION:
        raise Skip(f"LLVM MC {oracle.PINNED_LLVM_VERSION} is not installed")
    return tool


def regenerate(generator: str, archive: Path, arguments: list[str],
               timeout: int) -> None:
    """Run a generator of this directory on the archive."""
    subprocess.run(
        [
            sys.executable,
            "-B",
            str(TOOLING / generator),
            "--architecture-archive",
            str(archive.resolve(strict=True)),
            *arguments,
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        timeout=timeout,
    )


def run(main: Callable[[], int]) -> None:
    """Exit with main's status, 77 for a skip and 1 for an I/O error."""
    try:
        sys.exit(main())
    except Skip as skip:
        print(f"skipped: {skip}")
        sys.exit(SKIP)
    except (OSError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
