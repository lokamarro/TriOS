#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a boot base archive against its acceptance descriptor.

The launcher embeds the base and boots every Linux guest through it, so a new
base is accepted only when this check passes: the descriptor's size and
SHA-256, one gzip member holding a newc archive, the fail-loud /init, the
programs the per-run /init calls, and static ELF programs for the
descriptor's architecture with 16 KiB segment alignment.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import dataclasses
import hashlib
import json
import re
import shlex
import struct
import sys
import zlib
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
NEWC_MAGIC = b"070701"
NEWC_HEADER_SIZE = 110
TRAILER = "TRAILER!!!"
# Unpacking happens in boot memory before the guest has reclaimed anything; a
# base far above today's size would squeeze every guest's payload.
MAXIMUM_UNPACKED_BYTES = 16 * 1024 * 1024
# The accepted bases are built with 16 KiB segment alignment, which the 4 KiB
# guest maps and which matches the Android userspace's page-agnostic alignment.
MINIMUM_SEGMENT_ALIGNMENT = 0x4000
ELF_MACHINES = {"aarch64": 183, "x86_64": 62}
DESCRIPTOR_KEYS = ("SCHEMA", "RELEASE", "FILE", "ARCHITECTURE", "SIZE", "SHA256")
BASE_ERROR_MARKER = b"UML_BOOT_BASE_ERROR:"

FILE_TYPE = 0o170000
REGULAR = 0o100000
DIRECTORY = 0o040000
SYMLINK = 0o120000
CHARACTER = 0o020000

REQUIRED_DIRECTORIES = ("bin", "dev", "newroot", "proc", "sbin", "sys")
REQUIRED_PROGRAMS = (
    "bin/busybox",
    "sbin/dumpe2fs",
    "sbin/e2fsck",
    "sbin/mke2fs",
    "sbin/resize2fs",
)
# Every applet the per-run /init reaches through PATH. BusyBox's own mke2fs
# has no -E, so mkfs.ext4 must stay e2fsprogs'.
REQUIRED_LINKS = {
    "bin/[": "busybox",
    "bin/cp": "busybox",
    "bin/echo": "busybox",
    "bin/mkdir": "busybox",
    "bin/mount": "busybox",
    "bin/printf": "busybox",
    "bin/sh": "busybox",
    "bin/sleep": "busybox",
    "bin/sync": "busybox",
    "bin/umount": "busybox",
    "sbin/blkid": "../bin/busybox",
    "sbin/mkfs.ext4": "mke2fs",
    "sbin/switch_root": "../bin/busybox",
}


class VerificationError(RuntimeError):
    """The archive or its descriptor breaks the boot base contract."""


@dataclasses.dataclass(frozen=True)
class Entry:
    name: str
    mode: int
    links: int
    major: int
    minor: int
    contents: bytes


def read_descriptor(path: Path) -> dict[str, str]:
    """Parse the KEY=VALUE shell assignments CMake's reader accepts."""
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise VerificationError(f"cannot read {path}: {error}") from error
    for line in lines:
        if not line or line.startswith("#"):
            continue
        match = re.fullmatch(r"UML_BOOT_BASE_([A-Z0-9_]+)=(.*)", line)
        if match is None:
            raise VerificationError(f"{path}: unexpected line {line!r}")
        words = shlex.split(match.group(2))
        if len(words) != 1:
            raise VerificationError(f"{path}: {line!r} is not one value")
        if match.group(1) in values:
            raise VerificationError(f"{path}: {match.group(1)} is repeated")
        values[match.group(1)] = words[0]
    missing = [key for key in DESCRIPTOR_KEYS if key not in values]
    if missing:
        raise VerificationError(f"{path}: missing {', '.join(missing)}")
    unknown = sorted(set(values) - set(DESCRIPTOR_KEYS))
    if unknown:
        raise VerificationError(f"{path}: unknown {', '.join(unknown)}")
    if values["SCHEMA"] != "1":
        raise VerificationError(f"{path}: unsupported schema {values['SCHEMA']}")
    if values["ARCHITECTURE"] not in ELF_MACHINES:
        raise VerificationError(
            f"{path}: unsupported architecture {values['ARCHITECTURE']}"
        )
    if values["FILE"] != f"uml-boot-base-{values['ARCHITECTURE']}.cpio.gz":
        raise VerificationError(f"{path}: FILE does not name its architecture")
    if not values["SIZE"].isdigit():
        raise VerificationError(f"{path}: SIZE is not a byte count")
    if re.fullmatch(r"[0-9a-f]{64}", values["SHA256"]) is None:
        raise VerificationError(f"{path}: SHA256 is not a lowercase digest")
    return values


