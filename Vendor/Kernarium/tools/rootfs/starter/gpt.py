#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read and validate the single-partition GPT/ext4 starter-image contract."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import binascii
import json
import struct
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

SECTOR_SIZE = 512
GPT_SIGNATURE = b"EFI PART"
GPT_REVISION = 0x00010000
GPT_HEADER_SIZE = 92
GPT_ENTRY_SIZE = 128
LINUX_FILESYSTEM_GUID = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")
EXT4_MAGIC = 0xEF53
EXT4_VALID_FS = 0x0001
EXT4_FEATURE_INCOMPAT_64BIT = 0x0080

HEADER = struct.Struct("<8sIIIIQQQQ16sQIII")
ENTRY = struct.Struct("<16s16sQQQ72s")


class ImageError(RuntimeError):
    """The raw disk does not meet the starter-image contract."""


@dataclass(frozen=True)
class GPTHeader:
    current_lba: int
    backup_lba: int
    first_usable_lba: int
    last_usable_lba: int
    disk_guid: uuid.UUID
    entries_lba: int
    entry_count: int
    entry_size: int
    entries_crc32: int


@dataclass(frozen=True)
class Partition:
    type_guid: uuid.UUID
    unique_guid: uuid.UUID
    first_lba: int
    last_lba: int
    attributes: int
    name: str

    @property
    def byte_offset(self) -> int:
        return self.first_lba * SECTOR_SIZE

    @property
    def byte_size(self) -> int:
        return (self.last_lba - self.first_lba + 1) * SECTOR_SIZE


def _crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def _read_exact(image: BinaryIO, offset: int, length: int, label: str) -> bytes:
    image.seek(offset)
    data = image.read(length)
    if len(data) != length:
        raise ImageError(f"{label} is truncated")
    return data


def _read_header(image: BinaryIO, lba: int, disk_lbas: int) -> GPTHeader:
    sector = _read_exact(
        image, lba * SECTOR_SIZE, SECTOR_SIZE, f"GPT header at LBA {lba}"
    )
    values = HEADER.unpack_from(sector)
    (
        signature,
        revision,
        header_size,
        stored_crc,
        reserved,
        current_lba,
        backup_lba,
        first_usable,
        last_usable,
        disk_guid_bytes,
        entries_lba,
        entry_count,
        entry_size,
        entries_crc,
    ) = values
    if signature != GPT_SIGNATURE:
        raise ImageError(f"GPT header at LBA {lba} has no EFI PART signature")
    if revision != GPT_REVISION or header_size != GPT_HEADER_SIZE or reserved != 0:
        raise ImageError(f"GPT header at LBA {lba} has unsupported geometry")
    checked = bytearray(sector[:header_size])
    struct.pack_into("<I", checked, 16, 0)
    if _crc32(checked) != stored_crc:
        raise ImageError(f"GPT header at LBA {lba} has an invalid CRC32")
    if current_lba != lba or backup_lba >= disk_lbas:
        raise ImageError(f"GPT header at LBA {lba} points outside the disk")
    if entry_size != GPT_ENTRY_SIZE or entry_count < 1:
        raise ImageError("GPT uses an unsupported partition-entry layout")
    if not (first_usable <= last_usable < disk_lbas):
        raise ImageError("GPT usable-LBA range is invalid")
    return GPTHeader(
        current_lba=current_lba,
        backup_lba=backup_lba,
        first_usable_lba=first_usable,
        last_usable_lba=last_usable,
        disk_guid=uuid.UUID(bytes_le=disk_guid_bytes),
        entries_lba=entries_lba,
        entry_count=entry_count,
        entry_size=entry_size,
        entries_crc32=entries_crc,
    )


def _read_entries(image: BinaryIO, header: GPTHeader) -> tuple[bytes, list[Partition]]:
    byte_count = header.entry_count * header.entry_size
    raw = _read_exact(
        image,
        header.entries_lba * SECTOR_SIZE,
        byte_count,
        "GPT partition-entry array",
    )
    if _crc32(raw) != header.entries_crc32:
        raise ImageError("GPT partition-entry array has an invalid CRC32")
    partitions: list[Partition] = []
    for index in range(header.entry_count):
        offset = index * header.entry_size
        type_bytes, unique_bytes, first, last, attributes, name_bytes = (
            ENTRY.unpack_from(raw, offset)
        )
        if type_bytes == bytes(16):
            continue
        try:
            name = name_bytes.decode("utf-16-le").split("\0", 1)[0]
        except UnicodeDecodeError as error:
            raise ImageError("GPT partition name is not valid UTF-16LE") from error
        partitions.append(
            Partition(
                type_guid=uuid.UUID(bytes_le=type_bytes),
                unique_guid=uuid.UUID(bytes_le=unique_bytes),
                first_lba=first,
                last_lba=last,
                attributes=attributes,
                name=name,
            )
        )
    return raw, partitions


