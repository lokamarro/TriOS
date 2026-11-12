#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author the SM = 0 family cases of the A64 instruction corpus.

usage: sm0-vl-corpus.py DUMP OUTPUT

The virtual CPU has FEAT_SME on an SME host and never FEAT_SVE, and it
executes, outside streaming mode, CNTB, CNTH, CNTW and CNTD, RDVL, ADDVL, ADDPL, and the scalar INCB,
INCH, INCW, INCD, DECB, DECH, DECW and DECD, every predicate pattern and
multiplier, for one constant vector length of 128 bits (CNTB = 16, CNTD = 2);
and LDR and STR of a vector register Z (16 bytes) and of a predicate register
P (2 bytes) with a scalar base and an immediate offset (MUL VL). The
architecture makes every one of them UNDEFINED at PSTATE.SM = 0 on such a CPU,
and an M4 traps them, so no hardware result exists: these cases are
PROJECT-DEFINED, and the expectation is the instructions' pseudocode with the
constant length, written here independently of the engines. The virtual CPU
has no predicate state at SM = 0, so STR of P stores zeros and LDR of P
performs its access and drops the data. See
darwin/cpu/arm64/detail/sm0_vector_length.hpp and docs/architecture.md.

The cases go into one arm64-sm0-vl record, and the record's SVL makes the
runner run them only where the virtual CPU implements the SME profile (the
A64 instructions the engines translate them to run on any hardware, so a host
without SME runs them with the profile assumed; see
arm64-instruction-corpus-sm0-vl). The streaming-mode meaning of the same
words, with the streaming vector length, belongs to the native SME records,
and sme-native-corpus.py leaves out the cases whose results the SME host's
trap would contradict. The same words are UNDEFINED where SME is not
implemented, which arm64-absent-sve under without_sme pins, and every other
SVE word at SM = 0 stays UNDEFINED under the arm64-absent-sve records. The record also holds
CONTROL cases: words the compilers emit beside the spill words (the data
processing, contiguous loads and stores, and predicate construction of an
SVE function) that stay UNDEFINED at SM = 0, so a binary that computes with
SVE outside streaming mode still takes SIGILL.

The spill cases take their words from the compilers (the callee-saved Z8-Z23
and P4-P15 saves and restores of clang 22, Apple clang 21 and GCC 14 objects,
which sme-idiom-corpus.py builds) as well as from a grid of immediates,
registers and bases: in a page, misaligned, across a page, at an unmapped
page, through a tagged pointer, into read-only memory, and with a
misaligned SP.

Run it on a finished dump, after the native floating-point trap step, and
--repack the result; the invalid-encoding step still runs last.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import dataclasses
import functools
import hashlib
import importlib.util
import json
import pathlib
import re
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent


def _load(name: str, filename: str):
    module = sys.modules.get(name)
    if module is None:
        spec = importlib.util.spec_from_file_location(name, HERE / filename)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return module


corpus = _load("build_unit_corpus", "build-unit-corpus.py")
sme_table = _load("sme_encoding_table", "sme_encoding_table.py")

SITE = "tools/arm64-tests/sm0-vl-corpus.py"
PRODUCER = "arm64-sm0-vl"
# The streaming vector length the record's SME state has, as the native SME
# records do; it is the profile the virtual CPU implements, not what the
# family counts.
SVL = 64
SVC = 0xD400_0001
ZA = corpus.SVCR_ZA
# The constant non-streaming vector length, in bytes.
VL = 16


def pattern_count(pattern: int, elements: int) -> int:
    """DecodePredCount()."""
    if pattern == 0:  # POW2
        count = 1 << (elements.bit_length() - 1)
    elif 1 <= pattern <= 8:  # VL1 to VL8
        count = pattern
    elif 9 <= pattern <= 13:  # VL16 to VL256
        count = 16 << (pattern - 9)
    elif pattern == 29:  # MUL4
        count = elements - elements % 4
    elif pattern == 30:  # MUL3
        count = elements - elements % 3
    elif pattern == 31:  # ALL
        count = elements
    else:
        count = 0
    return count if count <= elements else 0