def gunzip_single_member(data: bytes) -> bytes:
    decompressor = zlib.decompressobj(wbits=16 + zlib.MAX_WBITS)
    try:
        unpacked = decompressor.decompress(data, MAXIMUM_UNPACKED_BYTES + 1)
    except zlib.error as error:
        raise VerificationError(f"the archive is not gzip: {error}") from error
    if len(unpacked) > MAXIMUM_UNPACKED_BYTES or decompressor.unconsumed_tail:
        raise VerificationError(
            f"the archive unpacks to more than {MAXIMUM_UNPACKED_BYTES} bytes"
        )
    if not decompressor.eof:
        raise VerificationError("the gzip member is truncated")
    if decompressor.unused_data:
        raise VerificationError("bytes follow the gzip member")
    return unpacked


def align4(value: int) -> int:
    return (value + 3) & ~3


def parse_newc(data: bytes) -> list[Entry]:
    """Every entry before the trailer, which must end the archive."""
    entries: list[Entry] = []
    offset = 0
    while True:
        header = data[offset : offset + NEWC_HEADER_SIZE]
        if len(header) != NEWC_HEADER_SIZE or not header.startswith(NEWC_MAGIC):
            raise VerificationError(f"no newc header at offset {offset}")
        try:
            fields = [int(header[6 + 8 * i : 14 + 8 * i], 16) for i in range(13)]
        except ValueError as error:
            raise VerificationError(f"malformed newc header at {offset}") from error
        mode, links, size = fields[1], fields[4], fields[6]
        rdev_major, rdev_minor, name_size = fields[9], fields[10], fields[11]
        name_start = offset + NEWC_HEADER_SIZE
        name_end = name_start + name_size
        if name_size == 0 or data[name_end - 1 : name_end] != b"\0":
            raise VerificationError(f"unterminated newc name at {offset}")
        name = data[name_start : name_end - 1].decode("utf-8", "strict")
        contents_start = align4(name_end)
        contents = data[contents_start : contents_start + size]
        if len(contents) != size:
            raise VerificationError(f"{name}: truncated contents")
        offset = align4(contents_start + size)
        if name == TRAILER:
            break
        entries.append(Entry(name, mode, links, rdev_major, rdev_minor, contents))
    if data[offset:].strip(b"\0"):
        raise VerificationError("data follows the newc trailer")
    return entries


def check_elf(name: str, contents: bytes, machine: int) -> None:
    if len(contents) < 64 or contents[:4] != b"\x7fELF":
        raise VerificationError(f"{name} is not an ELF program")
    if contents[4] != 2 or contents[5] != 1:
        raise VerificationError(f"{name} is not a little-endian ELF64 program")
    (elf_type, elf_machine) = struct.unpack_from("<HH", contents, 16)
    if elf_machine != machine:
        raise VerificationError(f"{name} is built for ELF machine {elf_machine}")
    if elf_type not in (2, 3):
        raise VerificationError(f"{name} is not an executable")
    (program_offset,) = struct.unpack_from("<Q", contents, 32)
    (entry_size, entry_count) = struct.unpack_from("<HH", contents, 54)
    if entry_size < 56 or program_offset + entry_size * entry_count > len(contents):
        raise VerificationError(f"{name} has a malformed program header table")
    loads = 0
    for index in range(entry_count):
        start = program_offset + index * entry_size
        (segment_type,) = struct.unpack_from("<I", contents, start)
        (alignment,) = struct.unpack_from("<Q", contents, start + 48)
        if segment_type == 3:
            raise VerificationError(f"{name} names a program interpreter")
        if segment_type == 1:
            loads += 1
            if alignment < MINIMUM_SEGMENT_ALIGNMENT:
                raise VerificationError(
                    f"{name} has a load segment aligned to {alignment:#x}"
                )
    if loads == 0:
        raise VerificationError(f"{name} has no load segment")


