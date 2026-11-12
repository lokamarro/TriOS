#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Put the embedded kernel's function symbols into a dSYM.

An image that links the kernel ELF as its __TEXT,__uml_elf section reports a
guest-kernel frame at the address of the same instruction inside that section
(runtime/kernel_crash_frames), which a crash report attributes to the image. A
symbolicator then looks the section offset up in the image's dSYM. This tool
adds one symbol for every STT_FUNC entry of the ELF's own .symtab to the dSYM's
symbol table, at the address the linker gave the section plus the entry's file
offset. Only the dSYM changes: the linked image keeps the symbols it had.

A name that more than one function carries is kept for the lowest address, and
the others become name~2, name~3 and so on in address order, so every address
resolves to a distinct name. Every name takes the Mach-O leading underscore.
The tool is idempotent: a dSYM that already has the marker symbol is left as
it is.

usage: kernel-dsym-symbols.py --dsym DWARF-FILE --elf KERNEL-ELF
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import struct
import sys
import tempfile
from pathlib import Path

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
LC_SYMTAB = 0x2
LC_SEGMENT_64 = 0x19
# Commands that hold file offsets this tool does not move, or that a dSYM made
# by dsymutil does not have: refuse rather than corrupt them.
LC_WITH_OFFSETS = {0xB, 0x1D, 0x1E, 0x1F, 0x21, 0x22, 0x26, 0x29, 0x2B,
                   0x80000022, 0x80000033, 0x80000034}
N_SECT = 0x0E
SEGMENT_NAME = b"__TEXT"
SECTION_NAME = b"__uml_elf"
MARKER = "__uml_kernel_symbols"
PT_LOAD = 1
PF_X = 1
STT_FUNC = 2
SHT_SYMTAB = 2
HEADER = struct.Struct("<IiiIIIII")
COMMAND = struct.Struct("<II")
SYMTAB = struct.Struct("<IIIIII")
SEGMENT = struct.Struct("<II16sQQQQiiII")
SECTION = struct.Struct("<16s16sQQIIIIIIII")
NLIST = struct.Struct("<IBBHQ")


class SymbolError(Exception):
    pass


