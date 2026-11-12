#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Find the SVE and SME words an AArch64 ELF executes at PSTATE.SM = 0 in
its SME code, and classify them against the SM=0 family.

The guest's SME profile has FEAT_SME without FEAT_SVE, so outside
streaming mode an SVE word is UNDEFINED, as on an Apple M4. The engines
make one deliberate exception, the SM=0 family: CNTB/H/W/D, RDVL, ADDVL,
ADDPL and the scalar INCB/H/W/D and DECB/H/W/D execute there with a
constant 128-bit vector length, and so do LDR and STR of Z and P registers
(base plus immediate, MUL VL), the callee-saved spills, which move 16 and 2
bytes. This module finds what real SME code executes outside streaming
mode, so that every other word is a decision rather than a guest SIGILL.

A function is SME code when it contains SMSTART or SMSTOP, an MRS or MSR
of SVCR or TPIDR2_EL0, or an SME word that uses ZA. Its bounds come from
.eh_frame, then sized symbols, then the nearest function start before it
(a symbol, an FDE, a BL target or a BTI C landing pad) and the next one
after it. Its entry mode is inferred:

- it reads SVCR or calls __arm_sme_state: streaming-compatible, SM 0 or 1;
- the first mode change on every path from the entry is SMSTART SM: SM 0
  (a non-streaming or locally streaming function);
- it is SMSTOP SM: SM 1 (a streaming function calling out);
- no mode change, but a word that needs streaming mode on every CPU
  before any: SM 1; otherwise SM 0 or 1.

The control-flow walk follows direct branches, takes calls as returning in
the same mode, seeds BTI J landing pads (or, without them, the code no
direct edge reaches) from an indirect jump, and tracks the set of possible
SM values per instruction. Every word reachable with SM 0 is classified
with the generated SVE/SME encoding table (sme_encoding_table.py):

- a64: not an SVE or SME word;
- family: an SM=0 family word;
- sme: an SME word the profile executes outside streaming mode (RDSVL,
  ADDSVL, ADDSPL, LDR/STR/ZERO ZA);
- sve: an SVE word that needs FEAT_SVE outside streaming mode;
- streaming-only: an SME word that needs streaming mode on every CPU;
- unadvertised: a word of a feature the profile lacks (SME2 and later).

The last three are SIGILL at SM 0 in the guest. The first two are
decisions (the SVE data processing, loads and stores of an SVE function
outside streaming mode are a limit of the SM=0 family, not an oversight); an
unadvertised word runs only past the code's own check for its feature, such
as compiler-rt's ZT0 save behind its FEAT_SME2 test, so it is listed for
review. A function whose only markers are ZA-like words its walk never
reaches, or that lie in a gap between known functions, is data (UBSan
function prefixes, literal pools) and is set aside.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import bisect
import importlib.util
import pathlib
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass, field

HERE = pathlib.Path(__file__).resolve().parent


def _load(name: str, filename: str):
    module = sys.modules.get(name)
    if module is None:
        spec = importlib.util.spec_from_file_location(name, HERE / filename)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return module


sme_table = _load("sme_encoding_table", "sme_encoding_table.py")

# The SM=0 family, by encoding-table name.
FAMILY = frozenset({
    "cntb_r_s_", "cnth_r_s_", "cntw_r_s_", "cntd_r_s_",
    "rdvl_r_i_", "addvl_r_ri_", "addpl_r_ri_",
    "incb_r_rs_", "inch_r_rs_", "incw_r_rs_", "incd_r_rs_",
    "decb_r_rs_", "dech_r_rs_", "decw_r_rs_", "decd_r_rs_",
    "ldr_z_bi_", "str_z_bi_", "ldr_p_bi_", "str_p_bi_",
})

# The features an SME word may need and the guest still has outside
# streaming mode (BF16 and I8MM follow the host; the SME profile is SME1).
ADVERTISED = frozenset({"sme", "sme_f64f64", "sme_i16i64", "bf16", "i8mm"})

# The classes that raise SIGILL at SM 0 in the guest whatever the code
# checks first: each is a decision. An unadvertised word raises SIGILL in
# either mode and runs only past the code's own check for its feature.
DECISION_CLASSES = ("sve", "streaming-only")
UNDEFINED_CLASSES = DECISION_CLASSES + ("unadvertised",)

EM_AARCH64 = 183
EM_NAMES = {3: "x86", 62: "x86-64", 40: "arm", 183: "aarch64"}

