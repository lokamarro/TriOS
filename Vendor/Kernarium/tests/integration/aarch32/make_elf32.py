#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Wrap a self-contained ARM ELF object into a static ELF32 executable.

The object has one allocated section, .text, a global _start, and no
relocation but the R_ARM_CALL of a bl to a symbol of that section, which is
resolved here.  The executable has one read-execute PT_LOAD at 0x10000,
aligned to 64 KiB as GNU ld lays out an armhf program, and a non-executable
PT_GNU_STACK.  It is an ARM EABI5 hard-float program, which is what the guest
kernel's AArch32 loader accepts.  No linker is involved.

--layout 4k instead places the segment at 0x11000, aligned to 4 KiB only, as
11 files of a stock Debian armhf image are: its file offset and address are
congruent modulo 4 KiB but not modulo 64 KiB.

usage: make_elf32.py [--layout 64k|4k] INPUT.o OUTPUT
"""

import struct
import sys

LAYOUTS = {
    # Segment address and alignment: congruent to its file offset modulo
    # 64 KiB, or only modulo 4 KiB.
    "64k": (0x10000, 0x10000),
    "4k": (0x11000, 0x1000),
}
EHDR_SIZE = 52
PHDR_SIZE = 32
EM_ARM = 40
EF_ARM_EABI_VER5 = 0x05000000
EF_ARM_ABI_FLOAT_HARD = 0x00000400
SHT_SYMTAB = 2
SHT_REL = 9
SHF_ALLOC = 2
R_ARM_CALL = 28
SHN_UNDEF = 0
PT_LOAD = 1
PT_GNU_STACK = 0x6474E551
PF_X, PF_W, PF_R = 1, 2, 4


class ObjectError(Exception):
    pass


def sections(data):
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ObjectError("not a little-endian ELF32 object")
    (e_type, e_machine, _, _, _, shoff, _, _, _, _, shentsize, shnum,
     shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if e_type != 1 or e_machine != EM_ARM:
        raise ObjectError("not an ARM relocatable object")
    result = []
    for index in range(shnum):
        (name, kind, flags, addr, offset, size, link, info, align,
         entsize) = struct.unpack_from("<IIIIIIIIII", data,
                                       shoff + index * shentsize)
        result.append((name, kind, flags, offset, size, link, info, align,
                       entsize))
    strings = result[shstrndx]

    def name_of(offset):
        start = strings[3] + offset
        return data[start:data.index(b"\0", start)].decode()

    return [(name_of(entry[0]),) + entry[1:] for entry in result]


def symbols(data, symtab, strtab):
    _, _, _, offset, size, _, _, _, entsize = symtab
    result = []
    for position in range(offset, offset + size, entsize):
        st_name, st_value, _, _, _, st_shndx = struct.unpack_from(
            "<IIIBBH", data, position)
        start = strtab[3] + st_name
        result.append((data[start:data.index(b"\0", start)].decode(),
                       st_value, st_shndx))
    return result


def resolve_calls(data, table, text_index, symtab, body):
    """Apply the R_ARM_CALL relocations of .text to the body."""
    body = bytearray(body)
    names = symbols(data, symtab[1], table[symtab[1][5]])
    for name, kind, flags, offset, size, link, info, align, entsize in table:
        if kind != SHT_REL or not size:
            continue
        if info != text_index or table[link] is not symtab[1]:
            raise ObjectError(f"{name}: unexpected relocation section")
        for position in range(offset, offset + size, 8):
            r_offset, r_info = struct.unpack_from("<II", data, position)
            if r_info & 0xff != R_ARM_CALL:
                raise ObjectError(f"{name}: relocation type {r_info & 0xff}")
            _, value, section = names[r_info >> 8]
            if section != text_index or value & 1:
                raise ObjectError(f"{name}: a call leaves the ARM .text")
            instruction, = struct.unpack_from("<I", body, r_offset)
            addend = (instruction & 0xffffff) << 2
            addend -= (addend & 0x2000000) << 1
            displacement = value + addend - r_offset
            if displacement & 3 or not -(1 << 25) <= displacement < (1 << 25):
                raise ObjectError(f"{name}: a call at {r_offset:#x} is out of range")
            struct.pack_into("<I", body, r_offset,
                             instruction & 0xff000000 |
                             (displacement >> 2) & 0xffffff)
    return bytes(body)


def wrap(data, layout="64k"):
    load_base, load_align = LAYOUTS[layout]
    table = sections(data)
    text = None
    symtab = None
    for index, entry in enumerate(table):
        name, kind, flags, offset, size, link, info, align, entsize = entry
        if kind == SHT_SYMTAB:
            symtab = (index, entry)
        if flags & SHF_ALLOC and size:
            if name != ".text" or text is not None:
                raise ObjectError(f"{name}: only a .text section may be allocated")
            text = (index, entry)
    if text is None or symtab is None:
        raise ObjectError("no .text or no symbol table")
    text_index, (_, _, _, text_offset, text_size, _, _, text_align, _) = text
    _, (_, _, _, sym_offset, sym_size, sym_link, _, _, sym_entsize) = symtab
    strtab = table[sym_link]
    entry_value = None
    for position in range(sym_offset, sym_offset + sym_size, sym_entsize):
        st_name, st_value, _, st_info, _, st_shndx = struct.unpack_from(
            "<IIIBBH", data, position)
        start = strtab[3] + st_name
        if data[start:data.index(b"\0", start)] == b"_start":
            if st_shndx != text_index:
                raise ObjectError("_start is not in .text")
            entry_value = st_value
    if entry_value is None:
        raise ObjectError("no _start")

    phnum = 2
    text_at = (EHDR_SIZE + phnum * PHDR_SIZE + 15) & ~15
    text_at = (text_at + max(text_align, 4) - 1) & ~(max(text_align, 4) - 1)
    body = resolve_calls(data, table, text_index, symtab,
                         data[text_offset:text_offset + text_size])
    size = text_at + len(body)
    header = struct.pack(
        "<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01" + b"\0" * 9,
        2, EM_ARM, 1, load_base + text_at + entry_value, EHDR_SIZE, 0,
        EF_ARM_EABI_VER5 | EF_ARM_ABI_FLOAT_HARD, EHDR_SIZE, PHDR_SIZE,
        phnum, 0, 0, 0)
    load = struct.pack("<IIIIIIII", PT_LOAD, 0, load_base, load_base, size,
                       size, PF_R | PF_X, load_align)
    stack = struct.pack("<IIIIIIII", PT_GNU_STACK, 0, 0, 0, 0, 0,
                        PF_R | PF_W, 16)
    image = header + load + stack
    image += b"\0" * (text_at - len(image)) + body
    return image


def main(arguments):
    layout = "64k"
    arguments = arguments[1:]
    if arguments[:1] == ["--layout"] and len(arguments) > 1:
        layout = arguments[1]
        arguments = arguments[2:]
    if len(arguments) != 2 or layout not in LAYOUTS:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        with open(arguments[0], "rb") as source:
            image = wrap(source.read(), layout)
    except (OSError, ObjectError) as error:
        print(f"make_elf32.py: {arguments[0]}: {error}", file=sys.stderr)
        return 1
    with open(arguments[1], "wb") as output:
        output.write(image)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
