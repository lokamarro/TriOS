#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Assemble and verify the Android 17 read-only system disk.

The system disk is one raw GPT image that ships with the app and is attached
read-only. It holds the boot ramdisk containers and the five read-only
filesystems, each partition exactly its image rounded up to the alignment, so
no byte of the disk is slack. `assemble` packs the images, expanding the
Android sparse images the signed target files carry into the raw bytes the
guest reads, and writes the release's partition-layout.json beside the disk:
the system disk's exact partitions with a SHA-256 each, and the data disk's
template, which the host turns into the writable disk of each guest instance.
`verify` checks a disk against that file. The host validates the same geometry
before it boots.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import binascii
import hashlib
import json
import os
import re
import struct
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO


GPT_SIGNATURE = b"EFI PART"
GPT_REVISION = 0x00010000
GPT_HEADER_SIZE = 92
GPT_ENTRY_SIZE = 128
PROTECTIVE_MBR_TYPE = 0xEE
SPARSE_MAGIC = 0xED26FF3A
SPARSE_HEADER = struct.Struct("<IHHHHIIII")
SPARSE_CHUNK_HEADER = struct.Struct("<HHII")
SPARSE_RAW = 0xCAC1
SPARSE_FILL = 0xCAC2
SPARSE_DONT_CARE = 0xCAC3
SPARSE_CRC32 = 0xCAC4
COPY_CHUNK = 1024 * 1024
SECTOR_SIZE = 512
POLICY_SCHEMA_VERSION = 1
LAYOUT_SCHEMA_VERSION = 2
SYSTEM_DISK_FILE = "system.img"
LAYOUT_FILE = "partition-layout.json"
NAME_PATTERN = re.compile(r"[a-z][a-z0-9_]{0,35}")
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
# The partitions the guest and the host address by name.
BOOT_PARTITION_NAMES = ("init_boot", "recovery")
DATA_PARTITION_NAMES = ("misc", "metadata", "userdata")
GIB = 1024**3
# An expanded image larger than this is a corrupt or hostile sparse header, not
# a partition; the largest read-only partition is under a gibibyte.
MAX_PARTITION_IMAGE_BYTES = 16 * GIB


class DiskError(ValueError):
    pass


@dataclass(frozen=True)
class Partition:
    name: str
    start_bytes: int
    size_bytes: int
    type_guid: uuid.UUID
    unique_guid: uuid.UUID


def require_int(owner: dict[str, object], key: str, *, minimum: int = 0) -> int:
    value = owner.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise DiskError(f"{key} must be an integer of at least {minimum}")
    return value


def require_string(owner: dict[str, object], key: str) -> str:
    value = owner.get(key)
    if not isinstance(value, str) or not value:
        raise DiskError(f"{key} must be a non-empty string")
    return value


def require_object(owner: dict[str, object], key: str) -> dict[str, object]:
    value = owner.get(key)
    if not isinstance(value, dict):
        raise DiskError(f"{key} must be an object")
    return value


def require_keys(owner: dict[str, object], keys: set[str], label: str) -> None:
    if set(owner) != keys:
        raise DiskError(
            f"{label} keys differ from the contract: "
            f"missing={sorted(keys - set(owner))} extra={sorted(set(owner) - keys)}"
        )


def parse_guid(owner: dict[str, object], key: str) -> uuid.UUID:
    text = require_string(owner, key)
    try:
        parsed = uuid.UUID(text)
    except ValueError as error:
        raise DiskError(f"{key} is not a canonical UUID") from error
    if str(parsed) != text:
        raise DiskError(f"{key} is not a canonical lower-case UUID")
    return parsed


def partition_name(owner: dict[str, object]) -> str:
    name = require_string(owner, "name")
    if NAME_PATTERN.fullmatch(name) is None:
        raise DiskError(f"unsafe GPT partition name: {name}")
    return name


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def read_json(path: Path, label: str) -> dict[str, object]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise DiskError(f"cannot read {label} {path}: {error}") from error
    if not isinstance(document, dict):
        raise DiskError(f"{label} root must be an object")
    return document


@dataclass(frozen=True)
class Geometry:
    """The GPT geometry of one disk: its sector count and its table's size."""

    disk_size_bytes: int
    partition_entry_count: int

    @property
    def sector_count(self) -> int:
        return self.disk_size_bytes // SECTOR_SIZE

    @property
    def table_bytes(self) -> int:
        return self.partition_entry_count * GPT_ENTRY_SIZE

    @property
    def table_sectors(self) -> int:
        return self.table_bytes // SECTOR_SIZE

    @property
    def first_usable_lba(self) -> int:
        return 2 + self.table_sectors

    @property
    def last_usable_lba(self) -> int:
        return self.sector_count - self.table_sectors - 2

    @property
    def backup_entries_lba(self) -> int:
        return self.sector_count - self.table_sectors - 1


def check_entry_count(entry_count: int) -> None:
    if entry_count < 1 or entry_count > 4096:
        raise DiskError("partition entry count is outside the supported range")
    if entry_count * GPT_ENTRY_SIZE % SECTOR_SIZE:
        raise DiskError("partition entry array must occupy whole sectors")