def elf_functions(elf: bytes) -> list[tuple[str, int, int]]:
    """(name, file offset, size) of each STT_FUNC entry inside executable
    PT_LOAD file data, by address."""
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        raise SymbolError("the kernel is not a little-endian ELF64 image")
    phoff, = struct.unpack_from("<Q", elf, 32)
    phentsize, phnum = struct.unpack_from("<HH", elf, 54)
    segments = []
    for index in range(phnum):
        kind, flags, offset, vaddr, _, filesz, _, _ = struct.unpack_from(
            "<IIQQQQQQ", elf, phoff + index * phentsize)
        if kind == PT_LOAD and flags & PF_X:
            segments.append((vaddr, offset, filesz))
    shoff, = struct.unpack_from("<Q", elf, 40)
    shentsize, shnum = struct.unpack_from("<HH", elf, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", elf, shoff + index * shentsize)
                for index in range(shnum)]
    tables = [section for section in sections if section[1] == SHT_SYMTAB]
    if len(tables) != 1:
        raise SymbolError("the kernel ELF has no single symbol table")
    table = tables[0]
    strings = sections[table[6]]
    functions = []
    for index in range(table[5] // 24):
        name, info, _, shndx, value, size = struct.unpack_from(
            "<IBBHQQ", elf, table[4] + index * 24)
        if info & 15 != STT_FUNC or shndx == 0:
            continue
        for vaddr, offset, filesz in segments:
            if vaddr <= value < vaddr + filesz:
                begin = strings[4] + name
                text = elf[begin:elf.index(b"\0", begin)].decode(
                    "utf-8", "replace")
                functions.append((text, offset + value - vaddr, size))
                break
    functions.sort(key=lambda function: function[1])
    return functions


def distinct_names(functions: list[tuple[str, int, int]]) -> list[str]:
    """Each function's name, a repeat of an earlier one numbered from ~2."""
    seen: dict[str, int] = {}
    names = []
    for name, _, _ in functions:
        seen[name] = seen.get(name, 0) + 1
        names.append(name if seen[name] == 1 else f"{name}~{seen[name]}")
    return names


def add_symbols(macho: bytes, elf: bytes) -> bytes | None:
    """The dSYM `macho` with the kernel's symbols, or None if it has them."""
    if len(macho) < HEADER.size:
        raise SymbolError("the dSYM is not a Mach-O file")
    (magic, cputype, _, _, ncmds, sizeofcmds, _, _) = HEADER.unpack_from(macho)
    if magic != MH_MAGIC_64 or cputype != CPU_TYPE_ARM64:
        raise SymbolError("the dSYM is not a thin arm64 Mach-O file")
    commands = []
    position = HEADER.size
    for _ in range(ncmds):
        command, size = COMMAND.unpack_from(macho, position)
        if command in LC_WITH_OFFSETS:
            raise SymbolError(f"unsupported load command {command:#x} in a dSYM")
        commands.append((command, position, size))
        position += size
    if position != HEADER.size + sizeofcmds:
        raise SymbolError("the dSYM's load commands are inconsistent")

    symtab = [entry for entry in commands if entry[0] == LC_SYMTAB]
    if len(symtab) != 1:
        raise SymbolError("the dSYM has no single symbol table")
    _, symtab_at, _ = symtab[0]
    (_, _, symoff, nsyms, stroff, strsize) = SYMTAB.unpack_from(macho, symtab_at)
    old_strings = macho[stroff:stroff + strsize]
    if stroff != symoff + nsyms * NLIST.size:
        raise SymbolError("the dSYM's string table does not follow its symbols")

    carrier = None
    linkedit = None
    ordinal = 0
    for command, at, _ in commands:
        if command != LC_SEGMENT_64:
            continue
        (_, _, segname, _, _, fileoff, filesize, _, _, nsects, _) = \
            SEGMENT.unpack_from(macho, at)
        segname = segname.rstrip(b"\0")
        if segname == b"__LINKEDIT":
            linkedit = (at, fileoff, filesize)
        for index in range(nsects):
            ordinal += 1
            (sectname, sectseg, addr, size, *_rest) = SECTION.unpack_from(
                macho, at + SEGMENT.size + index * SECTION.size)
            if (sectname.rstrip(b"\0"), sectseg.rstrip(b"\0")) == (
                    SECTION_NAME, SEGMENT_NAME):
                carrier = (ordinal, addr, size)
    if carrier is None:
        raise SymbolError("the dSYM has no __TEXT,__uml_elf section")
    if linkedit is None:
        raise SymbolError("the dSYM has no __LINKEDIT segment")
    ordinal, base, size = carrier
    if size != len(elf):
        raise SymbolError(
            f"the dSYM's __uml_elf section is {size} bytes; the ELF is {len(elf)}")
    link_at, link_off, link_size = linkedit
    if (link_off, link_size) != (symoff, nsyms * NLIST.size + strsize):
        raise SymbolError("the dSYM's __LINKEDIT holds more than its symbols")
    marker = b"_" + MARKER.encode() + b"\0"
    if marker in old_strings:
        return None

    functions = elf_functions(elf)
    if not functions:
        raise SymbolError("the kernel ELF has no function symbols")
    entries = bytearray()
    strings = bytearray(old_strings)
    for name, offset in [(MARKER, 0)] + [
            (text, function[1]) for text, function in
            zip(distinct_names(functions), functions)]:
        entries += NLIST.pack(len(strings), N_SECT, ordinal, 0, base + offset)
        strings += b"_" + name.encode("utf-8") + b"\0"

    new_symbols = nsyms + len(entries) // NLIST.size
    new_link_size = new_symbols * NLIST.size + len(strings)
    shift = (new_link_size - link_size + 15) & ~15
    old_end = link_off + link_size

    out = bytearray(macho[:old_end - link_size])
    out += macho[symoff:symoff + nsyms * NLIST.size] + entries + strings
    out += b"\0" * (shift - (new_link_size - link_size))
    out += macho[old_end:]
    struct.pack_into("<IIII", out, symtab_at + 8, symoff, new_symbols,
                     symoff + new_symbols * NLIST.size, len(strings))
    # A dSYM's virtual layout puts __LINKEDIT, then __DWARF, after the image's
    # own segments, so growing the one moves what follows it.
    (_, _, _, link_vmaddr, link_vmsize, *_rest) = SEGMENT.unpack_from(out, link_at)
    new_vmsize = max(link_vmsize, (new_link_size + 0xFFF) & ~0xFFF)
    vm_shift = new_vmsize - link_vmsize
    for command, at, _ in commands:
        if command != LC_SEGMENT_64:
            continue
        (_, _, _, vmaddr, _, fileoff, filesize, _, _, nsects, _) = \
            SEGMENT.unpack_from(out, at)
        if at == link_at:
            struct.pack_into("<QQQQ", out, at + 24, vmaddr, new_vmsize,
                             fileoff, new_link_size)
            continue
        after_in_vm = vmaddr >= link_vmaddr + link_vmsize
        if after_in_vm:
            struct.pack_into("<Q", out, at + 24, vmaddr + vm_shift)
        if filesize != 0 and fileoff >= old_end:
            struct.pack_into("<Q", out, at + 40, fileoff + shift)
        for index in range(nsects):
            section_at = at + SEGMENT.size + index * SECTION.size
            if after_in_vm:
                addr, = struct.unpack_from("<Q", out, section_at + 32)
                struct.pack_into("<Q", out, section_at + 32, addr + vm_shift)
            offset, = struct.unpack_from("<I", out, section_at + 48)
            reloff, nreloc = struct.unpack_from("<II", out, section_at + 56)
            if offset >= old_end:
                struct.pack_into("<I", out, section_at + 48, offset + shift)
            if nreloc != 0:
                struct.pack_into("<I", out, section_at + 56, reloff + shift)
    return bytes(out)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--dsym", type=Path, required=True,
                        help="the dSYM's Mach-O file (Contents/Resources/DWARF/<name>)")
    parser.add_argument("--elf", type=Path, required=True,
                        help="the kernel ELF the image links as __TEXT,__uml_elf")
    arguments = parser.parse_args(argv)
    try:
        updated = add_symbols(arguments.dsym.read_bytes(),
                              arguments.elf.read_bytes())
    except (OSError, SymbolError) as error:
        print(f"kernel-dsym-symbols.py: {error}", file=sys.stderr)
        return 1
    if updated is None:
        return 0
    with tempfile.NamedTemporaryFile(
            dir=arguments.dsym.parent, prefix=".kernel-symbols.",
            delete=False) as scratch:
        scratch.write(updated)
        scratch_path = Path(scratch.name)
    os.chmod(scratch_path, arguments.dsym.stat().st_mode & 0o7777)
    os.replace(scratch_path, arguments.dsym)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
