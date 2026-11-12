# SPDX-License-Identifier: GPL-2.0-or-later
"""A reader of thin 64-bit little-endian Mach-O files for the tooling tests.

The tests that check linked images and objects share it. It reads the
segments, sections and symbol table, which is all they need.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

MH_MAGIC_64 = 0xFEEDFACF
MH_OBJECT = 0x1
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
N_STAB = 0xE0
HEADER_SIZE = 32


@dataclass(frozen=True)
class Section:
    address: int
    size: int
    offset: int
    alignment: int


class MachO:
    """One file's load commands; `name` prefixes its errors.

    filetype      the header's file type, such as MH_OBJECT
    segments      each segment's address, by name
    sections      each section, by (segment, section) name
    symbol_names  every non-debug symbol name in table order, None when the
                  file has no symbol table
    symbols       the value of each of those names, the first definition
                  winning
    """

    def __init__(self, data: bytes, name: str):
        if len(data) < HEADER_SIZE or \
                struct.unpack_from("<I", data)[0] != MH_MAGIC_64:
            raise ValueError(f"{name}: not a thin 64-bit Mach-O file")
        self.data = data
        self.name = name
        self.filetype, commands = struct.unpack_from("<II", data, 12)
        self.segments: dict[str, int] = {}
        self.sections: dict[tuple[str, str], Section] = {}
        self.symbol_names: list[str] | None = None
        self.symbols: dict[str, int] = {}
        offset = HEADER_SIZE
        for _ in range(commands):
            command, size = struct.unpack_from("<II", data, offset)
            if command == LC_SEGMENT_64:
                self._segment(offset)
            elif command == LC_SYMTAB:
                self._symbols(*struct.unpack_from("<IIII", data, offset + 8))
            offset += size

    def _text(self, offset: int) -> str:
        return self.data[offset:offset + 16].rstrip(b"\0").decode()

    def _segment(self, offset: int) -> None:
        segment = self._text(offset + 8)
        self.segments[segment] = struct.unpack_from(
            "<Q", self.data, offset + 24)[0]
        count = struct.unpack_from("<I", self.data, offset + 64)[0]
        for entry in range(offset + 72, offset + 72 + 80 * count, 80):
            self.sections[(self._text(entry + 16), self._text(entry))] = \
                Section(*struct.unpack_from("<QQII", self.data, entry + 32))

    def _symbols(self, symbol_offset: int, count: int, string_offset: int,
                 _string_size: int) -> None:
        self.symbol_names = []
        for index in range(count):
            string, kind, _, _, value = struct.unpack_from(
                "<IBBHQ", self.data, symbol_offset + 16 * index)
            if kind & N_STAB:
                continue
            end = self.data.index(b"\0", string_offset + string)
            name = self.data[string_offset + string:end].decode()
            self.symbol_names.append(name)
            self.symbols.setdefault(name, value)

    def section_bytes(self, segment: str, name: str) -> bytes | None:
        section = self.sections.get((segment, name))
        if section is None:
            return None
        return self.data[section.offset:section.offset + section.size]

    def words(self, section: Section, begin: int, end: int) -> tuple[int, ...]:
        """The 32-bit words at addresses begin up to end of a section."""
        start = section.offset + begin - section.address
        return struct.unpack_from(f"<{(end - begin) // 4}I", self.data, start)