def partition_entries(partitions: tuple[Partition, ...], geometry: Geometry) -> bytes:
    table = bytearray(geometry.table_bytes)
    for index, partition in enumerate(partitions):
        name = partition.name.encode("utf-16-le")
        if len(name) > 72:
            raise DiskError(f"partition name is too long for GPT: {partition.name}")
        first_lba = partition.start_bytes // SECTOR_SIZE
        last_lba = (partition.start_bytes + partition.size_bytes) // SECTOR_SIZE - 1
        entry = struct.pack(
            "<16s16sQQQ72s",
            partition.type_guid.bytes_le,
            partition.unique_guid.bytes_le,
            first_lba,
            last_lba,
            0,
            name.ljust(72, b"\0"),
        )
        offset = index * GPT_ENTRY_SIZE
        table[offset : offset + GPT_ENTRY_SIZE] = entry
    return bytes(table)


def gpt_header(
    geometry: Geometry,
    disk_guid: uuid.UUID,
    *,
    current_lba: int,
    backup_lba: int,
    entries_lba: int,
    entries_crc: int,
) -> bytes:
    header = bytearray(SECTOR_SIZE)
    struct.pack_into(
        "<8sIIIIQQQQ16sQIII",
        header,
        0,
        GPT_SIGNATURE,
        GPT_REVISION,
        GPT_HEADER_SIZE,
        0,
        0,
        current_lba,
        backup_lba,
        geometry.first_usable_lba,
        geometry.last_usable_lba,
        disk_guid.bytes_le,
        entries_lba,
        geometry.partition_entry_count,
        GPT_ENTRY_SIZE,
        entries_crc,
    )
    crc = binascii.crc32(header[:GPT_HEADER_SIZE]) & 0xFFFFFFFF
    struct.pack_into("<I", header, 16, crc)
    return bytes(header)


def protective_mbr(geometry: Geometry) -> bytes:
    mbr = bytearray(SECTOR_SIZE)
    sectors = min(geometry.sector_count - 1, 0xFFFFFFFF)
    struct.pack_into(
        "<B3sB3sII",
        mbr,
        446,
        0,
        b"\0\x02\0",
        PROTECTIVE_MBR_TYPE,
        b"\xff\xff\xff",
        1,
        sectors,
    )
    mbr[510:512] = b"\x55\xaa"
    return bytes(mbr)


def read_exact(source: BinaryIO, offset: int, size: int) -> bytes:
    source.seek(offset)
    payload = source.read(size)
    if len(payload) != size:
        raise DiskError("disk image is truncated")
    return payload


def verify_header(
    geometry: Geometry,
    disk_guid: uuid.UUID,
    sector: bytes,
    *,
    current_lba: int,
    backup_lba: int,
    entries_lba: int,
    entries_crc: int,
) -> None:
    if sector[:8] != GPT_SIGNATURE:
        raise DiskError("GPT header signature is missing")
    revision, size, recorded_crc, reserved = struct.unpack_from("<IIII", sector, 8)
    if revision != GPT_REVISION or size != GPT_HEADER_SIZE or reserved != 0:
        raise DiskError("GPT header revision, size, or reserved field is invalid")
    header = bytearray(sector[:size])
    struct.pack_into("<I", header, 16, 0)
    if binascii.crc32(header) & 0xFFFFFFFF != recorded_crc:
        raise DiskError("GPT header CRC32 does not match")
    fields = struct.unpack_from("<QQQQ16sQIII", sector, 24)
    expected = (
        current_lba,
        backup_lba,
        geometry.first_usable_lba,
        geometry.last_usable_lba,
        disk_guid.bytes_le,
        entries_lba,
        geometry.partition_entry_count,
        GPT_ENTRY_SIZE,
        entries_crc,
    )
    if fields != expected:
        raise DiskError("GPT header does not match the checked layout")


def verify_gpt(
    disk_path: Path,
    geometry: Geometry,
    disk_guid: uuid.UUID,
    partitions: tuple[Partition, ...],
) -> None:
    """Check protective MBR, both headers and both entry arrays of one disk."""
    if disk_path.is_symlink() or not disk_path.is_file():
        raise DiskError(f"disk image must be a regular non-symlink file: {disk_path}")
    if disk_path.stat().st_size != geometry.disk_size_bytes:
        raise DiskError("disk image size does not match the checked layout")
    expected_table = partition_entries(partitions, geometry)
    table_crc = binascii.crc32(expected_table) & 0xFFFFFFFF
    with disk_path.open("rb") as disk:
        mbr = read_exact(disk, 0, SECTOR_SIZE)
        if mbr[510:512] != b"\x55\xaa" or mbr[450] != PROTECTIVE_MBR_TYPE:
            raise DiskError("protective MBR is invalid")
        primary = read_exact(disk, SECTOR_SIZE, SECTOR_SIZE)
        primary_table = read_exact(disk, 2 * SECTOR_SIZE, geometry.table_bytes)
        backup_table = read_exact(
            disk, geometry.backup_entries_lba * SECTOR_SIZE, geometry.table_bytes
        )
        backup = read_exact(disk, (geometry.sector_count - 1) * SECTOR_SIZE, SECTOR_SIZE)
    if primary_table != expected_table or backup_table != expected_table:
        raise DiskError("GPT entry array does not match the checked layout")
    verify_header(
        geometry,
        disk_guid,
        primary,
        current_lba=1,
        backup_lba=geometry.sector_count - 1,
        entries_lba=2,
        entries_crc=table_crc,
    )
    verify_header(
        geometry,
        disk_guid,
        backup,
        current_lba=geometry.sector_count - 1,
        backup_lba=1,
        entries_lba=geometry.backup_entries_lba,
        entries_crc=table_crc,
    )


