#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Link freestanding ARM objects into a static ELF32 executable.

The AArch32 engine probes are C++ compiled with the configured compiler's ARM
backend, and the toolchain has no ARM linker, so this does the one job a
linker has for such a program: it lays the object's sections out and applies
its relocations. make_elf32.py does the same for the single-section assembly
program of the kernel probe; this handles what a compiler emits.

Each object may allocate .text, .rodata, .data and .bss sections (any
.text.*, .rodata.* and so on), and nothing else but sections it discards
(.ARM.exidx, .ARM.extab). They are laid out in one read-execute PT_LOAD at
0x10000 (code, then read-only data), aligned to 64 KiB as GNU ld lays out an
armhf program, and, when the object has writable sections, one read-write
PT_LOAD after it, then a non-executable PT_GNU_STACK. The entry point is
_start. Every symbol must be defined, and a relocation must be one
this knows; it fails otherwise, naming it. Calls between ARM and Thumb code
are interworking BL/BLX; a branch, as opposed to a call, may not change state
since that would need a veneer. No range check is silent: every displacement is
range checked. A global or weak symbol an object leaves undefined is resolved
against the other objects' global definitions, and a local symbol never leaves
its object; a global defined twice is an error.

--layout 4k places the first segment at 0x11000, aligned to 4 KiB only, as
make_elf32.py does.