def signed(value: int, bits: int) -> int:
    return value - (1 << bits) if value >> (bits - 1) else value


def cnt(size: int, pattern: int, multiplier: int, rd: int) -> int:
    return (0x0420_E000 | size << 22 | (multiplier - 1) << 16 |
            pattern << 5 | rd)


def inc_dec(decrement: bool, size: int, pattern: int, multiplier: int,
            rdn: int) -> int:
    return (0x0430_E000 | (0x400 if decrement else 0) | size << 22 |
            (multiplier - 1) << 16 | pattern << 5 | rdn)


def rdvl(immediate: int, rd: int) -> int:
    return 0x04BF_5000 | (immediate & 63) << 5 | rd


def add_length(predicate: bool, rd: int, rn: int, immediate: int) -> int:
    return ((0x0460_5000 if predicate else 0x0420_5000) | rn << 16 |
            (immediate & 63) << 5 | rd)


class Registers:
    """The general registers of the base fill; 31 is SP."""

    def __init__(self) -> None:
        self.value = list(corpus.FILL_REGISTERS)

    def get(self, number: int, sp: bool) -> int:
        return self.value[31] if number == 31 and sp else \
            0 if number == 31 else self.value[number]

    def set(self, number: int, value: int, sp: bool) -> None:
        if number != 31 or sp:
            self.value[number] = value & (1 << 64) - 1