def write_gpt(
    disk: BinaryIO, geometry: Geometry, disk_guid: uuid.UUID,
    partitions: tuple[Partition, ...],
) -> None:
    """Write the protective MBR, both headers and both entry arrays."""
    table = partition_entries(partitions, geometry)
    table_crc = binascii.crc32(table) & 0xFFFFFFFF
    disk.seek(0)
    disk.write(protective_mbr(geometry))
    disk.write(
        gpt_header(
            geometry, disk_guid, current_lba=1, backup_lba=geometry.sector_count - 1,
            entries_lba=2, entries_crc=table_crc,
        )
    )
    disk.write(table)
    disk.seek(geometry.backup_entries_lba * SECTOR_SIZE)
    disk.write(table)
    disk.write(
        gpt_header(
            geometry, disk_guid, current_lba=geometry.sector_count - 1, backup_lba=1,
            entries_lba=geometry.backup_entries_lba, entries_crc=table_crc,
        )
    )


# --- the policy: what the tree decides ---------------------------------------


@dataclass(frozen=True)
class Policy:
    document: dict[str, object]
    boot: tuple[dict[str, object], ...]
    filesystems: tuple[dict[str, object], ...]


def load_policy(path: Path) -> Policy:
    document = read_json(path, "disk policy")
    require_keys(
        document, {"schema_version", "sector_size", "system_disk", "data_disk"},
        "disk policy",
    )
    if require_int(document, "schema_version") != POLICY_SCHEMA_VERSION:
        raise DiskError("only disk policy schema version 1 is supported")
    if require_int(document, "sector_size") != SECTOR_SIZE:
        raise DiskError("GPT sector size must be exactly 512 bytes")
    system = require_object(document, "system_disk")
    require_keys(
        system,
        {
            "alignment_bytes", "partition_entry_count", "disk_guid",
            "boot_partitions", "filesystem_partitions",
        },
        "system disk policy",
    )
    alignment = require_int(system, "alignment_bytes", minimum=SECTOR_SIZE)
    if alignment % 4096:
        raise DiskError("system disk alignment must be a multiple of 4 KiB")
    check_entry_count(require_int(system, "partition_entry_count", minimum=1))
    parse_guid(system, "disk_guid")
    groups: list[tuple[dict[str, object], ...]] = []
    seen_names: set[str] = set()
    seen_images: set[str] = set()
    seen_guids: set[uuid.UUID] = set()
    for key in ("boot_partitions", "filesystem_partitions"):
        entries = system[key]
        if not isinstance(entries, list) or not entries:
            raise DiskError(f"{key} must be a non-empty array")
        for index, item in enumerate(entries):
            if not isinstance(item, dict):
                raise DiskError(f"{key} entry {index} must be an object")
            require_keys(
                item, {"name", "image", "type_guid", "unique_guid"},
                f"{key} entry {index}",
            )
            name = partition_name(item)
            image = require_string(item, "image")
            if Path(image).name != image or "/" in image or "\\" in image:
                raise DiskError(f"partition {name} image must be a plain filename")
            parse_guid(item, "type_guid")
            unique = parse_guid(item, "unique_guid")
            if name in seen_names or image in seen_images or unique in seen_guids:
                raise DiskError(f"partition {name} duplicates a name, image or GUID")
            seen_names.add(name)
            seen_images.add(image)
            seen_guids.add(unique)
        groups.append(tuple(entries))
    if tuple(entry["name"] for entry in groups[0]) != BOOT_PARTITION_NAMES:
        raise DiskError("boot partitions must be exactly init_boot and recovery")
    validate_data_disk(require_object(document, "data_disk"), seen_names)
    return Policy(document, groups[0], groups[1])