usage: link_elf32.py [--layout 64k|4k] OUTPUT INPUT.o [INPUT.o ...]
"""

import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_elf32  # noqa: E402

LAYOUTS = make_elf32.LAYOUTS
EHDR_SIZE = make_elf32.EHDR_SIZE
PHDR_SIZE = make_elf32.PHDR_SIZE
EM_ARM = make_elf32.EM_ARM

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_NOBITS = 8
SHT_REL = 9
SHF_WRITE = 1
SHF_ALLOC = 2
SHF_EXECINSTR = 4
SHN_UNDEF = 0
SHN_ABS = 0xfff1
SHN_COMMON = 0xfff2
STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_SECTION = 0, 1, 2, 3
PT_LOAD = 1
PT_GNU_STACK = 0x6474E551
PF_X, PF_W, PF_R = 1, 2, 4

# Relocation types (ELF for the Arm Architecture, IHI 0044).
R_ARM_NONE = 0
R_ARM_PC24 = 1
R_ARM_ABS32 = 2
R_ARM_REL32 = 3
R_ARM_THM_CALL = 10
R_ARM_CALL = 28
R_ARM_JUMP24 = 29
R_ARM_MOVW_ABS_NC = 43
R_ARM_MOVT_ABS = 44
R_ARM_THM_JUMP24 = 30
R_ARM_THM_MOVW_ABS_NC = 47
R_ARM_THM_MOVT_ABS = 48

# Sections the linker drops: unwind tables, which the probes never use.
DISCARDED = (".ARM.exidx", ".ARM.extab")


class LinkError(Exception):
    pass


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def signed(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value >> (bits - 1) else value


def read_object(data):
    """Section headers, as make_elf32.sections() reads them, and the symbols."""
    table = make_elf32.sections(data)
    symtab = None
    for index, entry in enumerate(table):
        if entry[1] == SHT_SYMTAB:
            symtab = index
    if symtab is None:
        raise LinkError("no symbol table")
    _, _, _, sym_offset, sym_size, sym_link, _, _, sym_entsize = table[symtab]
    strtab = table[sym_link]
    symbols = []
    for position in range(sym_offset, sym_offset + sym_size, sym_entsize):
        st_name, st_value, st_size, st_info, _, st_shndx = struct.unpack_from(
            "<IIIBBH", data, position)
        start = strtab[3] + st_name
        name = data[start:data.index(b"\0", start)].decode()
        symbols.append((name, st_value, st_info & 15, st_info >> 4, st_shndx))
    return table, symbols


def classify(name):
    for prefix, kind in ((".text", "text"), (".rodata", "rodata"),
                         (".data", "data"), (".bss", "bss")):
        if name == prefix or name.startswith(prefix + "."):
            return kind
    return None


def layout_sections(tables, load_base, load_align):
    """Assign an address to every allocated section of every object: returns
    (addresses, segments, phnum), where addresses is keyed (object, section)
    and segments are (flags, vaddr, file_offset, filesz, memsz, [keys])."""
    kinds = {}
    for object_index, table in enumerate(tables):
        for index, (name, kind, flags, *_rest) in enumerate(table):
            if not flags & SHF_ALLOC:
                continue
            if name.startswith(DISCARDED):
                continue
            category = classify(name)
            if category is None:
                raise LinkError(f"{name}: an allocated section this does not place")
            size = table[index][4]
            if size == 0 and kind != SHT_NOBITS:
                continue
            if flags & 0x400:
                raise LinkError(f"{name}: thread-local storage")
            writable = bool(flags & SHF_WRITE)
            if category in ("text", "rodata") and writable:
                raise LinkError(f"{name}: writable code or read-only data")
            if category in ("data", "bss") and not writable:
                raise LinkError(f"{name}: read-only {category}")
            if category == "text" and not flags & SHF_EXECINSTR:
                raise LinkError(f"{name}: a text section that is not executable")
            kinds[(object_index, index)] = category

    def section(key):
        return tables[key[0]][key[1]]

    rx = [key for key in kinds if kinds[key] in ("text", "rodata")]
    rx.sort(key=lambda key: ({"text": 0, "rodata": 1}[kinds[key]], key))
    rw = [key for key in kinds if kinds[key] in ("data", "bss")]
    rw.sort(key=lambda key: ({"data": 0, "bss": 1}[kinds[key]], key))

    phnum = 2 + (1 if rw else 0)
    addresses = {}
    cursor = align_up(EHDR_SIZE + phnum * PHDR_SIZE, 16)
    for key in rx:
        cursor = align_up(cursor, max(section(key)[7], 1))
        addresses[key] = load_base + cursor
        cursor += section(key)[4]
    segments = [(PF_R | PF_X, load_base, 0, cursor, cursor, rx)]
    if rw:
        file_offset = align_up(cursor, 16)
        file_end = file_offset
        # Congruent to its file offset modulo the segment alignment.
        vaddr = align_up(load_base + cursor, load_align) + (file_offset % load_align)
        position = vaddr
        for key in rw:
            position = align_up(position, max(section(key)[7], 1))
            addresses[key] = position
            if section(key)[1] != SHT_NOBITS:
                file_end = file_offset + (position - vaddr) + section(key)[4]
            position += section(key)[4]
        segments.append((PF_R | PF_W, vaddr, file_offset,
                         file_end - file_offset, position - vaddr, rw))
    return addresses, segments, phnum


def symbol_value(symbol, object_index, addresses, global_symbols):
    """(address, is_thumb) of a symbol of object `object_index`."""
    name, value, kind, bind, shndx = symbol
    if shndx == SHN_UNDEF:
        if name == "" and value == 0:
            return 0, False
        if name not in global_symbols:
            raise LinkError(f"{name}: undefined")
        object_index, symbol = global_symbols[name]
        name, value, kind, bind, shndx = symbol
    if shndx == SHN_COMMON:
        raise LinkError(f"{name}: a common symbol")
    if shndx == SHN_ABS:
        return value, False
    key = (object_index, shndx)
    if key not in addresses:
        raise LinkError(f"{name}: defined in a section that is not placed")
    thumb = kind == STT_FUNC and bool(value & 1)
    if kind == STT_SECTION:
        return addresses[key], False
    return addresses[key] + (value & ~1 if thumb else value), thumb


def check_range(value, bits, what, position):
    if not -(1 << (bits - 1)) <= value < (1 << (bits - 1)):
        raise LinkError(f"{what} at {position:#x} is out of range ({value:#x})")


def arm_branch(word, relocation, displacement, thumb_target, where):
    """BL/B/BLX imm24 of an ARM instruction word."""
    condition = word >> 28
    link_exchange = condition == 0xf
    if relocation == R_ARM_JUMP24 or relocation == R_ARM_PC24:
        if thumb_target:
            raise LinkError(f"{where}: a branch (not a call) from ARM to Thumb "
                            "code needs a veneer")
        if link_exchange:
            raise LinkError(f"{where}: BLX relocated as a jump")
    if thumb_target:
        if condition not in (0xe, 0xf):
            raise LinkError(f"{where}: a conditional call to Thumb code")
        # BLX (immediate): the halfword bit of the offset is H.
        check_range(displacement, 26, "BLX", where)
        if displacement & 1:
            raise LinkError(f"{where}: an odd Thumb target")
        return (0xfa000000 | (displacement >> 1 & 1) << 24 |
                (displacement >> 2) & 0xffffff)
    if displacement & 3:
        raise LinkError(f"{where}: an unaligned ARM target")
    check_range(displacement, 26, "branch", where)
    if link_exchange:
        # BLX (immediate) to ARM code: becomes BL.
        word = 0xeb000000
    return (word & 0xff000000) | (displacement >> 2) & 0xffffff


def decode_thumb_branch(hw1, hw2):
    sign = hw1 >> 10 & 1
    imm10 = hw1 & 0x3ff
    j1 = hw2 >> 13 & 1
    j2 = hw2 >> 11 & 1
    imm11 = hw2 & 0x7ff
    i1 = 1 - (j1 ^ sign)
    i2 = 1 - (j2 ^ sign)
    return signed(sign << 24 | i1 << 23 | i2 << 22 | imm10 << 12 | imm11 << 1, 25)


def encode_thumb_branch(hw1, hw2, offset, link, exchange_to_arm):
    sign = offset >> 24 & 1
    i1 = offset >> 23 & 1
    i2 = offset >> 22 & 1
    j1 = (1 - i1) ^ sign
    j2 = (1 - i2) ^ sign
    imm10 = offset >> 12 & 0x3ff
    imm11 = offset >> 1 & 0x7ff
    if exchange_to_arm:
        if imm11 & 1:
            raise LinkError("a BLX target that is not word aligned")
        hw2_base = 0xc000
    elif link:
        hw2_base = 0xd000
    else:
        hw2_base = 0x9000
    return ((hw1 & 0xf800) | sign << 10 | imm10,
            hw2_base | j1 << 13 | j2 << 11 | imm11)


def apply_relocation(body, offset, relocation, symbol_address, thumb_target,
                     place, where):
    """Relocate the field at body[offset:]; `place` is its address."""
    if relocation == R_ARM_NONE:
        return
    if relocation == R_ARM_ABS32:
        addend = struct.unpack_from("<I", body, offset)[0]
        value = (symbol_address + addend) | (1 if thumb_target else 0)
        struct.pack_into("<I", body, offset, value & 0xffffffff)
    elif relocation == R_ARM_REL32:
        addend = struct.unpack_from("<I", body, offset)[0]
        value = ((symbol_address + addend) | (1 if thumb_target else 0)) - place
        struct.pack_into("<I", body, offset, value & 0xffffffff)
    elif relocation in (R_ARM_CALL, R_ARM_JUMP24, R_ARM_PC24):
        word = struct.unpack_from("<I", body, offset)[0]
        addend = signed((word & 0xffffff) << 2, 26)
        if word >> 28 == 0xf:
            addend += (word >> 24 & 1) << 1
        displacement = symbol_address + addend - place
        struct.pack_into("<I", body, offset, arm_branch(
            word, relocation, displacement, thumb_target, where))
    elif relocation in (R_ARM_THM_CALL, R_ARM_THM_JUMP24):
        hw1, hw2 = struct.unpack_from("<HH", body, offset)
        addend = decode_thumb_branch(hw1, hw2)
        displacement = symbol_address + addend - place
        is_call = relocation == R_ARM_THM_CALL
        if not thumb_target:
            if not is_call:
                raise LinkError(f"{where}: a branch (not a call) from Thumb "
                                "to ARM code needs a veneer")
            # BLX: the offset is from the word-aligned PC, whose base is 2
            # bytes later when the instruction is halfword-aligned only.
            displacement += place & 2
        check_range(displacement, 25, "Thumb branch", where)
        new1, new2 = encode_thumb_branch(
            hw1, hw2, displacement, is_call, not thumb_target)
        struct.pack_into("<HH", body, offset, new1, new2)
    elif relocation in (R_ARM_MOVW_ABS_NC, R_ARM_MOVT_ABS):
        word = struct.unpack_from("<I", body, offset)[0]
        addend = signed((word >> 4 & 0xf000) | (word & 0xfff), 16)
        value = symbol_address + addend
        if relocation == R_ARM_MOVW_ABS_NC:
            value = (value | (1 if thumb_target else 0)) & 0xffff
        else:
            value = value >> 16 & 0xffff
        word = (word & 0xfff0f000) | (value >> 12 & 0xf) << 16 | (value & 0xfff)
        struct.pack_into("<I", body, offset, word)
    elif relocation in (R_ARM_THM_MOVW_ABS_NC, R_ARM_THM_MOVT_ABS):
        hw1, hw2 = struct.unpack_from("<HH", body, offset)
        field = ((hw1 & 0xf) << 12 | (hw1 >> 10 & 1) << 11 |
                 (hw2 >> 12 & 7) << 8 | (hw2 & 0xff))
        value = symbol_address + signed(field, 16)
        if relocation == R_ARM_THM_MOVW_ABS_NC:
            value = (value | (1 if thumb_target else 0)) & 0xffff
        else:
            value = value >> 16 & 0xffff
        hw1 = (hw1 & 0xfbf0) | (value >> 12 & 0xf) | (value >> 11 & 1) << 10
        hw2 = (hw2 & 0x8f00) | (value >> 8 & 7) << 12 | (value & 0xff)
        struct.pack_into("<HH", body, offset, hw1, hw2)
    else:
        raise LinkError(f"{where}: relocation type {relocation}")


def link(blobs, layout="64k"):
    load_base, load_align = LAYOUTS[layout]
    objects = [read_object(data) for data in blobs]
    tables = [table for table, _ in objects]
    addresses, segments, phnum = layout_sections(tables, load_base, load_align)

    # Global and weak definitions, by name; a strong one beats a weak one.
    global_symbols = {}
    weak = set()
    for object_index, (_, symbols) in enumerate(objects):
        for symbol in symbols:
            name, _, _, bind, shndx = symbol
            if bind not in (1, 2) or shndx == SHN_UNDEF or not name:
                continue
            if name in global_symbols and name not in weak and bind == 1:
                raise LinkError(f"{name}: defined twice")
            if name in global_symbols and bind == 2:
                continue
            global_symbols[name] = (object_index, symbol)
            if bind == 2:
                weak.add(name)
            else:
                weak.discard(name)

    # Section contents, relocated in place.
    bodies = {}
    for (object_index, index) in addresses:
        _, kind, _, offset, size, *_ = tables[object_index][index]
        data = blobs[object_index]
        bodies[(object_index, index)] = (
            bytearray(size) if kind == SHT_NOBITS else
            bytearray(data[offset:offset + size]))
    for object_index, (table, symbols) in enumerate(objects):
        data = blobs[object_index]
        for index, (name, kind, _, offset, size, link_index, info, *_) in enumerate(table):
            if kind != SHT_REL or not size or (object_index, info) not in addresses:
                continue
            for position in range(offset, offset + size, 8):
                r_offset, r_info = struct.unpack_from("<II", data, position)
                symbol = symbols[r_info >> 8]
                address, thumb = symbol_value(symbol, object_index, addresses,
                                              global_symbols)
                where = (f"object {object_index} {table[info][0]}+{r_offset:#x} "
                         f"({symbol[0] or 'section'})")
                key = (object_index, info)
                apply_relocation(bodies[key], r_offset, r_info & 0xff, address,
                                 thumb, addresses[key] + r_offset, where)

    if "_start" not in global_symbols:
        raise LinkError("no _start")
    object_index, symbol = global_symbols["_start"]
    address, thumb = symbol_value(symbol, object_index, addresses, global_symbols)
    # A Thumb entry has bit 0 set, which the kernel's start_thread() turns
    # into PSTATE.T.
    entry = address | (1 if thumb else 0)

    image = bytearray()
    headers = [
        struct.pack("<IIIIIIII", PT_LOAD, offset, vaddr, vaddr, filesz, memsz,
                    flags, load_align)
        for flags, vaddr, offset, filesz, memsz, _ in segments]
    headers.append(struct.pack("<IIIIIIII", PT_GNU_STACK, 0, 0, 0, 0, 0,
                               PF_R | PF_W, 16))
    image += struct.pack(
        "<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01" + b"\0" * 9,
        2, EM_ARM, 1, entry, EHDR_SIZE, 0,
        make_elf32.EF_ARM_EABI_VER5 | make_elf32.EF_ARM_ABI_FLOAT_HARD,
        EHDR_SIZE, PHDR_SIZE, phnum, 0, 0, 0)
    for header in headers:
        image += header
    for flags, vaddr, offset, filesz, memsz, members in segments:
        image += b"\0" * (offset - len(image))
        for key in members:
            if tables[key[0]][key[1]][1] == SHT_NOBITS:
                continue
            position = offset + addresses[key] - vaddr
            image += b"\0" * (position - len(image))
            image += bodies[key]
    return bytes(image)


def main(arguments):
    layout = "64k"
    arguments = arguments[1:]
    if arguments[:1] == ["--layout"] and len(arguments) > 1:
        layout = arguments[1]
        arguments = arguments[2:]
    if len(arguments) < 2 or layout not in LAYOUTS:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        blobs = []
        for name in arguments[1:]:
            with open(name, "rb") as source:
                blobs.append(source.read())
        image = link(blobs, layout)
    except (OSError, make_elf32.ObjectError, LinkError) as error:
        print(f"link_elf32.py: {arguments[0]}: {error}", file=sys.stderr)
        return 1
    with open(arguments[0], "wb") as output:
        output.write(image)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