# Mode-changing and SME-state system instructions.
SMSTART_SM = frozenset({0xD503437F, 0xD503477F})   # SMSTART SM, SMSTART
SMSTOP_SM = frozenset({0xD503427F, 0xD503467F})    # SMSTOP SM, SMSTOP
SMSTART_ZA, SMSTOP_ZA = 0xD503457F, 0xD503447F
MSR_SVCR, MRS_SVCR = 0xD51B4240, 0xD53B4240        # | Rt
MSR_TPIDR2, MRS_TPIDR2 = 0xD51BD0A0, 0xD53BD0A0    # | Rt
BTI_C, BTI_J, BTI_JC = 0xD503245F, 0xD503249F, 0xD50324DF

_SMSTARTSTOP = re.compile(rb"\x7f[\x42-\x47]\x03\xd5")
_SYSREG = re.compile(rb"(?:\xd0|\x42)[\x1b\x3b]\xd5")
_SME_TOP = re.compile(rb"[\x80\x81\xa0\xa1\xc0\xc1\xe0\xe1]")
_BL_TOP = re.compile(rb"[\x94-\x97]")
_BTI_C = re.compile(rb"[\x5f\xdf]\x24\x03\xd5")
_PAC_SP = re.compile(rb"[\x3f\x7f]\x23\x03\xd5")
NOP = 0xD503201F
_MAPPING = re.compile(r"\$[xd](?:\.|$)")


class NotAArch64(ValueError):
    pass


def elf_machine(data: bytes) -> str | None:
    """The ELF machine of the bytes, or None when they are no ELF file."""
    if len(data) < 20 or data[:4] != b"\x7fELF":
        return None
    order = "<" if data[5] == 1 else ">"
    machine = struct.unpack_from(order + "H", data, 18)[0]
    name = EM_NAMES.get(machine, f"machine-{machine}")
    if machine == EM_AARCH64 and data[4] != 2:
        return "aarch64-ilp32"
    if machine == EM_AARCH64 and order == ">":
        return "aarch64-be"
    return name


def _sext(value: int, bits: int) -> int:
    return value - (1 << bits) if value >> (bits - 1) & 1 else value


def control(word: int, pc: int) -> tuple[str | None, int | None]:
    """The control-flow kind of a word and its direct target: b (always),
    cond, bl, blr, br (indirect jump), ret (or another return), stop
    (UDF, BRK, HLT), or None for straight-line code."""
    top = word & 0xFC000000
    if top == 0x14000000:
        return "b", pc + _sext(word & 0x3FFFFFF, 26) * 4
    if top == 0x94000000:
        return "bl", pc + _sext(word & 0x3FFFFFF, 26) * 4
    if word & 0xFF000000 == 0x54000000:
        target = pc + _sext(word >> 5 & 0x7FFFF, 19) * 4
        return ("b" if word & 0xE == 0xE else "cond"), target
    if word & 0x7E000000 == 0x34000000:
        return "cond", pc + _sext(word >> 5 & 0x7FFFF, 19) * 4
    if word & 0x7E000000 == 0x36000000:
        return "cond", pc + _sext(word >> 5 & 0x3FFF, 14) * 4
    if word & 0xFE000000 == 0xD6000000:
        opc = word >> 21 & 0xF
        if opc in (0, 8):
            return "br", None
        if opc in (1, 9):
            return "blr", None
        if opc in (2, 4, 5):
            return "ret", None
        return None, None
    if word & 0xFFFF0000 == 0:
        return "stop", None
    if word & 0xFFE0001F in (0xD4200000, 0xD4400000):
        return "stop", None
    return None, None


def sm_effect(word: int) -> str | None:
    """start, stop or unknown for an instruction that writes PSTATE.SM."""
    if word in SMSTART_SM:
        return "start"
    if word in SMSTOP_SM:
        return "stop"
    if word & 0xFFFFFFE0 == MSR_SVCR:
        return "unknown"
    return None


def marker(word: int, table) -> str | None:
    """The SME-code marker a word is, if any."""
    if word in SMSTART_SM or word in SMSTOP_SM or word in (SMSTART_ZA,
                                                          SMSTOP_ZA):
        return "smstart" if word & 0x100 else "smstop"
    masked = word & 0xFFFFFFE0
    if masked in (MSR_SVCR, MRS_SVCR):
        return "svcr"
    if masked in (MSR_TPIDR2, MRS_TPIDR2):
        return "tpidr2"
    if word >> 31 and (word >> 25) & 0xF == 0:
        row = table.row_for(word)
        if row is not None and row.za:
            return "za"
    return None