def validate_data_disk(data: dict[str, object], system_names: set[str]) -> None:
    """The data disk template: a fixed head and a userdata that fills the rest."""
    require_keys(
        data,
        {
            "layout_version", "alignment_bytes", "partition_entry_count",
            "disk_guid", "trailing_free_bytes", "capacity_gib", "partitions",
        },
        "data disk",
    )
    require_int(data, "layout_version", minimum=1)
    alignment = require_int(data, "alignment_bytes", minimum=4096)
    if alignment % 4096:
        raise DiskError("data disk alignment must be a multiple of 4 KiB")
    check_entry_count(require_int(data, "partition_entry_count", minimum=1))
    parse_guid(data, "disk_guid")
    trailing = require_int(data, "trailing_free_bytes", minimum=1)
    if trailing % alignment:
        raise DiskError("data disk trailing space must be aligned")
    capacity = require_object(data, "capacity_gib")
    require_keys(capacity, {"minimum", "default", "maximum"}, "data disk capacity")
    minimum = require_int(capacity, "minimum", minimum=1)
    default = require_int(capacity, "default", minimum=minimum)
    require_int(capacity, "maximum", minimum=default)
    partitions = data["partitions"]
    if not isinstance(partitions, list) or len(partitions) != len(DATA_PARTITION_NAMES):
        raise DiskError("data disk partitions must be misc, metadata and userdata")
    previous_end = 0
    seen_guids: set[uuid.UUID] = set()
    for index, (item, expected) in enumerate(zip(partitions, DATA_PARTITION_NAMES)):
        if not isinstance(item, dict):
            raise DiskError(f"data partition {index} must be an object")
        require_keys(
            item, {"name", "start_bytes", "size_bytes", "type_guid", "unique_guid"},
            f"data partition {index}",
        )
        name = partition_name(item)
        if name != expected:
            raise DiskError(f"data partition {index} must be {expected}")
        if name in system_names:
            raise DiskError(f"data partition {name} repeats a system disk name")
        start = require_int(item, "start_bytes", minimum=alignment)
        last = expected == DATA_PARTITION_NAMES[-1]
        size = 0
        if last:
            if item["size_bytes"] is not None:
                raise DiskError("the last data partition must fill the disk")
        else:
            size = require_int(item, "size_bytes", minimum=1)
            if size % alignment:
                raise DiskError(f"data partition {name} is not aligned")
        if start % alignment or start < previous_end:
            raise DiskError(f"data partition {name} is misaligned or out of order")
        previous_end = start + size
        parse_guid(item, "type_guid")
        unique = parse_guid(item, "unique_guid")
        if unique in seen_guids:
            raise DiskError(f"data partition {name} duplicates a unique GUID")
        seen_guids.add(unique)
    user_start = partitions[-1]["start_bytes"]
    if user_start + trailing >= minimum * GIB:
        raise DiskError("the smallest data disk leaves userdata no room")


# --- the release layout: what one build produced -----------------------------


@dataclass(frozen=True)
class SystemLayout:
    geometry: Geometry
    alignment_bytes: int
    disk_guid: uuid.UUID
    partitions: tuple[Partition, ...]
    hashes: dict[str, str]
    image_bytes: dict[str, int]
    disk_sha256: str


@dataclass(frozen=True)
class Layout:
    system: SystemLayout
    data: dict[str, object]


def load_layout(path: Path) -> Layout:
    document = read_json(path, "partition layout")
    require_keys(document, {"schema_version", "sector_size", "system_disk", "data_disk"},
                 "partition layout")
    if require_int(document, "schema_version") != LAYOUT_SCHEMA_VERSION:
        raise DiskError("only partition layout schema version 2 is supported")
    if require_int(document, "sector_size") != SECTOR_SIZE:
        raise DiskError("GPT sector size must be exactly 512 bytes")
    system = require_object(document, "system_disk")
    require_keys(
        system,
        {
            "size_bytes", "sha256", "alignment_bytes", "partition_entry_count",
            "disk_guid", "partitions",
        },
        "system disk",
    )
    size = require_int(system, "size_bytes", minimum=SECTOR_SIZE)
    alignment = require_int(system, "alignment_bytes", minimum=4096)
    entry_count = require_int(system, "partition_entry_count", minimum=1)
    check_entry_count(entry_count)
    if size % alignment or alignment % 4096:
        raise DiskError("system disk size and alignment must be multiples of 4 KiB")
    geometry = Geometry(size, entry_count)
    disk_sha256 = require_string(system, "sha256")
    if SHA256_PATTERN.fullmatch(disk_sha256) is None:
        raise DiskError("system disk sha256 is not a lower-case hex digest")
    entries = system["partitions"]
    if not isinstance(entries, list) or not entries or len(entries) > entry_count:
        raise DiskError("system disk partitions must be a non-empty array")
    partitions: list[Partition] = []
    hashes: dict[str, str] = {}
    image_bytes: dict[str, int] = {}
    names: set[str] = set()
    guids: set[uuid.UUID] = set()
    cursor = align_up(geometry.first_usable_lba * SECTOR_SIZE, alignment)
    for index, item in enumerate(entries):
        if not isinstance(item, dict):
            raise DiskError(f"system partition {index} must be an object")
        require_keys(
            item,
            {
                "name", "start_bytes", "size_bytes", "image_bytes", "sha256",
                "type_guid", "unique_guid",
            },
            f"system partition {index}",
        )
        name = partition_name(item)
        start = require_int(item, "start_bytes")
        length = require_int(item, "size_bytes", minimum=alignment)
        used = require_int(item, "image_bytes", minimum=1)
        digest = require_string(item, "sha256")
        if SHA256_PATTERN.fullmatch(digest) is None:
            raise DiskError(f"partition {name} sha256 is not a lower-case hex digest")
        if start % alignment or length % alignment:
            raise DiskError(f"partition {name} is not aligned to {alignment} bytes")
        if used > length or length - used >= alignment:
            raise DiskError(f"partition {name} is not exact fit")
        # Exact fit: the first partition follows the table at the next
        # alignment boundary and each later one starts where its predecessor
        # ends.
        if start != cursor:
            raise DiskError(f"partition {name} is not packed against its predecessor")
        unique = parse_guid(item, "unique_guid")
        if name in names or unique in guids:
            raise DiskError(f"partition {name} duplicates a name or unique GUID")
        names.add(name)
        guids.add(unique)
        partitions.append(
            Partition(name, start, length, parse_guid(item, "type_guid"), unique)
        )
        hashes[name] = digest
        image_bytes[name] = used
        cursor = start + length
    if cursor > (geometry.last_usable_lba + 1) * SECTOR_SIZE:
        raise DiskError("last partition overlaps the backup GPT")
    if align_up(cursor + (geometry.table_sectors + 1) * SECTOR_SIZE, alignment) != size:
        raise DiskError("system disk has bytes beyond its exact fit")
    for required in BOOT_PARTITION_NAMES:
        if required not in names:
            raise DiskError(f"system disk lacks {required}")
    data = require_object(document, "data_disk")
    validate_data_disk(data, names)
    return Layout(
        SystemLayout(
            geometry, alignment, parse_guid(system, "disk_guid"), tuple(partitions),
            hashes, image_bytes, disk_sha256,
        ),
        data,
    )