def check_archive(entries: list[Entry], architecture: str) -> None:
    machine = ELF_MACHINES[architecture]
    by_name: dict[str, Entry] = {}
    for entry in entries:
        if entry.name in by_name:
            raise VerificationError(f"{entry.name} appears twice")
        by_name[entry.name] = entry

    def require_type(name: str, file_type: int) -> Entry:
        entry = by_name.get(name)
        if entry is None:
            raise VerificationError(f"{name} is missing")
        if entry.mode & FILE_TYPE != file_type:
            raise VerificationError(f"{name} has type {entry.mode & FILE_TYPE:o}")
        return entry

    init = require_type("init", REGULAR)
    if init.mode & 0o111 != 0o111:
        raise VerificationError("init is not executable")
    if not init.contents.startswith(b"#!/bin/sh\n"):
        raise VerificationError("init is not a /bin/sh script")
    if BASE_ERROR_MARKER not in init.contents:
        raise VerificationError(
            "init does not fail loudly when no per-run /init follows"
        )
    for name in REQUIRED_DIRECTORIES:
        require_type(name, DIRECTORY)
    console = require_type("dev/console", CHARACTER)
    if (console.major, console.minor) != (5, 1):
        raise VerificationError("dev/console is not character device 5:1")
    for name, target in REQUIRED_LINKS.items():
        link = require_type(name, SYMLINK)
        if link.contents != target.encode():
            raise VerificationError(f"{name} does not point to {target}")
    for name in REQUIRED_PROGRAMS:
        require_type(name, REGULAR)
    for entry in entries:
        if entry.mode & FILE_TYPE != REGULAR:
            continue
        if entry.links != 1:
            raise VerificationError(f"{entry.name} has {entry.links} links")
        if entry.contents.startswith(b"\x7fELF") or entry.name in REQUIRED_PROGRAMS:
            check_elf(entry.name, entry.contents, machine)


def verify(descriptor_path: Path, archive: Path) -> dict[str, object]:
    descriptor = read_descriptor(descriptor_path)
    try:
        data = archive.read_bytes()
    except OSError as error:
        raise VerificationError(f"cannot read {archive}: {error}") from error
    if len(data) != int(descriptor["SIZE"]):
        raise VerificationError(
            f"{archive} is {len(data)} bytes, expected {descriptor['SIZE']}"
        )
    if hashlib.sha256(data).hexdigest() != descriptor["SHA256"]:
        raise VerificationError(f"{archive} does not match the descriptor SHA-256")
    unpacked = gunzip_single_member(data)
    entries = parse_newc(unpacked)
    check_archive(entries, descriptor["ARCHITECTURE"])
    return {
        "archive": str(archive),
        "architecture": descriptor["ARCHITECTURE"],
        "entries": len(entries),
        "release": descriptor["RELEASE"],
        "sha256": descriptor["SHA256"],
        "size": len(data),
        "unpacked_size": len(unpacked),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--descriptor",
        type=Path,
        help="acceptance descriptor (default: accepted-<architecture>.env here)",
    )
    parser.add_argument("--architecture", choices=sorted(ELF_MACHINES))
    parser.add_argument(
        "--archive",
        type=Path,
        help="the archive (default: .build/rootfs/<release>/<file>)",
    )
    arguments = parser.parse_args()
    if (arguments.descriptor is None) == (arguments.architecture is None):
        parser.error("pass exactly one of --descriptor and --architecture")
    descriptor = arguments.descriptor or HERE / f"accepted-{arguments.architecture}.env"
    try:
        archive = arguments.archive
        if archive is None:
            values = read_descriptor(descriptor)
            archive = ROOT / ".build" / "rootfs" / values["RELEASE"] / values["FILE"]
        result = verify(descriptor.resolve(), archive.resolve())
    except VerificationError as error:
        print(f"boot base verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
