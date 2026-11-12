#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify the starter-image GPT reader on synthetic GPT/ext4 fixtures."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import binascii
import struct
import sys
import tempfile
import uuid
from pathlib import Path


SECTOR_SIZE = 512
DISK_LBAS = 4096
FIRST_USABLE_LBA = 34
LAST_USABLE_LBA = DISK_LBAS - 34
PARTITION_FIRST_LBA = 2048
ENTRY_COUNT = 128
ENTRY_SIZE = 128
HEADER_SIZE = 92


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def header(
    *,
    current_lba: int,
    backup_lba: int,
    entries_lba: int,
    entries_crc: int,
    disk_guid: uuid.UUID,
) -> bytes:
    fields = (
        b"EFI PART",
        0x00010000,
        HEADER_SIZE,
        0,
        0,
        current_lba,
        backup_lba,
        FIRST_USABLE_LBA,
        LAST_USABLE_LBA,
        disk_guid.bytes_le,
        entries_lba,
        ENTRY_COUNT,
        ENTRY_SIZE,
        entries_crc,
    )
    packed = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", *fields))
    struct.pack_into("<I", packed, 16, binascii.crc32(packed) & 0xFFFFFFFF)
    return bytes(packed).ljust(SECTOR_SIZE, b"\0")


def write_gpt_ext4_fixture(path: Path, gpt: object) -> None:
    disk = bytearray(DISK_LBAS * SECTOR_SIZE)
    disk_guid = uuid.UUID("7db8173c-a7fc-4f80-8eb1-1211bd50ff99")
    partition_guid = uuid.UUID("59ba0501-9142-4bed-99ef-9e3f584f5c54")
    name = "rootfs".encode("utf-16-le").ljust(72, b"\0")
    first_entry = struct.pack(
        "<16s16sQQQ72s",
        gpt.LINUX_FILESYSTEM_GUID.bytes_le,
        partition_guid.bytes_le,
        PARTITION_FIRST_LBA,
        LAST_USABLE_LBA,
        0,
        name,
    )
    entries = first_entry.ljust(ENTRY_COUNT * ENTRY_SIZE, b"\0")
    entries_crc = binascii.crc32(entries) & 0xFFFFFFFF
    primary_entries = 2 * SECTOR_SIZE
    backup_entries_lba = DISK_LBAS - 33
    backup_entries = backup_entries_lba * SECTOR_SIZE
    disk[primary_entries : primary_entries + len(entries)] = entries
    disk[backup_entries : backup_entries + len(entries)] = entries
    disk[SECTOR_SIZE : 2 * SECTOR_SIZE] = header(
        current_lba=1,
        backup_lba=DISK_LBAS - 1,
        entries_lba=2,
        entries_crc=entries_crc,
        disk_guid=disk_guid,
    )
    disk[-SECTOR_SIZE:] = header(
        current_lba=DISK_LBAS - 1,
        backup_lba=1,
        entries_lba=backup_entries_lba,
        entries_crc=entries_crc,
        disk_guid=disk_guid,
    )

    superblock_offset = PARTITION_FIRST_LBA * SECTOR_SIZE + 1024
    superblock = memoryview(disk)[superblock_offset : superblock_offset + 1024]
    struct.pack_into("<I", superblock, 4, 128)
    struct.pack_into("<I", superblock, 24, 2)
    struct.pack_into("<H", superblock, 56, gpt.EXT4_MAGIC)
    struct.pack_into("<H", superblock, 58, gpt.EXT4_VALID_FS)
    filesystem_uuid = uuid.UUID("d938498c-08fa-4d88-a51d-2f4ff507963a")
    superblock[104:120] = filesystem_uuid.bytes
    superblock[120:136] = b"fixture\0".ljust(16, b"\0")
    path.write_bytes(disk)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--starter-root", required=True, type=Path)
    arguments = parser.parse_args()

    starter_root = arguments.starter_root.resolve()
    sys.path.insert(0, str(starter_root))
    import gpt  # pylint: disable=import-outside-toplevel

    with tempfile.TemporaryDirectory(prefix="uml-starter-gpt-test-") as name:
        image = Path(name) / "fixture.img"
        write_gpt_ext4_fixture(image, gpt)
        result = gpt.inspect(image)
        require(result["size"] == DISK_LBAS * SECTOR_SIZE, "wrong disk size")
        partition = result["partition"]
        filesystem = result["filesystem"]
        require(partition["first_lba"] == PARTITION_FIRST_LBA, "wrong start LBA")
        require(partition["last_lba"] == LAST_USABLE_LBA, "wrong end LBA")
        require(partition["name"] == "rootfs", "wrong partition name")
        require(filesystem["block_size"] == 4096, "wrong ext4 block size")
        require(filesystem["bytes"] == 128 * 4096, "wrong ext4 byte size")

        corrupted = bytearray(image.read_bytes())
        corrupted[SECTOR_SIZE + 24] ^= 0x01
        image.write_bytes(corrupted)
        try:
            gpt.inspect(image)
        except gpt.ImageError as error:
            require("invalid CRC32" in str(error), "wrong corrupt-GPT error")
        else:
            raise RuntimeError("corrupt GPT header was accepted")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
