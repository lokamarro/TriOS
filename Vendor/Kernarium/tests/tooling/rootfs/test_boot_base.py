#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the boot base verifier, its descriptors and the generic /init.

A synthetic base that meets the contract must pass the verifier, and each
single breach of it must fail: a digest mismatch, trailing bytes after the
gzip member, a dynamic or wrongly aligned program, a program for the other
architecture, a missing applet link and a base /init that does not fail
loudly. The checked-in descriptors must parse, the checked-in per-run /init
must parse as POSIX sh without probing for programs, and the harness session
must parse as POSIX sh.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import gzip
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def elf(machine: int, *, interpreter: bool = False, alignment: int = 0x10000) -> bytes:
    header = bytearray(64)
    header[:4] = b"\x7fELF"
    header[4] = 2
    header[5] = 1
    header[6] = 1
    struct.pack_into("<HHI", header, 16, 2, machine, 1)
    struct.pack_into("<QQQ", header, 24, 0x400000, 64, 0)
    struct.pack_into("<HHHHHH", header, 52, 64, 56, 2 if interpreter else 1, 64, 0, 0)
    load = struct.pack("<IIQQQQQQ", 1, 5, 0, 0x400000, 0x400000, 256, 256, alignment)
    segments = load
    if interpreter:
        segments += struct.pack("<IIQQQQQQ", 3, 4, 0, 0, 0, 0, 0, 1)
    return bytes(header) + segments + b"\0" * 64


def newc(entries: list[tuple[str, int, bytes, tuple[int, int]]]) -> bytes:
    output = bytearray()
    for inode, (name, mode, contents, (major, minor)) in enumerate(
        [*entries, ("TRAILER!!!", 0, b"", (0, 0))], start=1
    ):
        encoded = name.encode() + b"\0"
        fields = (
            inode, mode, 0, 0, 1, 0, len(contents), 0, 0, major, minor,
            len(encoded), 0,
        )
        output += b"070701" + b"".join(b"%08X" % field for field in fields)
        output += encoded
        output += b"\0" * (-len(output) % 4)
        output += contents
        output += b"\0" * (-len(output) % 4)
    return bytes(output)


BASE_INIT = (
    b"#!/bin/sh\necho \"UML_BOOT_BASE_ERROR:no per-run /init followed the base "
    b"archive\"\nexit 1\n"
)


def base_entries(machine: int) -> list[tuple[str, int, bytes, tuple[int, int]]]:
    directory = 0o040755
    program = 0o100755
    link = 0o120777
    entries = [
        ("bin", directory, b"", (0, 0)),
        ("bin/busybox", program, elf(machine), (0, 0)),
    ]
    for applet in ("[", "cp", "echo", "mkdir", "mount", "printf", "sh",
                   "sleep", "sync", "umount"):
        entries.append((f"bin/{applet}", link, b"busybox", (0, 0)))
    entries += [
        ("dev", directory, b"", (0, 0)),
        ("dev/console", 0o020622, b"", (5, 1)),
        ("init", program, BASE_INIT, (0, 0)),
        ("newroot", directory, b"", (0, 0)),
        ("proc", directory, b"", (0, 0)),
        ("sbin", directory, b"", (0, 0)),
        ("sbin/blkid", link, b"../bin/busybox", (0, 0)),
        ("sbin/switch_root", link, b"../bin/busybox", (0, 0)),
        ("sbin/mkfs.ext4", link, b"mke2fs", (0, 0)),
        ("sys", directory, b"", (0, 0)),
    ]
    for tool in ("dumpe2fs", "e2fsck", "mke2fs", "resize2fs"):
        entries.append((f"sbin/{tool}", program, elf(machine), (0, 0)))
    return entries