# --- assembling ----------------------------------------------------------------


def sha256_range(source: BinaryIO, offset: int, size: int) -> str:
    digest = hashlib.sha256()
    source.seek(offset)
    remaining = size
    while remaining:
        block = source.read(min(remaining, COPY_CHUNK))
        if not block:
            raise DiskError("disk image is truncated")
        digest.update(block)
        remaining -= len(block)
    return digest.hexdigest()


def sha256_file(path: Path) -> str:
    with path.open("rb") as source:
        return sha256_range(source, 0, path.stat().st_size)


def copy_exact(source: BinaryIO, destination: BinaryIO, byte_count: int) -> None:
    remaining = byte_count
    while remaining:
        block = source.read(min(remaining, COPY_CHUNK))
        if not block:
            raise DiskError("partition image is truncated")
        destination.write(block)
        remaining -= len(block)


def write_fill(destination: BinaryIO, pattern: bytes, byte_count: int) -> None:
    if pattern == b"\0\0\0\0":
        destination.seek(byte_count, os.SEEK_CUR)
        return
    block = pattern * (COPY_CHUNK // len(pattern))
    remaining = byte_count
    while remaining:
        piece = block[: min(remaining, len(block))]
        destination.write(piece)
        remaining -= len(piece)


def read_sparse_header(source: BinaryIO) -> tuple[int, int, int, int, int]:
    """Parse and check a sparse header, returning the sizes the chunks need."""
    raw_header = source.read(SPARSE_HEADER.size)
    if len(raw_header) != SPARSE_HEADER.size:
        raise DiskError("Android sparse header is truncated")
    (
        magic,
        major,
        _minor,
        file_header_size,
        chunk_header_size,
        block_size,
        total_blocks,
        total_chunks,
        _checksum,
    ) = SPARSE_HEADER.unpack(raw_header)
    if magic != SPARSE_MAGIC or major != 1:
        raise DiskError("unsupported Android sparse image version")
    if file_header_size < SPARSE_HEADER.size or chunk_header_size < SPARSE_CHUNK_HEADER.size:
        raise DiskError("Android sparse header sizes are invalid")
    if block_size == 0 or block_size % 4:
        raise DiskError("Android sparse block size is invalid")
    if total_blocks == 0:
        raise DiskError("Android sparse image is empty")
    source.seek(file_header_size - SPARSE_HEADER.size, os.SEEK_CUR)
    return file_header_size, chunk_header_size, block_size, total_blocks, total_chunks


def expand_sparse(source: BinaryIO, destination: BinaryIO) -> int:
    """Write the raw bytes an Android sparse image stands for and return their count.

    The destination is a pre-sized zero-filled disk, so a DONT_CARE chunk and a
    zero FILL chunk only advance it. The expanded bytes are exactly the bytes
    the guest reads from the partition.
    """
    _file_header, chunk_header_size, block_size, total_blocks, total_chunks = (
        read_sparse_header(source)
    )
    written_blocks = 0
    for _ in range(total_chunks):
        raw_chunk = source.read(SPARSE_CHUNK_HEADER.size)
        if len(raw_chunk) != SPARSE_CHUNK_HEADER.size:
            raise DiskError("Android sparse chunk header is truncated")
        chunk_type, _reserved, chunk_blocks, total_size = SPARSE_CHUNK_HEADER.unpack(
            raw_chunk
        )
        source.seek(chunk_header_size - SPARSE_CHUNK_HEADER.size, os.SEEK_CUR)
        chunk_bytes = chunk_blocks * block_size
        if total_size < chunk_header_size:
            raise DiskError("Android sparse chunk size underflows its header")
        data_size = total_size - chunk_header_size
        if written_blocks + chunk_blocks > total_blocks:
            raise DiskError("Android sparse chunks exceed declared block count")
        if chunk_type == SPARSE_RAW:
            if data_size != chunk_bytes:
                raise DiskError("Android sparse RAW chunk has the wrong byte count")
            copy_exact(source, destination, chunk_bytes)
        elif chunk_type == SPARSE_FILL:
            if data_size != 4:
                raise DiskError("Android sparse FILL chunk must contain four bytes")
            pattern = source.read(4)
            if len(pattern) != 4:
                raise DiskError("Android sparse FILL chunk is truncated")
            write_fill(destination, pattern, chunk_bytes)
        elif chunk_type == SPARSE_DONT_CARE:
            if data_size != 0:
                raise DiskError("Android sparse DONT_CARE chunk contains data")
            destination.seek(chunk_bytes, os.SEEK_CUR)
        elif chunk_type == SPARSE_CRC32:
            if chunk_blocks != 0 or data_size != 4:
                raise DiskError("Android sparse CRC32 chunk is malformed")
            if len(source.read(4)) != 4:
                raise DiskError("Android sparse CRC32 chunk is truncated")
        else:
            raise DiskError(f"unknown Android sparse chunk type 0x{chunk_type:04x}")
        written_blocks += chunk_blocks
    if written_blocks != total_blocks:
        raise DiskError("Android sparse image block count does not match its chunks")
    if source.read(1):
        raise DiskError("Android sparse image has trailing data")
    return total_blocks * block_size


def is_sparse(source: BinaryIO) -> bool:
    prefix = source.read(4)
    source.seek(0)
    return len(prefix) == 4 and struct.unpack("<I", prefix)[0] == SPARSE_MAGIC


def image_size(image: Path) -> int:
    """The bytes the guest reads from the partition: a raw image's size, or the
    size an Android sparse image expands to."""
    if image.is_symlink() or not image.is_file():
        raise DiskError(f"partition image must be a regular non-symlink file: {image}")
    with image.open("rb") as source:
        if is_sparse(source):
            _file_header, _chunk_header, block_size, total_blocks, _chunks = (
                read_sparse_header(source)
            )
            size = block_size * total_blocks
        else:
            size = image.stat().st_size
    if size <= 0:
        raise DiskError(f"partition image is empty: {image.name}")
    if size > MAX_PARTITION_IMAGE_BYTES:
        raise DiskError(
            f"partition image {image.name} is {size} bytes, limit is "
            f"{MAX_PARTITION_IMAGE_BYTES}"
        )
    return size


def write_partition_image(image: Path, destination: BinaryIO, expected: int) -> None:
    """Write the image at the destination's position, expanding a sparse one.

    `expected` is the size image_size reported, which placed the partition."""
    with image.open("rb") as source:
        if is_sparse(source):
            written = expand_sparse(source, destination)
        else:
            written = image.stat().st_size
            copy_exact(source, destination, written)
            if source.read(1):
                raise DiskError(f"partition image grew while it was staged: {image.name}")
    if written != expected:
        raise DiskError(
            f"partition image {image.name} changed while it was staged: "
            f"{written} bytes, expected {expected}"
        )


def parse_overrides(values: list[str], known: set[str]) -> dict[str, Path]:
    overrides: dict[str, Path] = {}
    for value in values:
        name, separator, path_text = value.partition("=")
        if not separator or not name or not path_text:
            raise DiskError("partition image override must be NAME=ABSOLUTE_PATH")
        if name in overrides:
            raise DiskError(f"partition image override is repeated: {name}")
        if name not in known:
            raise DiskError(f"partition image override is absent from the policy: {name}")
        path = Path(path_text)
        if not path.is_absolute():
            raise DiskError(f"partition image override must be absolute: {name}")
        if path.is_symlink():
            raise DiskError(f"partition image override must not be a symlink: {name}")
        overrides[name] = path
    return overrides


def assemble(
    policy: Policy,
    images_dir: Path,
    output_dir: Path,
    image_overrides: list[str],
) -> None:
    if images_dir.is_symlink() or not images_dir.is_dir():
        raise DiskError(f"images directory must be a non-symlink directory: {images_dir}")
    disk_path = output_dir / SYSTEM_DISK_FILE
    layout_path = output_dir / LAYOUT_FILE
    for existing in (disk_path, layout_path):
        if existing.exists() or existing.is_symlink():
            raise DiskError(f"output already exists: {existing}")
    system = policy.document["system_disk"]
    alignment = system["alignment_bytes"]
    entry_count = system["partition_entry_count"]
    entries = list(policy.boot) + list(policy.filesystems)
    overrides = parse_overrides(image_overrides, {entry["name"] for entry in entries})

    sources: dict[str, Path] = {}
    sizes: dict[str, int] = {}
    for entry in entries:
        name = entry["name"]
        sources[name] = overrides.get(name, images_dir / entry["image"])
        sizes[name] = image_size(sources[name])
    # Boot containers first, then the filesystems largest first. The order is
    # deterministic and carries no meaning.
    ordered = list(policy.boot) + sorted(
        policy.filesystems, key=lambda entry: (-sizes[entry["name"]], entry["name"])
    )

    probe = Geometry(SECTOR_SIZE * 2, entry_count)
    cursor = align_up(probe.first_usable_lba * SECTOR_SIZE, alignment)
    placed: list[dict[str, object]] = []
    for entry in ordered:
        name = entry["name"]
        length = align_up(sizes[name], alignment)
        placed.append({
            "name": name,
            "start_bytes": cursor,
            "size_bytes": length,
            "image_bytes": sizes[name],
            "type_guid": entry["type_guid"],
            "unique_guid": entry["unique_guid"],
        })
        cursor += length
    disk_size = align_up(cursor + (probe.table_sectors + 1) * SECTOR_SIZE, alignment)
    geometry = Geometry(disk_size, entry_count)
    partitions = tuple(
        Partition(
            item["name"], item["start_bytes"], item["size_bytes"],
            uuid.UUID(item["type_guid"]), uuid.UUID(item["unique_guid"]),
        )
        for item in placed
    )
    disk_guid = uuid.UUID(system["disk_guid"])

    output_dir.mkdir(parents=True, exist_ok=True)
    temporary = output_dir / f".{SYSTEM_DISK_FILE}.tmp.{os.getpid()}"
    layout_temporary = output_dir / f".{LAYOUT_FILE}.tmp.{os.getpid()}"
    try:
        with temporary.open("xb") as disk:
            disk.truncate(disk_size)
            write_gpt(disk, geometry, disk_guid, partitions)
            for item in placed:
                disk.seek(item["start_bytes"])
                write_partition_image(sources[item["name"]], disk, sizes[item["name"]])
                print(
                    f"staged {item['name']}: {sizes[item['name']]}/{item['size_bytes']} "
                    f"bytes at {item['start_bytes']}"
                )
            disk.flush()
            os.fsync(disk.fileno())
        with temporary.open("rb") as disk:
            for item in placed:
                item["sha256"] = sha256_range(disk, item["start_bytes"], item["size_bytes"])
        layout = {
            "schema_version": LAYOUT_SCHEMA_VERSION,
            "sector_size": SECTOR_SIZE,
            "system_disk": {
                "size_bytes": disk_size,
                "sha256": sha256_file(temporary),
                "alignment_bytes": alignment,
                "partition_entry_count": entry_count,
                "disk_guid": system["disk_guid"],
                "partitions": [
                    {
                        key: item[key]
                        for key in (
                            "name", "start_bytes", "size_bytes", "image_bytes",
                            "sha256", "type_guid", "unique_guid",
                        )
                    }
                    for item in placed
                ],
            },
            "data_disk": policy.document["data_disk"],
        }
        layout_temporary.write_text(
            json.dumps(layout, indent=2) + "\n", encoding="utf-8"
        )
        # The tool's own check of what it wrote, before either file is published.
        verify_layout_and_disk(load_layout(layout_temporary), temporary, check_hashes=True)
        os.replace(temporary, disk_path)
        os.replace(layout_temporary, layout_path)
    finally:
        temporary.unlink(missing_ok=True)
        layout_temporary.unlink(missing_ok=True)
    print(
        f"assembled the system disk: {disk_size} bytes, "
        f"{len(placed)} partitions, sha256 {layout['system_disk']['sha256']}"
    )


# --- verifying -----------------------------------------------------------------


def verify_layout_and_disk(layout: Layout, disk_path: Path, *, check_hashes: bool) -> None:
    system = layout.system
    verify_gpt(disk_path, system.geometry, system.disk_guid, system.partitions)
    if not check_hashes:
        return
    with disk_path.open("rb") as disk:
        for partition in system.partitions:
            observed = sha256_range(disk, partition.start_bytes, partition.size_bytes)
            if observed != system.hashes[partition.name]:
                raise DiskError(f"partition {partition.name} differs from its recorded sha256")
        # The slack between the last partition and the backup table is zero.
        end = system.partitions[-1].start_bytes + system.partitions[-1].size_bytes
        gap_end = system.geometry.backup_entries_lba * SECTOR_SIZE
        disk.seek(end)
        if disk.read(gap_end - end).strip(b"\0"):
            raise DiskError("the slack beyond the last partition is not zero")
    if sha256_file(disk_path) != system.disk_sha256:
        raise DiskError("system disk differs from its recorded sha256")


def data_disk_gpt(
    layout: Layout, capacity_gib: int
) -> tuple[Geometry, uuid.UUID, tuple[Partition, ...]]:
    """The metadata of a data disk of `capacity_gib` GiB, as the host builds it."""
    data = layout.data
    capacity = data["capacity_gib"]
    if not capacity["minimum"] <= capacity_gib <= capacity["maximum"]:
        raise DiskError(
            f"data disk capacity must be {capacity['minimum']} through "
            f"{capacity['maximum']} GiB"
        )
    size = capacity_gib * GIB
    geometry = Geometry(size, data["partition_entry_count"])
    partitions = tuple(
        Partition(
            item["name"],
            item["start_bytes"],
            item["size_bytes"]
            if item["size_bytes"] is not None
            else size - data["trailing_free_bytes"] - item["start_bytes"],
            uuid.UUID(item["type_guid"]),
            uuid.UUID(item["unique_guid"]),
        )
        for item in data["partitions"]
    )
    return geometry, uuid.UUID(data["disk_guid"]), partitions


def create_data_disk(layout_path: Path, capacity_gib: int | None, output: Path) -> None:
    """A sparse data disk with only its partition tables written. The host
    creates the same bytes for each instance; this is the reference."""
    layout = load_layout(layout_path)
    gib = capacity_gib if capacity_gib is not None else layout.data["capacity_gib"]["default"]
    geometry, disk_guid, partitions = data_disk_gpt(layout, gib)
    with output.open("xb") as disk:
        disk.truncate(geometry.disk_size_bytes)
        write_gpt(disk, geometry, disk_guid, partitions)
    print(f"created data disk: {gib} GiB, {output}")


def verify_data_disk(layout_path: Path, disk_path: Path) -> None:
    layout = load_layout(layout_path)
    if disk_path.is_symlink() or not disk_path.is_file():
        raise DiskError(f"disk image must be a regular non-symlink file: {disk_path}")
    size = disk_path.stat().st_size
    if size % GIB:
        raise DiskError("data disk is not a whole number of GiB")
    geometry, disk_guid, partitions = data_disk_gpt(layout, size // GIB)
    verify_gpt(disk_path, geometry, disk_guid, partitions)
    print(f"verified data disk: {size // GIB} GiB")


def check_against_policy(layout: Layout, policy: Policy) -> None:
    """A release's layout is what the tracked policy produces: the same
    identities for every partition and the same data disk template."""
    system = policy.document["system_disk"]
    if layout.data != policy.document["data_disk"]:
        raise DiskError("the data disk template differs from the tracked policy")
    if (
        layout.system.disk_guid != uuid.UUID(system["disk_guid"])
        or layout.system.alignment_bytes != system["alignment_bytes"]
        or layout.system.geometry.partition_entry_count != system["partition_entry_count"]
    ):
        raise DiskError("the system disk geometry differs from the tracked policy")
    expected = {
        entry["name"]: entry for entry in list(policy.boot) + list(policy.filesystems)
    }
    if {partition.name for partition in layout.system.partitions} != set(expected):
        raise DiskError("the system disk partitions differ from the tracked policy")
    for partition in layout.system.partitions:
        entry = expected[partition.name]
        if (
            partition.type_guid != uuid.UUID(entry["type_guid"])
            or partition.unique_guid != uuid.UUID(entry["unique_guid"])
        ):
            raise DiskError(f"partition {partition.name} GUIDs differ from the tracked policy")
    boot_count = len(policy.boot)
    if [partition.name for partition in layout.system.partitions[:boot_count]] != [
        entry["name"] for entry in policy.boot
    ]:
        raise DiskError("the boot partitions do not come first")


def verify(
    layout_path: Path, disk_path: Path, check_hashes: bool, policy_path: Path | None
) -> None:
    layout = load_layout(layout_path)
    if policy_path is not None:
        check_against_policy(layout, load_policy(policy_path))
    verify_layout_and_disk(layout, disk_path, check_hashes=check_hashes)
    suffix = ", every sha256 matches" if check_hashes else ""
    print(
        f"verified system disk: {len(layout.system.partitions)} partitions, "
        f"{layout.system.geometry.disk_size_bytes} bytes{suffix}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Assemble or verify the Android 17 read-only system disk"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("--policy", type=Path, required=True)
    assemble_parser = subparsers.add_parser("assemble")
    assemble_parser.add_argument("--policy", type=Path, required=True)
    assemble_parser.add_argument("--images-dir", type=Path, required=True)
    assemble_parser.add_argument("--output-dir", type=Path, required=True)
    assemble_parser.add_argument("--image", action="append", default=[])
    data_parser = subparsers.add_parser(
        "data", help="create a sparse data disk (the reference for the host's)"
    )
    data_parser.add_argument("--layout", type=Path, required=True)
    data_parser.add_argument("--capacity-gib", type=int)
    data_parser.add_argument("--output", type=Path, required=True)
    verify_data_parser = subparsers.add_parser(
        "verify-data", help="check a data disk against the layout's template"
    )
    verify_data_parser.add_argument("--layout", type=Path, required=True)
    verify_data_parser.add_argument("--disk", type=Path, required=True)
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--layout", type=Path, required=True)
    verify_parser.add_argument("--disk", type=Path, required=True)
    verify_parser.add_argument(
        "--hash", action="store_true",
        help="also check every partition's and the disk's SHA-256",
    )
    verify_parser.add_argument(
        "--policy", type=Path,
        help="also require the layout to be what this disk policy produces",
    )
    args = parser.parse_args()
    try:
        if args.command == "validate":
            policy = load_policy(args.policy.resolve())
            print(
                "validated disk policy: "
                f"{len(policy.boot) + len(policy.filesystems)} system partitions"
            )
        elif args.command == "assemble":
            assemble(
                load_policy(args.policy.resolve()),
                args.images_dir.resolve(),
                args.output_dir.resolve(),
                args.image,
            )
        elif args.command == "data":
            create_data_disk(args.layout.resolve(), args.capacity_gib, args.output.resolve())
        elif args.command == "verify-data":
            verify_data_disk(args.layout.resolve(), args.disk.resolve())
        else:
            verify(
                args.layout.resolve(), args.disk.resolve(), args.hash,
                args.policy.resolve() if args.policy else None,
            )
    except (DiskError, OSError) as error:
        raise SystemExit(f"UBD disk: {error}") from error


if __name__ == "__main__":
    main()
