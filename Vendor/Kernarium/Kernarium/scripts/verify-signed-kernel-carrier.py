#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify that an AArch64 ET_DYN image is safe for the signed carrier."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import struct
from pathlib import Path


ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
PT_LOAD = 1
ET_DYN = 3
EM_AARCH64 = 183
PF_X = 1
PF_W = 2
EXECUTABLE = 1
NONEXECUTABLE = 2


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def align_down(value: int, alignment: int) -> int:
    return value & -alignment


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & -alignment


def verify(image: bytes, host_page_size: int) -> int:
    require(
        host_page_size >= 4096
        and host_page_size & (host_page_size - 1) == 0,
        "host page size must be a power of two",
    )
    require(len(image) >= ELF_HEADER.size, "truncated ELF header")
    (
        identity,
        elf_type,
        machine,
        _version,
        _entry,
        program_offset,
        _section_offset,
        _flags,
        _header_size,
        program_entry_size,
        program_count,
        _section_entry_size,
        _section_count,
        _section_names,
    ) = ELF_HEADER.unpack_from(image)
    require(identity[:6] == b"\x7fELF\x02\x01", "not a little-endian ELF64 image")
    require(elf_type == ET_DYN, "kernel image is not ET_DYN")
    require(machine == EM_AARCH64, "kernel image is not AArch64")
    require(program_entry_size == PROGRAM_HEADER.size, "unexpected program header size")
    require(program_count > 0, "kernel image has no program headers")
    table_size = program_count * program_entry_size
    require(
        program_offset <= len(image)
        and table_size <= len(image) - program_offset,
        "program header table is outside the image",
    )

    page_kinds: dict[int, int] = {}
    writable_nonexecutable_pages: set[int] = set()
    load_count = 0
    executable_count = 0
    for index in range(program_count):
        offset = program_offset + index * program_entry_size
        (
            kind,
            flags,
            file_offset,
            virtual_address,
            _physical_address,
            file_size,
            memory_size,
            _alignment,
        ) = PROGRAM_HEADER.unpack_from(image, offset)
        if kind != PT_LOAD:
            continue
        load_count += 1
        require(memory_size > 0 and file_size <= memory_size, "invalid PT_LOAD size")
        require(
            file_offset <= len(image) and file_size <= len(image) - file_offset,
            "PT_LOAD bytes are outside the image",
        )
        require(
            virtual_address <= (1 << 64) - memory_size,
            "PT_LOAD virtual range overflows",
        )
        executable = flags & PF_X != 0
        if executable:
            executable_count += 1
            require(flags & PF_W == 0, "signed executable PT_LOAD is writable")
            require(
                file_size == memory_size,
                "signed executable PT_LOAD contains anonymous tail bytes",
            )
            require(
                file_offset % host_page_size
                == virtual_address % host_page_size,
                "signed executable PT_LOAD file/virtual offsets are incongruent",
            )
            require(
                align_up(file_offset + file_size, host_page_size) <= len(image),
                "signed executable host page extends past the carrier",
            )

        first_page = align_down(virtual_address, host_page_size)
        last_page = align_up(virtual_address + memory_size, host_page_size)
        page_kind = EXECUTABLE if executable else NONEXECUTABLE
        for page in range(first_page, last_page, host_page_size):
            prior_kind = page_kinds.get(page, 0)
            require(
                executable or prior_kind & EXECUTABLE == 0,
                "a non-executable PT_LOAD follows signed executable bytes in "
                f"the same host page at {page:#x}",
            )
            require(
                not executable or page not in writable_nonexecutable_pages,
                "a signed executable PT_LOAD shares a host page with writable "
                f"loader data at {page:#x}",
            )
            page_kinds[page] = page_kinds.get(page, 0) | page_kind
            if not executable and flags & PF_W != 0:
                writable_nonexecutable_pages.add(page)

    require(load_count > 0, "kernel image has no PT_LOAD segments")
    require(executable_count > 0, "kernel image has no executable PT_LOAD segments")
    return executable_count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--host-page-size", type=int, default=16384)
    arguments = parser.parse_args()
    try:
        executable_count = verify(
            arguments.image.read_bytes(),
            arguments.host_page_size,
        )
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(
        f"signed kernel carrier verified: {arguments.image} "
        f"({executable_count} executable segments)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