def write_fixture(directory: Path, archive: bytes, architecture: str) -> tuple[Path, Path]:
    path = directory / f"uml-boot-base-{architecture}.cpio.gz"
    path.write_bytes(archive)
    descriptor = directory / f"accepted-{architecture}.env"
    descriptor.write_text(
        "# SPDX-License-Identifier: GPL-2.0-or-later\n"
        "UML_BOOT_BASE_SCHEMA=1\n"
        "UML_BOOT_BASE_RELEASE='fixture'\n"
        f"UML_BOOT_BASE_FILE='uml-boot-base-{architecture}.cpio.gz'\n"
        f"UML_BOOT_BASE_ARCHITECTURE='{architecture}'\n"
        f"UML_BOOT_BASE_SIZE={len(archive)}\n"
        f"UML_BOOT_BASE_SHA256='{hashlib.sha256(archive).hexdigest()}'\n"
    )
    return descriptor, path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--boot-ramdisk", required=True, type=Path)
    arguments = parser.parse_args()
    boot_ramdisk = arguments.boot_ramdisk.resolve()
    sys.path.insert(0, str(boot_ramdisk))
    import verify_base  # pylint: disable=import-outside-toplevel

    for architecture in verify_base.ELF_MACHINES:
        values = verify_base.read_descriptor(
            boot_ramdisk / f"accepted-{architecture}.env"
        )
        require(values["ARCHITECTURE"] == architecture, "descriptor architecture")

    init = boot_ramdisk / "init"
    require(init.stat().st_mode & 0o111 == 0o111, "the per-run /init is not executable")
    shell = shutil.which("sh")
    require(shell is not None, "no POSIX sh to parse the per-run /init")
    subprocess.run([shell, "-n", str(init)], check=True, timeout=30)
    code = [
        line.split("#", 1)[0]
        for line in init.read_text().splitlines()
        if not line.lstrip().startswith("#")
    ]
    require(
        not any("command -v" in line or "which " in line for line in code),
        "the per-run /init probes for programs the base does not provide",
    )
    session = boot_ramdisk / "session"
    require(
        session.stat().st_mode & 0o111 == 0o111,
        "the harness session is not executable",
    )
    subprocess.run([shell, "-n", str(session)], check=True, timeout=30)

    machine = verify_base.ELF_MACHINES["aarch64"]
    other = verify_base.ELF_MACHINES["x86_64"]

    def replaced(name: str, contents: bytes) -> list:
        return [
            (entry[0], entry[1], contents, entry[3]) if entry[0] == name else entry
            for entry in base_entries(machine)
        ]

    cases: list[tuple[str, Callable[[], bytes], str | None]] = [
        ("accepted", lambda: gzip.compress(newc(base_entries(machine))), None),
        (
            "trailing bytes",
            lambda: gzip.compress(newc(base_entries(machine))) + b"\0\0\0\0",
            "bytes follow the gzip member",
        ),
        (
            "dynamic program",
            lambda: gzip.compress(newc(replaced("sbin/e2fsck", elf(machine, interpreter=True)))),
            "names a program interpreter",
        ),
        (
            "4 KiB alignment",
            lambda: gzip.compress(newc(replaced("bin/busybox", elf(machine, alignment=0x1000)))),
            "aligned to 0x1000",
        ),
        (
            "other architecture",
            lambda: gzip.compress(newc(replaced("sbin/mke2fs", elf(other)))),
            "ELF machine 62",
        ),
        (
            "missing switch_root",
            lambda: gzip.compress(newc([
                entry for entry in base_entries(machine)
                if entry[0] != "sbin/switch_root"
            ])),
            "sbin/switch_root is missing",
        ),
        (
            "silent init",
            lambda: gzip.compress(newc(replaced("init", b"#!/bin/sh\nexec /bin/sh\n"))),
            "does not fail loudly",
        ),
    ]
    with tempfile.TemporaryDirectory(prefix="uml-boot-base-test-") as name:
        for case, build, expected in cases:
            directory = Path(name) / case.replace(" ", "-")
            directory.mkdir()
            descriptor, archive = write_fixture(directory, build(), "aarch64")
            try:
                verify_base.verify(descriptor, archive)
            except verify_base.VerificationError as error:
                require(expected is not None, f"{case}: rejected: {error}")
                require(expected in str(error), f"{case}: wrong reason: {error}")
            else:
                require(expected is None, f"{case}: accepted")

        descriptor, archive = write_fixture(
            Path(name), gzip.compress(newc(base_entries(machine))), "aarch64"
        )
        archive.write_bytes(archive.read_bytes()[:-1] + b"\xff")
        try:
            verify_base.verify(descriptor, archive)
        except verify_base.VerificationError as error:
            require("SHA-256" in str(error), f"digest: wrong reason: {error}")
        else:
            raise RuntimeError("an archive that differs from its descriptor passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