@dataclass(frozen=True)
class Classified:
    cls: str
    encoding: str
    note: str = ""


def classify(word: int, table) -> Classified:
    row = table.row_for(word)
    if row is None:
        return Classified("a64", "")
    note = _spill_note(word, row)
    if row.encoding in FAMILY:
        return Classified("family", row.encoding, note)
    if "sve" in row.non_streaming_requirement:
        if not row.requirement:
            note = note or "non-streaming-only form"
        return Classified("sve", row.encoding, note)
    if row.streaming:
        return Classified("streaming-only", row.encoding, note)
    if row.requirement and set(row.requirement) <= ADVERTISED:
        return Classified("sme", row.encoding, note)
    return Classified("unadvertised", row.encoding, note)


_SPILL_FORMS = {"str_z_bi_": "z", "ldr_z_bi_": "z", "str_p_bi_": "p",
                "ldr_p_bi_": "p"}


def _spill_note(word: int, row) -> str:
    """A callee-saved Z/P save or restore through SP or X29."""
    kind = _SPILL_FORMS.get(row.encoding)
    if kind is None:
        return ""
    base = word >> 5 & 0x1F
    register = word & 0x1F if kind == "z" else word & 0xF
    saved = 8 <= register <= 23 if kind == "z" else 4 <= register <= 15
    if base in (29, 31) and saved:
        return f"callee-saved {kind}{register} " + (
            "save" if row.encoding.startswith("str") else "restore")
    return ""


@dataclass
class Section:
    name: str
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    info: int
    entsize: int


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    kind: int


