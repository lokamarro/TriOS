# SPDX-License-Identifier: GPL-2.0-or-later
"""What the x86-64 guest runners share: their options, their refusals and the
freestanding probes they cross-build for the guest.

A runner puts tests/integration/support and its own directory on sys.path
before importing this module; python3 -I leaves the latter off.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import tempfile
from collections.abc import Mapping, Sequence

from guest import GuestFile


GUEST_DIRECTORY = "/usr/local/libexec/uml-test"
BUILD_TIMEOUT_SECONDS = 120
# The target flags of an x86-64 and an i386 probe.
X86_64 = ("--target=x86_64-unknown-linux-gnu", "-mno-red-zone")
I386 = ("--target=i686-unknown-linux-gnu", "-march=i686")
COMPILER_HELP = "clang driver with x86-64 and i386 Linux targets and ld.lld"
COMMON_FLAGS = (
    "-std=c++20",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-ffreestanding",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-stack-protector",
    "-fno-asynchronous-unwind-tables",
    "-fno-pic",
    "-nostdlib",
    "-static",
    "-fuse-ld=lld",
    "-Wl,-e,_start",
    "-Wl,-z,max-page-size=4096",
    "-Wl,-s",
)


def build_probes(
    compiler: str,
    source: pathlib.Path,
    probes: Mapping[str, tuple[str, Sequence[str]]],
) -> dict[str, GuestFile]:
    """Builds `source` once per probe, a name mapped to its ABI (which the
    runner reports it under) and its target flags, and returns the
    executables as the payloads that install them in GUEST_DIRECTORY."""
    payloads = {}
    with tempfile.TemporaryDirectory(prefix=f"uml-{source.stem}-") as directory:
        for name, (_, target_flags) in probes.items():
            output = pathlib.Path(directory) / name
            subprocess.run(
                [compiler, *target_flags, *COMMON_FLAGS, str(source), "-o", str(output)],
                check=True,
                stdin=subprocess.DEVNULL,
                timeout=BUILD_TIMEOUT_SECONDS,
            )
            payloads[f"{GUEST_DIRECTORY}/{name}"] = GuestFile(output.read_bytes(), 0o755)
    return payloads


def add_arguments(
    parser: argparse.ArgumentParser,
    *,
    compiler_help: str | None = COMPILER_HELP,
    rootfs_help: str | None = None,
) -> None:
    """--emulator, --rootfs, --engine and, unless `compiler_help` is None,
    --compiler."""
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path, help=rootfs_help)
    parser.add_argument("--engine", choices=("gadgets", "jit"), default="gadgets")
    if compiler_help is not None:
        parser.add_argument("--compiler", default="clang++", help=compiler_help)


def check_files(parser: argparse.ArgumentParser, arguments: argparse.Namespace) -> None:
    """Resolves the emulator and the rootfs and refuses a missing one."""
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()
    if not arguments.emulator.is_file() or not os.access(arguments.emulator, os.X_OK):
        parser.error(f"emulator is not executable: {arguments.emulator}")
    if not arguments.rootfs.is_file():
        parser.error(f"rootfs is not a regular file: {arguments.rootfs}")


def parse_arguments(
    description: str,
    *,
    compiler_help: str | None = COMPILER_HELP,
    rootfs_help: str | None = None,
    timeout_help: str | None = None,
) -> argparse.Namespace:
    """The arguments of a runner whose only other option is --timeout, the
    boot timeout, with the emulator and the rootfs resolved and checked."""
    parser = argparse.ArgumentParser(description=description)
    add_arguments(parser, compiler_help=compiler_help, rootfs_help=rootfs_help)
    parser.add_argument("--timeout", type=int, default=180, help=timeout_help)
    arguments = parser.parse_args()
    check_files(parser, arguments)
    if arguments.timeout <= 0:
        parser.error("timeout must be positive")
    return arguments