def execute(word: int) -> Registers:
    """The registers after a family word, from the instructions' pseudocode
    with a vector length of VL bytes."""
    registers = Registers()
    rd = word & 31
    rn = word >> 16 & 31
    size = word >> 22 & 3
    multiplier = (word >> 16 & 15) + 1
    pattern = word >> 5 & 31
    immediate = signed(word >> 5 & 63, 6)
    count = pattern_count(pattern, VL // (1 << size)) * multiplier
    if word & 0xFF30_FC00 == 0x0420_E000:  # CNT[BHWD]
        registers.set(rd, count, False)
    elif word & 0xFF30_FC00 in (0x0430_E000, 0x0430_E400):  # INC, DEC
        delta = -count if word & 0x400 else count
        registers.set(rd, registers.get(rd, False) + delta, False)
    elif word & 0xFFFF_F800 == 0x04BF_5000:  # RDVL
        registers.set(rd, immediate * VL, False)
    elif word & 0xFFA0_F800 == 0x0420_5000:  # ADDVL, ADDPL
        scale = VL // 8 if word & 0x0040_0000 else VL
        registers.set(rd, registers.get(rn, True) + immediate * scale, True)
    else:
        raise SystemExit(f"{word:08x}: not a vector-length word")
    return registers


def register_name(number: int, sp: bool) -> str:
    return ("SP" if sp else "XZR") if number == 31 else f"X{number}"


def operands(word: int) -> str:
    rd = word & 31
    rn = word >> 16 & 31
    pattern = word >> 5 & 31
    multiplier = (word >> 16 & 15) + 1
    immediate = signed(word >> 5 & 63, 6)
    if word & 0xFFFF_F800 == 0x04BF_5000:
        return f"{register_name(rd, False)}, #{immediate}"
    if word & 0xFFA0_F800 == 0x0420_5000:
        return (f"{register_name(rd, True)}, {register_name(rn, True)}, "
                f"#{immediate}")
    return (f"{register_name(rd, False)}, pattern {pattern}, "
            f"MUL #{multiplier}")


def words() -> list[tuple[int, int]]:
    """(word, SVCR) of every case: each element size, every predicate
    pattern, including the reserved ones that count nothing, and three
    multipliers of CNT, INC and DEC, over destinations that rotate through
    the registers; RDVL and ADDVL and ADDPL for every immediate; the
    destination XZR where it discards the result and SP where it names the
    stack; and one word of each form with ZA enabled."""
    items: list[tuple[int, int]] = []
    rotation = 0

    def register() -> int:
        nonlocal rotation
        rotation += 1
        number = rotation * 7 % 30
        return number + 1 if number == 18 else number  # not X18
    for size in range(4):
        for pattern in range(32):
            for multiplier in (1, 3, 16):
                items.append((cnt(size, pattern, multiplier, register()), 0))
                items.append((inc_dec(False, size, pattern, multiplier,
                                      register()), 0))
                items.append((inc_dec(True, size, pattern, multiplier,
                                      register()), 0))
        items.append((cnt(size, 31, 1, 31), 0))
        items.append((inc_dec(False, size, 31, 1, 31), 0))
        items.append((inc_dec(True, size, 31, 1, 31), 0))
    for immediate in range(-32, 32):
        items.append((rdvl(immediate, register()), 0))
        for predicate in (False, True):
            for rd, rn in ((1, 2), (3, 3), (31, 4), (5, 31), (31, 31)):
                items.append((add_length(predicate, rd, rn, immediate), 0))
    items.append((rdvl(1, 31), 0))
    for word in (cnt(3, 31, 1, 0), cnt(0, 31, 1, 30), inc_dec(False, 1, 31, 2, 7),
                 inc_dec(True, 2, 5, 3, 8), rdvl(-5, 9), add_length(False, 31, 31, 3),
                 add_length(True, 31, 6, -4)):
        items.append((word, ZA))
    return items


# --------------------------------------------------------------------------
# The spill words: LDR and STR of Z and P, scalar base plus immediate, MUL VL

# Data regions sit above every mapping XNU places itself and below 2**48, as
# the native SME records place theirs. A region is readable and writable
# (flags 0x13) or readable only (0x01).
DATA = 0x100_0000_0000
PAGE = 0x4000
WIDE = 0x10000
MIDDLE = DATA + WIDE // 2
BOUNDARY = DATA + PAGE
READ_WRITE = 0x13
READ_ONLY = 0x01
MASK64 = (1 << 64) - 1
# A pointer's top byte, which a tagged access ignores with TBI enabled.
TAG = 0x5A00_0000_0000_0000


def spill_word(store: bool, predicate: bool, transfer: int, base: int,
               immediate: int) -> int:
    imm9 = immediate & 511
    return ((0xE580_0000 if store else 0x8580_0000) |
            (0 if predicate else 0x4000) | (imm9 >> 3) << 16 |
            (imm9 & 7) << 10 | base << 5 | transfer)


@dataclasses.dataclass(frozen=True)
class Spill:
    store: bool
    predicate: bool
    transfer: int
    base: int
    immediate: int

    @property
    def word(self) -> int:
        return spill_word(self.store, self.predicate, self.transfer,
                          self.base, self.immediate)

    @property
    def size(self) -> int:
        """The bytes moved: a Z register is the vector length, a P register
        one bit per byte of it."""
        return VL // 8 if self.predicate else VL

    @property
    def offset(self) -> int:
        """MUL VL scales by the size of the register."""
        return self.immediate * self.size


def decode_spill(word: int) -> Spill | None:
    """The fields of a spill word, from the encoding, or None."""
    if word & 0xFFC0_E000 in (0x8580_4000, 0xE580_4000):
        predicate = False
    elif word & 0xFFC0_E010 in (0x8580_0000, 0xE580_0000):
        predicate = True
    else:
        return None
    imm9 = (word >> 16 & 63) << 3 | word >> 10 & 7
    return Spill(store=bool(word & 0x6000_0000), predicate=predicate,
                 transfer=word & (15 if predicate else 31),
                 base=word >> 5 & 31,
                 immediate=imm9 - 512 if imm9 >= 256 else imm9)


# The words the compilers emit for the callee-saved saves and restores of Z8
# to Z23 and P4 to P15, and a few plain ones: clang 22, Apple clang 21 and
# GCC 14 at -O1 to -O3 (sme-idiom-corpus.py). Their bases are SP, the frame
# pointer and X0 to X2.
REAL_SPILL_WORDS = (
    0x8580_03E4, 0x8580_07E5, 0x8580_0BE6, 0x8580_0FE7, 0x8580_13E8, 0x8580_13EF,
    0x8580_17E9, 0x8580_17EE, 0x8580_1BEA, 0x8580_1BED, 0x8580_1FEB, 0x8580_1FEC,
    0x8580_4000, 0x8580_401F, 0x8580_43EF, 0x8580_43F7, 0x8580_47EE, 0x8580_47F6,
    0x8580_4BE8, 0x8580_4BED, 0x8580_4BF5, 0x8580_4BF7, 0x8580_4FE9, 0x8580_4FEC,
    0x8580_4FF4, 0x8580_4FF6, 0x8580_53EA, 0x8580_53EB, 0x8580_53F3, 0x8580_53F5,
    0x8580_57EA, 0x8580_57EB, 0x8580_57F2, 0x8580_57F4, 0x8580_5BE9, 0x8580_5BEC,
    0x8580_5BF1, 0x8580_5BF3, 0x8580_5FE8, 0x8580_5FED, 0x8580_5FF0, 0x8580_5FF2,
    0x8581_03EB, 0x8581_03EC, 0x8581_07EA, 0x8581_07ED, 0x8581_0BE9, 0x8581_0BEE,
    0x8581_0FE8, 0x8581_0FEF, 0x8581_13E7, 0x8581_17E6, 0x8581_1BE5, 0x8581_1FE4,
    0x8581_43EE, 0x8581_43EF, 0x8581_43F1, 0x8581_47EE, 0x8581_47EF, 0x8581_47F0,
    0x8581_4BED, 0x8581_4BEF, 0x8581_4BF0, 0x8581_4FEC, 0x8581_4FEE, 0x8581_4FF1,
    0x8581_53EB, 0x8581_53ED, 0x8581_53F2, 0x8581_57EA, 0x8581_57EC, 0x8581_57F3,
    0x8581_5BE9, 0x8581_5BEB, 0x8581_5BF4, 0x8581_5FE8, 0x8581_5FEA, 0x8581_5FF5,
    0x8582_43E9, 0x8582_43F6, 0x8582_47E8, 0x8582_47F7, 0x85BD_57A0, 0xE580_03E4,
    0xE580_07E5, 0xE580_0BE6, 0xE580_0FE7, 0xE580_13E8, 0xE580_13EF, 0xE580_17E9,
    0xE580_17EE, 0xE580_1BEA, 0xE580_1BED, 0xE580_1FEB, 0xE580_1FEC, 0xE580_4000,
    0xE580_4040, 0xE580_43EF, 0xE580_43F7, 0xE580_47EE, 0xE580_47F6, 0xE580_4BE8,
    0xE580_4BED, 0xE580_4BF5, 0xE580_4BF7, 0xE580_4FE9, 0xE580_4FEC, 0xE580_4FF4,
    0xE580_4FF6, 0xE580_53EA, 0xE580_53EB, 0xE580_53F3, 0xE580_53F5, 0xE580_57EA,
    0xE580_57EB, 0xE580_57F2, 0xE580_57F4, 0xE580_5BE9, 0xE580_5BEC, 0xE580_5BF1,
    0xE580_5BF3, 0xE580_5FE8, 0xE580_5FED, 0xE580_5FF0, 0xE580_5FF2, 0xE581_03EB,
    0xE581_03EC, 0xE581_07EA, 0xE581_07ED, 0xE581_0BE9, 0xE581_0BEE, 0xE581_0FE8,
    0xE581_0FEF, 0xE581_13E7, 0xE581_17E6, 0xE581_1BE5, 0xE581_1FE4, 0xE581_43EE,
    0xE581_43EF, 0xE581_43F1, 0xE581_47EE, 0xE581_47EF, 0xE581_47F0, 0xE581_4BED,
    0xE581_4BEF, 0xE581_4BF0, 0xE581_4FEC, 0xE581_4FEE, 0xE581_4FF1, 0xE581_53EB,
    0xE581_53ED, 0xE581_53F2, 0xE581_57EA, 0xE581_57EC, 0xE581_57F3, 0xE581_5BE9,
    0xE581_5BEB, 0xE581_5BF4, 0xE581_5FE8, 0xE581_5FEA, 0xE581_5FF5, 0xE582_43E9,
    0xE582_43F6, 0xE582_47E8, 0xE582_47F7, 0xE5BD_57A0,
)

# Words the same compilers emit beside the spill words, which the virtual CPU
# does not execute at SM = 0: SVE data processing, contiguous loads and
# stores, and predicate construction.
REAL_CONTROL_WORDS = (
    0x25A0_C020,  # add z0.s, z0.s, #1
    0x25A0_C03F,  # add z31.s, z31.s, #1
    0x04A0_0020,  # add z0.s, z1.s, z0.s
    0x04BE_03FE,  # add z30.s, z31.s, z30.s
    0x2538_C000,  # mov z0.b, #0
    0x2500_4200,  # not p0.b, p0/z, p0.b
    0xA540_BC1F,  # ld1w { z31.s }, p7/z, [x0]
    0xA543_5C1F,  # ld1w { z31.s }, p7/z, [x0, x3, lsl #2]
    0x047F_33E0,  # mov z0.d, z31.d
    0x2518_E400,  # pfalse p0.b
    0x2518_E3E7,  # ptrue p7.b
    0xE540_FC1F,  # st1w { z31.s }, p7, [x0]
    0xE543_5C1E,  # st1w { z30.s }, p7, [x0, x3, lsl #2]
    0x25A2_0C67,  # whilelo p7.s, w3, w2
)

# The words around the spill forms the encoding table does not name, which
# are UNDEFINED anywhere: the reserved bit of the predicate forms, and the
# vector forms with the other values of bits 15:13.
RESERVED_SPILL_WORDS = (
    0x8580_0010, 0xE580_0010, 0x8580_0BF4, 0xE580_07F5, 0x8580_2000,
    0xE580_6000,
)


class Region:
    """A data region: its address, flags, and bytes."""

    def __init__(self, address: int, size: int, flags: int,
                 seed: int) -> None:
        self.address = address
        self.size = size
        self.flags = flags
        self.initial = region_data(seed, size)
        self.data = bytearray(self.initial)

    def contains(self, address: int) -> bool:
        return self.address <= address < self.address + self.size

    @property
    def writable(self) -> bool:
        return bool(self.flags & 2)


@functools.cache
def region_data(seed: int, size: int) -> bytes:
    """Bytes with no repetition at the sizes a spill moves, so a misplaced
    access cannot pass."""
    start = (seed * 0x0100_0193) & 0xFFFF_FFFF
    return bytes(((start + offset) * 0x9E37_79B1 & 0xFFFF_FFFF) >> 24
                 for offset in range(size))


@dataclasses.dataclass
class Scenario:
    spill: Spill
    # The address the access starts at. An SP base names SP instead: the
    # address is then SP plus the offset.
    address: int
    regions: tuple[tuple[int, int, int], ...] = ((DATA, WIDE, READ_WRITE),)
    sp: int | None = None
    svcr: int = 0
    tagged: bool = False
    where: str = "in a page"


def locate(regions: list[Region], address: int) -> Region | None:
    for region in regions:
        if region.contains(address):
            return region
    return None


def run_scenario(scenario: Scenario):
    """(event, fault (address, access) or None, regions, register changes) of
    a spill, from the pseudocode with a vector length of VL bytes: an SP base
    that is not 16-byte aligned faults at SP; the access then moves its bytes,
    and faults at the first one that does not translate for its access, with
    no byte of a store written; a Z load fills V and a P load drops its data;
    a Z store writes V and a P store writes zeros."""
    spill = scenario.spill
    regions = [Region(address, size, flags, index + 1)
               for index, (address, size, flags) in
               enumerate(scenario.regions)]
    access = "write" if spill.store else "read"
    address = scenario.address
    if spill.base == 31:
        assert scenario.sp is not None and \
            address == scenario.sp + spill.offset
        if scenario.sp % 16:
            return "memory-fault", (scenario.sp, access), regions, {}
    for byte in range(spill.size):
        region = locate(regions, address + byte)
        if region is None or spill.store and not region.writable:
            return "memory-fault", (address + byte, access), regions, {}

    def read(start: int, size: int) -> bytes:
        return bytes(locate(regions, start + byte).data[
            start + byte - locate(regions, start + byte).address]
            for byte in range(size))

    def write(start: int, data: bytes) -> None:
        for byte, value in enumerate(data):
            region = locate(regions, start + byte)
            region.data[start + byte - region.address] = value
    changes: dict[str, str] = {}
    if spill.predicate:
        if spill.store:
            write(address, bytes(spill.size))
    elif spill.store:
        write(address, corpus.FILL_VECTORS[spill.transfer])
    else:
        loaded = read(address, spill.size)
        if loaded != corpus.FILL_VECTORS[spill.transfer]:
            changes[f"v{spill.transfer}"] = loaded.hex()
    return "system-call", None, regions, changes


def register_name(number: int, sp: bool) -> str:
    return ("SP" if sp else "XZR") if number == 31 else f"X{number}"


def spill_operands(spill: Spill) -> str:
    kind = "P" if spill.predicate else "Z"
    base = register_name(spill.base, True)
    mul = f", #{spill.immediate}, MUL VL" if spill.immediate else ""
    return f"{kind}{spill.transfer}, [{base}{mul}]"


def spill_case(scenario: Scenario, program: int, blobs: dict) -> dict:
    spill = scenario.spill
    event, fault, regions, changes = run_scenario(scenario)
    initial: dict[str, str] = {}
    if spill.base == 31:
        initial["sp"] = corpus.hex64(scenario.sp)
    else:
        value = (scenario.address - spill.offset) & MASK64
        if scenario.tagged:
            value |= TAG
        initial[f"x{spill.base}"] = corpus.hex64(value)
    if scenario.tagged:
        initial["tbi_enabled"] = corpus.hex64(1)
    if scenario.svcr:
        initial["svcr"] = corpus.hex64(scenario.svcr)
    expected = dict(changes)
    if event == "system-call":
        expected["pc"] = corpus.hex64(corpus.CODE_BASE + 8)
    row = table().row_for(spill.word)
    if row is None or decode_spill(spill.word) != spill:
        raise SystemExit(f"{spill.word:08x}: not a spill row")
    name = (f"{row.stable_id} {row.encoding} {spill.word:08x} "
            f"{spill_operands(spill)} {scenario.where} at SM = 0"
            f"{', ZA = 1' if scenario.svcr & ZA else ''} (project-defined)")
    item = {
        "name": name, "program": program, "event": event,
        "ordinals": [0, 0], "invalid_encoding": False,
        "minimization": "full", "native": [], "initial": initial,
        "expected": expected, "retired": 2 if event == "system-call" else 0,
        "memory": [],
    }
    if fault is not None:
        item["fault"] = {"address": corpus.hex64(fault[0]), "access": fault[1]}
    for region in regions:
        digest = hashlib.sha256(region.initial).hexdigest()
        blobs[digest] = region.initial
        item["memory"].append({
            "address": corpus.hex64(region.address), "size": region.size,
            "flags": region.flags, "blob": digest,
            "expected": [[offset, data.hex()] for offset, data in
                         corpus.runs_between(region.initial,
                                             bytes(region.data))],
            "tags": [], "expected_tags": []})
    return item


def control_case(word: int, program: int, why: str) -> dict:
    return {
        "name": f"{word:08x} {why} is undefined at SM = 0 (project-defined)",
        "program": program, "event": "illegal-instruction",
        "ordinals": [0, 0], "invalid_encoding": False,
        "minimization": "full", "native": [], "initial": {}, "expected": {},
        "retired": 0,
    }


def spills() -> list[Scenario]:
    """Every scenario: a grid of the four forms over immediates, transfer
    registers and bases; the compilers' words; and the placements and faults
    each form can meet."""
    scenarios: list[Scenario] = []

    def place(spill: Spill, **options) -> Scenario:
        # Inside the wide region: an SP base is aligned, any other base
        # names an address that is not.
        if spill.base == 31:
            sp = MIDDLE
            return Scenario(spill, sp + spill.offset, sp=sp, **options)
        return Scenario(spill, MIDDLE + 3, **options)
    immediates = (0, 1, -1, 2, 15, 16, 17, -16, -17, 31, -19, 127, 128, 255,
                  -128, -129, -255, -256)
    turn = 0
    for predicate in (False, True):
        for store in (False, True):
            for immediate in immediates:
                for bases in ((0, 29), (31, 2)):
                    base = bases[turn % 2]
                    transfer = (0, 5, 15, 31 if not predicate else 15)[turn % 4]
                    turn += 1
                    scenarios.append(place(Spill(store, predicate, transfer,
                                                 base, immediate)))
    for word in REAL_SPILL_WORDS:
        scenarios.append(place(decode_spill(word),
                               where="as the compilers emit it"))
    wide = ((DATA, WIDE, READ_WRITE),)
    first = (DATA, PAGE, READ_WRITE)
    second = (DATA + PAGE, PAGE, READ_WRITE)
    for predicate in (False, True):
        for store in (False, True):
            for transfer in (3, 31 if not predicate else 15):
                for immediate in (0, -3):
                    spill = Spill(store, predicate, transfer, 1, immediate)
                    start = BOUNDARY - spill.size // 2 - 0
                    scenarios.append(Scenario(
                        spill, start, regions=(first, second),
                        where="across a page"))
                    scenarios.append(Scenario(
                        spill, start + 1, regions=(first, second),
                        where="across a page, misaligned by 1"))
                    scenarios.append(Scenario(
                        spill, start, regions=(first,),
                        where="across a page into an unmapped one"))
                    scenarios.append(Scenario(
                        spill, start, regions=(second,),
                        where="across a page from an unmapped one"))
                    scenarios.append(Scenario(
                        spill, MIDDLE + 5 if immediate else MIDDLE,
                        regions=((DATA, WIDE, READ_ONLY),),
                        where="into a read-only page"))
                    scenarios.append(place(spill, tagged=True,
                                           where="through a tagged pointer"))
                    scenarios.append(Scenario(
                        spill, BOUNDARY - spill.size // 2, regions=(first, second),
                        tagged=True,
                        where="across a page through a tagged pointer"))
        for store in (False, True):
            spill = Spill(store, predicate, 6, 31, 1)
            scenarios.append(Scenario(
                spill, MIDDLE + 8 + spill.offset, sp=MIDDLE + 8,
                where="at a misaligned SP"))
            for offset in (0, 3, -17):
                spill = Spill(store, predicate, 7, 31, offset)
                scenarios.append(Scenario(
                    spill, MIDDLE + spill.offset, sp=MIDDLE, svcr=ZA,
                    where="at SP"))
            scenarios.append(Scenario(Spill(store, predicate, 2, 3, -4),
                                      MIDDLE + 1, svcr=ZA))
    return scenarios


def controls() -> list[tuple[int, str]]:
    return ([(word, "a word the compilers emit beside the spills")
             for word in REAL_CONTROL_WORDS] +
            [(word, "a reserved encoding beside the spills")
             for word in RESERVED_SPILL_WORDS])


@functools.cache
def table():
    return sme_table.load()


def case_for(word: int, svcr: int, program: int) -> dict:
    registers = execute(word)
    expected = {"pc": corpus.hex64(corpus.CODE_BASE + 8)}
    for number in range(32):
        if registers.value[number] != corpus.FILL_REGISTERS[number]:
            expected["sp" if number == 31 else f"x{number}"] = \
                corpus.hex64(registers.value[number])
    row = table().row_for(word)
    if row is None or not sme_table.sm0_vector_length_word(word):
        raise SystemExit(f"{word:08x}: not a row of the family")
    name = (f"{row.stable_id} {row.encoding} {word:08x} {operands(word)} "
            f"at SM = 0{', ZA = 1' if svcr & ZA else ''} (project-defined)")
    return {
        "name": name, "program": program, "event": "system-call",
        "ordinals": [0, 0], "invalid_encoding": False,
        "minimization": "full", "native": [],
        "initial": {"svcr": corpus.hex64(svcr)} if svcr else {},
        "expected": expected, "retired": 2,
    }


@functools.cache
def build() -> tuple[dict, dict]:
    """(the record, the data blobs its memory regions name, by digest)."""
    programs = []
    cases = []
    blobs: dict[str, bytes] = {}
    for word, svcr in words():
        programs.append({"entry": 0, "words": [[0, f"{word:08x}"],
                                               [4, f"{SVC:08x}"]]})
        cases.append(case_for(word, svcr, len(programs) - 1))
    for scenario in spills():
        programs.append({"entry": 0,
                         "words": [[0, f"{scenario.spill.word:08x}"],
                                   [4, f"{SVC:08x}"]]})
        cases.append(spill_case(scenario, len(programs) - 1, blobs))
    for word, why in controls():
        programs.append({"entry": 0, "words": [[0, f"{word:08x}"],
                                               [4, f"{SVC:08x}"]]})
        cases.append(control_case(word, len(programs) - 1, why))
    item = {
        "record": PRODUCER, "producer": PRODUCER,
        "site": SITE, "profiles": ["4k"], "options": [],
        "svl": SVL, "programs": programs, "cases": cases,
    }
    return item, blobs


def record() -> dict:
    return build()[0]


def blobs() -> dict[str, bytes]:
    return build()[1]


def counts() -> dict[str, int]:
    """The cases by kind: completed or faulting vector-length and spill
    words, and the controls, which are UNDEFINED."""
    item = record()
    control = sum(case["event"] == "illegal-instruction"
                  for case in item["cases"])
    spill = sum("memory" in case for case in item["cases"])
    return {"cases": len(item["cases"]), "spill_cases": spill,
            "control_cases": control}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    options = parser.parse_args()
    lines = options.dump.read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    kept = [line for line in lines[1:] if line.strip() and
            not (line.startswith('{"cases"') and
                 json.loads(line)["producer"] == PRODUCER)]
    item = record()
    known = {match.group(1) for line in kept
             if (match := re.match(r'\{"blob": "([0-9a-f]{64})"', line))}
    fresh = [json.dumps({"blob": digest, "zlib": zlib.compress(data, 9).hex()},
                        sort_keys=True)
             for digest, data in sorted(blobs().items())
             if digest not in known]
    summary = counts()
    header["meta"]["sm0_vl"] = {
        "script": SITE,
        "sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes())
        .hexdigest(),
        "table": {"file": sme_table.TABLE_PATH, "sha256": table().sha256},
        "svl": SVL, "vector_length": VL, **summary,
    }
    with options.output.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in kept:
            sink.write(line + "\n")
        for line in fresh:
            sink.write(line + "\n")
        sink.write(json.dumps(item, sort_keys=True) + "\n")
    print(f"appended {summary['cases']} project-defined SM = 0 cases "
          f"({summary['spill_cases']} spill, {summary['control_cases']} "
          f"control) and {len(fresh)} data blobs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