class Elf:
    """A little-endian ELF64 AArch64 file: its code, symbols, FDEs and PLT
    names. A relocatable object's allocated sections are laid out one after
    another from 0, so its symbols get distinct addresses."""

    def __init__(self, data: bytes):
        if elf_machine(data) != "aarch64":
            raise NotAArch64("not a little-endian ELF64 AArch64 file")
        self.data = data
        (self.type, _machine, _version, self.entry, self.phoff, self.shoff,
         _flags, _ehsize, self.phentsize, self.phnum, self.shentsize,
         self.shnum, self.shstrndx) = struct.unpack_from(
             "<HHIQQQIHHHHHH", data, 16)
        self.relocatable = self.type == 1
        self.sections = self._sections()
        self.code: list[tuple[int, int, bytes]] = []
        for section in self.sections:
            if section.type == 1 and section.flags & 0x4 and section.size:
                self.code.append((section.addr, section.addr + section.size,
                                  data[section.offset:section.offset +
                                       section.size]))
        if not self.code and not self.sections:
            self.code = self._segments()
        self.code.sort(key=lambda item: item[0])
        self._code_starts = [start for start, _end, _bytes in self.code]
        self.symbols = self._symbols()
        self.data_ranges = self._mapping_data()
        self.fdes = [] if self.relocatable else self._fdes()
        self.plt = {} if self.relocatable else self._plt()
        self.names: dict[int, str] = {}
        for symbol in self.symbols:
            if symbol.name and not _MAPPING.search(symbol.name):
                self.names.setdefault(symbol.value, symbol.name)
        for address, name in self.plt.items():
            self.names.setdefault(address, name + "@plt")
        self._sized = sorted((s.value, s.value + s.size) for s in
                             self.symbols if s.size and s.kind in (2, 10))
        self._fde_starts = [start for start, _end in self.fdes]
        self._starts: list[int] | None = None

    # ELF structure.

    def _sections(self) -> list[Section]:
        data = self.data
        if not self.shoff or not self.shnum:
            return []
        sections = []
        for index in range(self.shnum):
            base = self.shoff + index * self.shentsize
            if base + 64 > len(data):
                break
            (name, kind, flags, addr, offset, size, link, info, _align,
             entsize) = struct.unpack_from("<IIQQQQIIQQ", data, base)
            sections.append(Section("", kind, flags, addr, offset, size,
                                    link, info, entsize))
        if self.shstrndx < len(sections):
            names = sections[self.shstrndx]
            for index in range(len(sections)):
                base = self.shoff + index * self.shentsize
                offset = struct.unpack_from("<I", data, base)[0]
                sections[index].name = self._string(names.offset + offset)
        if self.relocatable:
            cursor = 0
            for index, section in enumerate(sections):
                if section.flags & 0x2:
                    align = struct.unpack_from(
                        "<Q", data, self.shoff + index * self.shentsize +
                        48)[0] or 1
                    align = max(align, 4)
                    cursor = (cursor + align - 1) // align * align
                    section.addr = cursor
                    cursor += section.size
        return sections

    def _segments(self) -> list[tuple[int, int, bytes]]:
        code = []
        for index in range(self.phnum):
            base = self.phoff + index * self.phentsize
            kind, flags, offset, vaddr, _paddr, filesz = struct.unpack_from(
                "<IIQQQQ", self.data, base)
            if kind == 1 and flags & 1 and filesz:
                code.append((vaddr, vaddr + filesz,
                             self.data[offset:offset + filesz]))
        return code

    def _string(self, offset: int) -> str:
        end = self.data.find(b"\0", offset)
        return self.data[offset:end if end >= 0 else None].decode(
            "utf-8", "replace")

    def _symbols(self) -> list[Symbol]:
        symbols = []
        for section in self.sections:
            if section.type not in (2, 11) or section.entsize != 24:
                continue
            strings = self.sections[section.link]
            for index in range(1, section.size // 24):
                base = section.offset + index * 24
                name, info, _other, shndx, value, size = struct.unpack_from(
                    "<IBBHQQ", self.data, base)
                kind = info & 0xF
                if kind not in (0, 2, 10) or shndx == 0 or shndx >= 0xFF00:
                    continue
                if shndx >= len(self.sections) or not \
                        self.sections[shndx].flags & 0x4:
                    continue
                if self.relocatable:
                    value += self.sections[shndx].addr
                text = self._string(strings.offset + name)
                if kind == 0 and not text:
                    continue
                symbols.append(Symbol(text, value, size, kind))
        return symbols

    def _mapping_data(self) -> list[tuple[int, int]]:
        """The [start, end) ranges the $d mapping symbols mark as data."""
        marks = sorted((s.value, s.name[:2]) for s in self.symbols
                       if s.name in ("$x", "$d") or s.name.startswith(
                           ("$x.", "$d.")))
        ranges = []
        for index, (value, kind) in enumerate(marks):
            if kind == "$d":
                end = marks[index + 1][0] if index + 1 < len(marks) else \
                    self._code_end(value)
                ranges.append((value, end))
        return ranges

    def _code_end(self, address: int) -> int:
        index = bisect.bisect_right(self._code_starts, address) - 1
        return self.code[index][1] if index >= 0 else address

    def section(self, name: str) -> Section | None:
        for section in self.sections:
            if section.name == name:
                return section
        return None

    @staticmethod
    def _uleb(data: bytes, pos: int) -> tuple[int, int]:
        result = shift = 0
        while True:
            byte = data[pos]
            pos += 1
            result |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                return result, pos

    @staticmethod
    def _sleb(data: bytes, pos: int) -> tuple[int, int]:
        result = shift = 0
        while True:
            byte = data[pos]
            pos += 1
            result |= (byte & 0x7F) << shift
            shift += 7
            if not byte & 0x80:
                if byte & 0x40:
                    result -= 1 << shift
                return result, pos

    def _encoded(self, data: bytes, pos: int, encoding: int,
                 address: int) -> tuple[int, int]:
        fmt = encoding & 0x0F
        if fmt == 0x01:
            value, end = self._uleb(data, pos)
        elif fmt == 0x09:
            value, end = self._sleb(data, pos)
        else:
            size, signed = {0x00: (8, False), 0x02: (2, False),
                            0x03: (4, False), 0x04: (8, False),
                            0x0A: (2, True), 0x0B: (4, True),
                            0x0C: (8, True)}[fmt]
            value = int.from_bytes(data[pos:pos + size], "little",
                                   signed=signed)
            end = pos + size
        if encoding & 0x70 == 0x10:
            value += address
        return value & 0xFFFFFFFFFFFFFFFF, end

    def _fdes(self) -> list[tuple[int, int]]:
        section = self.section(".eh_frame")
        if section is None or section.type == 8:
            return []
        data = self.data[section.offset:section.offset + section.size]
        cies: dict[int, int] = {}
        fdes = []
        pos = 0
        try:
            while pos + 4 <= len(data):
                length = struct.unpack_from("<I", data, pos)[0]
                start = pos
                pos += 4
                if length == 0:
                    break
                if length == 0xFFFFFFFF:
                    length = struct.unpack_from("<Q", data, pos)[0]
                    pos += 8
                end = pos + length
                id_pos = pos
                cie_id = struct.unpack_from("<I", data, pos)[0]
                pos += 4
                if cie_id == 0:
                    cies[start] = self._cie_encoding(data, pos)
                else:
                    cie = id_pos - cie_id
                    encoding = cies.get(cie)
                    if encoding is None:
                        cie_pos = cie + 4
                        if struct.unpack_from("<I", data, cie)[0] == \
                                0xFFFFFFFF:
                            cie_pos += 8
                        encoding = self._cie_encoding(data, cie_pos + 4)
                        cies[cie] = encoding
                    begin, after = self._encoded(
                        data, pos, encoding, section.addr + pos)
                    size, _ = self._encoded(data, after, encoding & 0x0F, 0)
                    if size:
                        fdes.append((begin, begin + size))
                pos = end
        except (KeyError, IndexError, struct.error):
            pass
        fdes.sort()
        return fdes

    def _cie_encoding(self, data: bytes, pos: int) -> int:
        version = data[pos]
        pos += 1
        end = data.index(b"\0", pos)
        augmentation = data[pos:end].decode("ascii", "replace")
        pos = end + 1
        if "eh" in augmentation:
            pos += 8
        _, pos = self._uleb(data, pos)
        _, pos = self._sleb(data, pos)
        if version == 1:
            pos += 1
        else:
            _, pos = self._uleb(data, pos)
        encoding = 0x00
        if augmentation.startswith("z"):
            _, pos = self._uleb(data, pos)
            for letter in augmentation[1:]:
                if letter == "R":
                    encoding = data[pos]
                    pos += 1
                elif letter == "P":
                    personality = data[pos]
                    _, pos = self._encoded(data, pos + 1, personality, 0)
                elif letter == "L":
                    pos += 1
        return encoding

    def _plt(self) -> dict[int, str]:
        """PLT stub address -> the symbol its GOT slot binds."""
        slots: dict[int, str] = {}
        for section in self.sections:
            if section.type != 4 or not section.name.startswith(".rela"):
                continue
            if section.link >= len(self.sections):
                continue
            symtab = self.sections[section.link]
            if symtab.type not in (2, 11):
                continue
            strings = self.sections[symtab.link]
            for index in range(section.size // 24):
                offset, info, _addend = struct.unpack_from(
                    "<QQq", self.data, section.offset + index * 24)
                if info & 0xFFFFFFFF != 1026:   # R_AARCH64_JUMP_SLOT
                    continue
                name_offset = struct.unpack_from(
                    "<I", self.data, symtab.offset + (info >> 32) * 24)[0]
                slots[offset] = self._string(strings.offset + name_offset)
        names: dict[int, str] = {}
        if not slots:
            return names
        for section in self.sections:
            if not section.name.startswith(".plt") or not \
                    section.flags & 0x4:
                continue
            code = self.data[section.offset:section.offset + section.size]
            for pos in range(0, len(code) - 7, 4):
                first, second = struct.unpack_from("<II", code, pos)
                if first & 0x9F00001F != 0x90000010 or \
                        second & 0xFFC003FF != 0xF9400211:
                    continue
                pc = section.addr + pos
                page = (pc & ~0xFFF) + (_sext(
                    (first >> 5 & 0x7FFFF) << 2 | first >> 29 & 3, 21) << 12)
                slot = (page + (second >> 10 & 0xFFF) * 8) & \
                    0xFFFFFFFFFFFFFFFF
                if slot in slots:
                    start = pc
                    if pos >= 4 and struct.unpack_from(
                            "<I", code, pos - 4)[0] == BTI_C:
                        start -= 4
                    names[start] = slots[slot]
        return names

    # Code access.

    def word(self, address: int) -> int | None:
        index = bisect.bisect_right(self._code_starts, address) - 1
        if index < 0:
            return None
        start, end, code = self.code[index]
        if address + 4 > end or address & 3:
            return None
        return struct.unpack_from("<I", code, address - start)[0]

    def in_data(self, address: int) -> bool:
        for start, end in self.data_ranges:
            if start <= address < end:
                return True
        return False

    def markers(self, table) -> list[tuple[int, str]]:
        """Every SME-code marker word, by address."""
        found = []
        for start, _end, code in self.code:
            base = (4 - start) & 3
            for pattern, offset in ((_SMSTARTSTOP, 0), (_SYSREG, 1)):
                for match in pattern.finditer(code):
                    pos = match.start() - offset
                    if pos < 0 or (pos - base) & 3:
                        continue
                    word = struct.unpack_from("<I", code, pos)[0]
                    kind = marker(word, table)
                    if kind is not None:
                        found.append((start + pos, kind))
            aligned = code[base:]
            for match in _SME_TOP.finditer(aligned[3::4]):
                pos = base + match.start() * 4
                word = struct.unpack_from("<I", code, pos)[0]
                if marker(word, table) == "za":
                    found.append((start + pos, "za"))
        return sorted(item for item in found
                      if not self.in_data(item[0]))

    def _function_starts(self) -> list[int]:
        """Every address something marks as a function start: symbols,
        FDEs, PLT stubs, BL targets and BTI C landing pads."""
        if self._starts is not None:
            return self._starts
        starts = {symbol.value for symbol in self.symbols
                  if symbol.kind in (2, 10)}
        starts.update(start for start, _end in self.fdes)
        starts.update(self.plt)
        for start, end, code in self.code:
            base = (4 - start) & 3
            aligned = code[base:]
            for match in _BL_TOP.finditer(aligned[3::4]):
                pos = base + match.start() * 4
                word = struct.unpack_from("<I", code, pos)[0]
                target = start + pos + _sext(word & 0x3FFFFFF, 26) * 4
                if self._code_contains(target):
                    starts.add(target)
            for match in _BTI_C.finditer(code):
                if not (match.start() - base) & 3:
                    starts.add(start + match.start())
            # PACIASP or PACIBSP lands an indirect call like BTI C, but a
            # shrink-wrapped prologue may sign mid-function: take it only
            # after a terminator or padding.
            for match in _PAC_SP.finditer(code):
                pos = match.start()
                if (pos - base) & 3:
                    continue
                if pos >= 4:
                    previous = struct.unpack_from("<I", code, pos - 4)[0]
                    kind, _target = control(previous, start + pos - 4)
                    if previous != NOP and kind not in ("b", "br", "ret",
                                                        "stop"):
                        continue
                starts.add(start + pos)
        self._starts = sorted(starts)
        return self._starts

    def _code_contains(self, address: int) -> bool:
        index = bisect.bisect_right(self._code_starts, address) - 1
        return index >= 0 and address < self.code[index][1]

    def is_function_start(self, address: int) -> bool:
        starts = self._function_starts()
        index = bisect.bisect_left(starts, address)
        return index < len(starts) and starts[index] == address

    def function_bounds(self, address: int) -> tuple[int, int, str]:
        """[start, end) of the function holding the address and the
        evidence for it: fde, symbol, heuristic (between function starts)
        or gap (after a known function's end, with no start before it)."""
        index = bisect.bisect_right(self._fde_starts, address) - 1
        best = None
        for candidate in range(index, max(index - 64, -1), -1):
            start, end = self.fdes[candidate]
            if start <= address < end and (
                    best is None or end - start < best[1] - best[0]):
                best = (start, end)
        if best is not None:
            return best[0], best[1], "fde"
        index = bisect.bisect_right(self._sized, (address, 1 << 64)) - 1
        while index >= 0 and address - self._sized[index][0] < 1 << 20:
            start, end = self._sized[index]
            if start <= address < end:
                return start, end, "symbol"
            index -= 1
        starts = self._function_starts()
        code_index = bisect.bisect_right(self._code_starts, address) - 1
        code_start, code_end, _code = self.code[code_index]
        # The code after a known function's end and before the next start
        # is a gap: padding, prefix data, or code with no evident start.
        floor = code_start
        index = bisect.bisect_right(self._fde_starts, address) - 1
        if index >= 0 and self.fdes[index][1] <= address:
            floor = max(floor, self.fdes[index][1])
        index = bisect.bisect_right(self._sized, (address, 1 << 64)) - 1
        if index >= 0 and self._sized[index][1] <= address:
            floor = max(floor, self._sized[index][1])
        index = bisect.bisect_right(starts, address) - 1
        if index >= 0 and starts[index] >= floor:
            start, bounds = starts[index], "heuristic"
        else:
            start, bounds = floor, "gap"
        after = bisect.bisect_right(starts, address)
        end = starts[after] if after < len(starts) else code_end
        return start, min(end, code_end), bounds

    def name_of(self, address: int) -> str:
        name = self.names.get(address)
        return name if name else f"fn@{address:#x}"


@dataclass
class Instruction:
    address: int
    word: int
    states: frozenset
    classified: Classified


@dataclass
class Function:
    name: str
    start: int
    end: int
    bounds: str
    kind: str
    entry: frozenset
    markers: list[tuple[int, str]]
    calls: list[str]
    sm0: list[Instruction] = field(default_factory=list)
    unresolved_jump: bool = False
    reached_markers: int = 0

    def findings(self) -> list[Instruction]:
        return [item for item in self.sm0
                if item.classified.cls in UNDEFINED_CLASSES]


ENTRY_STATES = {
    "streaming-compatible": frozenset({0, 1}),
    "non-streaming": frozenset({0}),
    "streaming": frozenset({1}),
    "either": frozenset({0, 1}),
}


class Analyzer:
    """The SM=0 walk over the SME functions of one ELF file."""

    def __init__(self, elf: Elf, table):
        self.elf = elf
        self.table = table
        self._classified: dict[int, Classified] = {}
        self.data_like: list[Function] = []

    def classify(self, word: int) -> Classified:
        result = self._classified.get(word)
        if result is None:
            result = classify(word, self.table)
            self._classified[word] = result
        return result

    def functions(self, declared: dict[str, str] | None = None
                  ) -> list[Function]:
        """Every SME function, analysed. declared maps a function name to a
        kind of ENTRY_STATES that overrides the inferred one."""
        by_start: dict[int, list] = {}
        for address, kind in self.elf.markers(self.table):
            start, end, bounds = self.elf.function_bounds(address)
            entry = by_start.setdefault(start, [end, bounds, []])
            entry[0] = max(entry[0], end)
            entry[2].append((address, kind))
        result = []
        self.data_like = []
        for start, (end, bounds, markers) in sorted(by_start.items()):
            function = self.analyse(start, end, bounds, markers, declared)
            if all(kind == "za" for _address, kind in markers) and (
                    function.reached_markers == 0 or bounds == "gap"):
                # SME-like data: no function executes these words.
                self.data_like.append(function)
                continue
            result.append(function)
        return result

    def _contains(self, function_parts, address: int) -> bool:
        for start, end in function_parts:
            if start <= address < end:
                return True
        return False

    def _successors(self, word: int, address: int, parts,
                    jump_seeds: list[int]) -> tuple[list[int], bool]:
        """The successors inside the function and whether the word is an
        indirect jump."""
        kind, target = control(word, address)
        if kind is None or kind in ("bl", "blr"):
            return [address + 4], False
        if kind in ("ret", "stop"):
            return [], False
        if kind == "br":
            return list(jump_seeds), True
        out = [] if kind == "b" else [address + 4]
        if target is not None:
            if self._contains(parts, target):
                out.append(target)
            elif not self.elf.is_function_start(target) and \
                    self.elf.word(target) is not None and \
                    not self.elf.relocatable:
                # A branch to code no function starts at: a cold part.
                start, end, _bounds = self.elf.function_bounds(target)
                if start <= target < end and (end - start) < 1 << 20:
                    parts.append((start, end))
                    out.append(target)
        return out, False

    def _infer(self, start: int, parts, jump_seeds, calls) -> str:
        if any(name.startswith("__arm_sme_state") for name in calls):
            return "streaming-compatible"
        for part_start, part_end in parts:
            for address in range(part_start, part_end, 4):
                word = self.elf.word(address)
                if word is not None and word & 0xFFFFFFE0 == MRS_SVCR:
                    return "streaming-compatible"
        firsts = set()
        needs_streaming = False
        seen = set()
        stack = [start]
        while stack:
            address = stack.pop()
            if address in seen or not self._contains(parts, address):
                continue
            seen.add(address)
            word = self.elf.word(address)
            if word is None:
                continue
            effect = sm_effect(word)
            if effect is not None:
                firsts.add(effect)
                continue
            if self.classify(word).cls == "streaming-only":
                needs_streaming = True
            successors, _ = self._successors(word, address, parts,
                                             jump_seeds)
            stack.extend(successors)
        if firsts == {"start"}:
            return "non-streaming"
        if firsts == {"stop"} and self._enters_streaming(parts):
            # A streaming function leaves streaming mode around a call and
            # enters it again; a lone SMSTOP (before a fatal error, say)
            # says nothing about the entry mode.
            return "streaming"
        if not firsts and needs_streaming:
            return "streaming"
        return "either"

    def _enters_streaming(self, parts) -> bool:
        for part_start, part_end in parts:
            for address in range(part_start, part_end, 4):
                if self.elf.word(address) in SMSTART_SM:
                    return True
        return False

    def analyse(self, start: int, end: int, bounds: str,
                markers: list[tuple[int, str]],
                declared: dict[str, str] | None = None) -> Function:
        elf = self.elf
        parts = [(start, end)]
        bti_j = [address for address in range(start, end, 4)
                 if elf.word(address) in (BTI_J, BTI_JC)]
        calls = []
        for address in range(start, end, 4):
            word = elf.word(address)
            if word is not None and word & 0xFC000000 == 0x94000000:
                target = address + _sext(word & 0x3FFFFFF, 26) * 4
                calls.append(elf.names.get(target, f"{target:#x}"))
        name = elf.name_of(start)
        kind = (declared or {}).get(name) or self._infer(start, parts,
                                                         bti_j, calls)
        entry = ENTRY_STATES[kind]
        function = Function(name, start, end, bounds, kind, entry, markers,
                            sorted(set(calls)))
        states: dict[int, frozenset] = {}
        jump_states: set = set()
        work = [(start, entry)]

        def run():
            while work:
                address, state = work.pop()
                if not self._contains(parts, address):
                    continue
                old = states.get(address, frozenset())
                new = old | state
                if new == old:
                    continue
                states[address] = new
                word = elf.word(address)
                if word is None:
                    continue
                effect = sm_effect(word)
                after = new
                if effect == "start":
                    after = frozenset({1})
                elif effect == "stop":
                    after = frozenset({0})
                elif effect == "unknown":
                    after = frozenset({0, 1})
                successors, indirect = self._successors(word, address,
                                                        parts, bti_j)
                if indirect:
                    jump_states.update(new)
                for successor in successors:
                    work.append((successor, after))

        run()
        if jump_states and not bti_j:
            # No landing pads name the jump targets: seed the code no
            # direct edge reaches with the states of the indirect jumps.
            function.unresolved_jump = True
            seed = frozenset(jump_states)
            for part_start, part_end in list(parts):
                for address in range(part_start, part_end, 4):
                    if address not in states:
                        work.append((address, seed))
            run()
        function.reached_markers = sum(1 for address, _kind in markers
                                       if address in states)
        for address in sorted(states):
            if 0 not in states[address]:
                continue
            word = elf.word(address)
            if word is None or elf.in_data(address):
                continue
            classified = self.classify(word)
            if classified.cls != "a64":
                function.sm0.append(Instruction(address, word,
                                                states[address],
                                                classified))
        return function


class IndexedTable:
    """The encoding table with its rows bucketed by top byte, keeping the
    table's first-match order within a bucket."""

    def __init__(self, table):
        self.table = table
        self.rows = table.rows
        self._buckets = [[row for row in table.rows
                          if (top << 24) & row.mask == row.value & 0xFF000000
                          & row.mask] for top in range(256)]

    def row_for(self, word: int):
        for row in self._buckets[word >> 24]:
            if word & row.mask == row.value:
                return row
        return None


def disassemble(words: set[int], llvm_mc: str | None) -> dict[int, str]:
    """Instruction text for each word, through LLVM MC when available."""
    if not words or not llvm_mc:
        return {}
    ordered = sorted(words)
    text = "".join(",".join(f"0x{word >> shift & 0xFF:02x}"
                            for shift in (0, 8, 16, 24)) + "\n"
                   for word in ordered)
    try:
        result = subprocess.run(
            [llvm_mc, "--disassemble", "-triple=aarch64",
             "-mattr=+sve2,+sme2,+sme-f64f64,+sme-i16i64,+bf16,+i8mm,"
             "+sve2p1"],
            input=text, capture_output=True, text=True, timeout=600)
    except OSError:
        return {}
    lines = [line.strip() for line in result.stdout.splitlines()
             if line.strip() and not line.strip().startswith(".")]
    invalid = sum(1 for line in result.stderr.splitlines()
                  if "invalid instruction encoding" in line)
    if invalid or len(lines) != len(ordered):
        return {}
    return {word: " ".join(line.split("//")[0].split())
            for word, line in zip(ordered, lines)}


def find_llvm_mc(explicit: str | None) -> str | None:
    """LLVM MC for instruction text: the given one, else llvm-mc or a
    versioned llvm-mc-N on PATH."""
    if explicit:
        return explicit
    for name in ["llvm-mc"] + [f"llvm-mc-{major}" for major in
                               range(30, 16, -1)]:
        found = shutil.which(name)
        if found:
            return found
    return None


def load_table(path: pathlib.Path | None = None) -> IndexedTable:
    return IndexedTable(sme_table.load(path or sme_table.TABLE))