def _read_ext4(image: BinaryIO, partition: Partition) -> dict[str, object]:
    superblock = _read_exact(
        image, partition.byte_offset + 1024, 1024, "ext4 superblock"
    )
    magic = struct.unpack_from("<H", superblock, 56)[0]
    if magic != EXT4_MAGIC:
        raise ImageError("the sole GPT partition is not ext4")
    state = struct.unpack_from("<H", superblock, 58)[0]
    if state & EXT4_VALID_FS == 0:
        raise ImageError("ext4 filesystem is not marked clean")
    log_block_size = struct.unpack_from("<I", superblock, 24)[0]
    if log_block_size > 6:
        raise ImageError("ext4 block-size shift is invalid")
    block_size = 1024 << log_block_size
    incompat = struct.unpack_from("<I", superblock, 96)[0]
    blocks = struct.unpack_from("<I", superblock, 4)[0]
    if incompat & EXT4_FEATURE_INCOMPAT_64BIT:
        blocks |= struct.unpack_from("<I", superblock, 0x150)[0] << 32
    filesystem_bytes = blocks * block_size
    if blocks == 0 or filesystem_bytes > partition.byte_size:
        raise ImageError("ext4 filesystem exceeds its GPT partition")
    volume_name = superblock[120:136].split(b"\0", 1)[0].decode(
        "utf-8", errors="strict"
    )
    return {
        "block_size": block_size,
        "blocks": blocks,
        "bytes": filesystem_bytes,
        "uuid": str(uuid.UUID(bytes=superblock[104:120])),
        "volume_name": volume_name,
    }


def inspect(path: Path) -> dict[str, object]:
    if path.is_symlink() or not path.is_file():
        raise ImageError(f"image is not a regular non-symlink file: {path}")
    size = path.stat().st_size
    if size < 68 * SECTOR_SIZE or size % SECTOR_SIZE:
        raise ImageError("image size is not valid GPT sector geometry")
    disk_lbas = size // SECTOR_SIZE
    with path.open("rb") as image:
        primary = _read_header(image, 1, disk_lbas)
        backup = _read_header(image, disk_lbas - 1, disk_lbas)
        primary_raw, partitions = _read_entries(image, primary)
        backup_raw, backup_partitions = _read_entries(image, backup)
        if primary.backup_lba != backup.current_lba or backup.backup_lba != 1:
            raise ImageError("primary and backup GPT headers do not refer to each other")
        if (
            primary.disk_guid != backup.disk_guid
            or primary.first_usable_lba != backup.first_usable_lba
            or primary.last_usable_lba != backup.last_usable_lba
            or primary_raw != backup_raw
            or partitions != backup_partitions
        ):
            raise ImageError("primary and backup GPT copies disagree")
        if len(partitions) != 1:
            raise ImageError(f"starter image must contain one partition, found {len(partitions)}")
        partition = partitions[0]
        if partition.type_guid != LINUX_FILESYSTEM_GUID:
            raise ImageError("starter partition does not use the Linux filesystem GUID")
        if partition.first_lba != 2048:
            raise ImageError("starter partition must begin at the 1 MiB boundary")
        if partition.last_lba != primary.last_usable_lba:
            raise ImageError("starter partition does not end at the last usable LBA")
        if partition.first_lba > partition.last_lba:
            raise ImageError("starter partition LBA range is empty")
        filesystem = _read_ext4(image, partition)
    return {
        "image": str(path),
        "size": size,
        "disk_lbas": disk_lbas,
        "disk_guid": str(primary.disk_guid),
        "partition": {
            "type_guid": str(partition.type_guid),
            "unique_guid": str(partition.unique_guid),
            "first_lba": partition.first_lba,
            "last_lba": partition.last_lba,
            "bytes": partition.byte_size,
            "name": partition.name,
        },
        "filesystem": filesystem,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    arguments = parser.parse_args()
    try:
        result = inspect(arguments.image.resolve())
    except (ImageError, OSError, UnicodeDecodeError) as error:
        print(f"starter image verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
