#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read the generated SVE/SME encoding table for the corpus tools.

darwin/cpu/arm64/detail/sme_encodings.hpp is the one classification both
engines decode SVE, SME, and streaming-mode FP&SIMD words with
(tests/tooling/arm64/generate_sme_encoding_table.py writes it). The corpus
derives its SVE and SME words from the same rows, so this module reads the
checked-in header rather than regenerating it, and checks every array
against its declared size.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import pathlib
import re
from dataclasses import dataclass


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
TABLE = REPOSITORY / "darwin" / "cpu" / "arm64" / "detail" / "sme_encodings.hpp"
TABLE_PATH = TABLE.relative_to(REPOSITORY).as_posix()
SME_SECTION = 0x8000

# The profile features a row can need, as runtime ArmFeature tokens, in the
# table's bit order.
FEATURES = ("sme", "sme2", "sme_f64f64", "sme_i16i64", "bf16", "i8mm", "sve",
            "sve2", "sve_aes", "sve_pmull128", "sve_sha3", "sve_bitperm",
            "sve_sm4")


class TableError(ValueError):
    pass


def sm0_vector_length_word(word: int) -> bool:
    """Whether a word is in the SM = 0 vector-length words: CNT[BHWD]
    (scalar), the scalar INC[BHWD] and DEC[BHWD], RDVL, ADDVL and ADDPL. A
    virtual CPU with FEAT_SME and without FEAT_SVE executes them outside
    streaming mode for a vector length of 16 bytes, a deliberate departure
    from the architecture, in which they are UNDEFINED there and an M4 traps
    them. This mirrors sm0_vector_length_word() in
    darwin/cpu/arm64/detail/sm0_vector_length.hpp, which arm64-sm0-vector-length
    checks against the table's rows."""
    count = word & 0xFF30_FC00
    return count in (0x0420_E000, 0x0430_E000, 0x0430_E400) or \
        word & 0xFFFF_F800 == 0x04BF_5000 or \
        word & 0xFFA0_F800 == 0x0420_5000


def sm0_spill_word(word: int) -> bool:
    """Whether a word is an SM = 0 spill word: LDR or STR of a vector
    register Z or of a predicate register P with a scalar base and an
    immediate offset. A virtual CPU with FEAT_SME and without FEAT_SVE
    executes them outside streaming mode, moving 16 bytes for Z and 2 for P
    (a deliberate departure, like the vector-length family). This mirrors
    sm0_spill_word() in darwin/cpu/arm64/detail/sm0_vector_length.hpp."""
    return word & 0xFFC0_E000 in (0x8580_4000, 0xE580_4000) or \
        word & 0xFFC0_E010 in (0x8580_0000, 0xE580_0000)


def sm0_family_word(word: int) -> bool:
    """Whether a word is in the SM = 0 family: a vector-length word or a
    spill word."""
    return sm0_vector_length_word(word) or sm0_spill_word(word)


@dataclass(frozen=True)
class Segment:
    low: int
    width: int
    constant: int
    field: bool

    def value(self, word: int) -> int:
        if not self.field:
            return self.constant
        return (word >> self.low) & ((1 << self.width) - 1)


@dataclass(frozen=True)
class Operand:
    kind: str
    count: int
    stride: int
    segments: tuple[Segment, ...]

    def number(self, word: int) -> int:
        number = 0
        for segment in self.segments:
            number = number << segment.width | segment.value(word)
        return number

    def fields(self) -> tuple[Segment, ...]:
        return tuple(segment for segment in self.segments if segment.field)


@dataclass(frozen=True)
class Memory:
    shape: str
    direction: str
    offset: str
    esize: int
    esize_shift: Segment | None
    msize: int
    registers: int
    stride: int
    sign_extend: bool
    nontemporal: bool
    immediate_signed: bool
    immediate: tuple[Segment, ...]
    base_operand: int
    index_operand: int | None
    vector_length: str
    index_bits: int
    index_shift: int
    index_sign: Segment | None

    def element_bits(self, word: int) -> int:
        if self.esize_shift is None:
            return self.esize
        return self.esize << self.esize_shift.value(word)

    def immediate_value(self, word: int) -> int:
        value = width = 0
        for segment in self.immediate:
            value = value << segment.width | segment.value(word)
            width += segment.width
        if self.immediate_signed and width and value >> (width - 1):
            value -= 1 << width
        return value


@dataclass(frozen=True)
class Row:
    index: int
    mask: int
    value: int
    form: int
    stable_id: str
    encoding: str
    requirement: tuple[str, ...]
    non_streaming_requirement: tuple[str, ...]
    checks: tuple[str, ...]
    mode: tuple[str, ...]
    operands: tuple[Operand, ...]
    memory: Memory | None
    min_svl: int
    witness: int

    @property
    def in_profile(self) -> bool:
        """Whether the SME profile implements the row in streaming mode;
        a row only non-streaming SVE would execute is not, as the virtual
        CPU has no such profile."""
        return bool(self.requirement)

    @property
    def streaming(self) -> bool:
        return "streaming" in self.mode

    @property
    def za(self) -> bool:
        return "za" in self.mode

    @property
    def zt0(self) -> bool:
        return "zt0" in self.mode

    def matches(self, word: int) -> bool:
        return word & self.mask == self.value


@dataclass(frozen=True)
class FpSimdRow:
    index: int
    mask: int
    value: int
    form: int
    stable_id: str
    encoding: str
    advsimd: bool
    witness: int


@dataclass(frozen=True)
class Table:
    rows: tuple[Row, ...]
    fp_simd: tuple[FpSimdRow, ...]
    sha256: str

    def row_for(self, word: int) -> Row | None:
        """The row whose encoding the word matches, as sme_decode.hpp's
        find_sme_encoding() finds it."""
        for row in self.rows:
            if row.matches(word):
                return row
        return None


def _number(text: str) -> int:
    return int(text.replace("'", "").rstrip("U"), 0)


def _array(text: str, element: str, name: str) -> list[str]:
    """The entry lines of `std::array<element, N> name{{ ... }};`, checked
    against N."""
    match = re.search(
        rf"inline constexpr std::array<{re.escape(element)}, (\d+)> "
        rf"{re.escape(name)}\{{\{{\n(.*?)\n\}}\}};", text, re.DOTALL)
    if match is None:
        raise TableError(f"{TABLE_PATH}: lacks {name}")
    lines = [line.strip() for line in match.group(2).splitlines()
             if line.strip()]
    if element.endswith("uint32_t"):
        values = [item.strip() for line in lines
                  for item in line.split(",") if item.strip()]
        lines = values
    if len(lines) != int(match.group(1)):
        raise TableError(f"{TABLE_PATH}: {name} has {len(lines)} entries, "
                         f"declares {match.group(1)}")
    return lines


_SEGMENT = re.compile(r"sme_(field|constant)\((\d+), (\d+)\)")


def _segments(text: str) -> tuple[Segment, ...]:
    segments = []
    for kind, first, second in _SEGMENT.findall(text):
        if kind == "field":
            segments.append(Segment(int(first), int(second), 0, True))
        else:
            segments.append(Segment(0, int(first), int(second), False))
    return tuple(segments)


def _flags(text: str, prefix: str) -> tuple[str, ...]:
    if text.strip() == "0":
        return ()
    names = []
    for item in text.split("|"):
        item = item.strip()
        if not item.startswith(prefix):
            raise TableError(f"{TABLE_PATH}: unexpected flag {item!r}")
        names.append(item[len(prefix):])
    return tuple(names)


_OPERAND = re.compile(
    r"^SmeOperand\{SmeOperandKind::(\w+), (\d+), (\d+), (\d+), "
    r"\{\{(.*)\}\}\},$")
_MEMORY = re.compile(
    r"^SmeMemory\{SmeMemoryShape::(\w+), SmeMemoryDirection::(\w+), "
    r"SmeAddressOffset::(\w+), (\d+), (\{\}|sme_field\(\d+, \d+\)), (\d+), "
    r"(\d+), (\d+), (true|false), (true|false), (true|false), (\d+), "
    r"\{\{(.*?)\}\}, (\d+), (\d+|sme_no_operand), SmeVectorLength::(\w+), "
    r"(\d+), (\d+), (\{\}|sme_field\(\d+, \d+\))\},$")
_ROW = re.compile(
    r"^SmeEncoding\{(0x[0-9a-f']+U), (0x[0-9a-f']+U), (\d+)U, ([^,]+), "
    r"([^,]+), ([^,]+), ([^,]+), (\d+), (\d+), (\d+), (\d+)\}, "
    r"// (\S+) (\S+)$")
_FP_SIMD = re.compile(
    r"^FpSimdStreamingClass\{(0x[0-9a-f']+U), (0x[0-9a-f']+U), (\d+)U, "
    r"(true|false)\}, // (\S+) (\S+)$")


def load(path: pathlib.Path = TABLE) -> Table:
    data = path.read_bytes()
    text = data.decode("utf-8")
    operands = []
    for line in _array(text, "SmeOperand", "sme_encoding_operands"):
        match = _OPERAND.match(line)
        if match is None:
            raise TableError(f"{TABLE_PATH}: cannot read operand {line!r}")
        kind, count, stride, segment_count, body = match.groups()
        segments = _segments(body)
        if len(segments) != int(segment_count):
            raise TableError(f"{TABLE_PATH}: operand {line!r}")
        operands.append(Operand(kind, int(count), int(stride), segments))
    memories = []
    for line in _array(text, "SmeMemory", "sme_encoding_memory"):
        match = _MEMORY.match(line)
        if match is None:
            raise TableError(f"{TABLE_PATH}: cannot read memory {line!r}")
        (shape, direction, offset, esize, shift, msize, registers, stride,
         sign, nontemporal, signed, immediate_count, immediate, base,
         index, vector_length, index_bits, index_shift,
         index_sign) = match.groups()
        shift_segments = _segments(shift)
        sign_segments = _segments(index_sign)
        immediate_segments = _segments(immediate)
        if len(immediate_segments) != int(immediate_count):
            raise TableError(f"{TABLE_PATH}: memory {line!r}")
        memories.append(Memory(
            shape=shape, direction=direction, offset=offset,
            esize=int(esize),
            esize_shift=shift_segments[0] if shift_segments else None,
            msize=int(msize), registers=int(registers), stride=int(stride),
            sign_extend=sign == "true", nontemporal=nontemporal == "true",
            immediate_signed=signed == "true",
            immediate=immediate_segments, base_operand=int(base),
            index_operand=None if index == "sme_no_operand" else int(index),
            vector_length=vector_length, index_bits=int(index_bits),
            index_shift=int(index_shift),
            index_sign=sign_segments[0] if sign_segments else None))
    witnesses = [_number(item) for item in _array(
        text, "std::uint32_t", "sme_encoding_witnesses")]
    rows = []
    for index, line in enumerate(_array(text, "SmeEncoding", "sme_encodings")):
        match = _ROW.match(line)
        if match is None:
            raise TableError(f"{TABLE_PATH}: cannot read row {line!r}")
        (mask, value, form, requirement, non_streaming, checks, mode,
         operand_count, operand_begin, memory, min_svl, stable_id,
         encoding) = match.groups()
        form = int(form)
        expected_id = (f"SME-{form - SME_SECTION:04d}" if form & SME_SECTION
                       else f"SVE-{form:04d}")
        if stable_id != expected_id:
            raise TableError(f"{TABLE_PATH}: row {index} names {stable_id}, "
                             f"form {form} is {expected_id}")
        begin, count = int(operand_begin), int(operand_count)
        if begin + count > len(operands) or int(memory) > len(memories):
            raise TableError(f"{TABLE_PATH}: row {index} indexes past a table")
        rows.append(Row(
            index=index, mask=_number(mask), value=_number(value), form=form,
            stable_id=stable_id, encoding=encoding,
            requirement=_flags(requirement, "sme_feature_"),
            non_streaming_requirement=_flags(non_streaming, "sme_feature_"),
            checks=_flags(checks, "sme_check_"),
            mode=_flags(mode, "sme_mode_"),
            operands=tuple(operands[begin:begin + count]),
            memory=memories[int(memory) - 1] if int(memory) else None,
            min_svl=int(min_svl), witness=witnesses[index]))
    for row in rows:
        if not row.matches(row.witness):
            raise TableError(f"{TABLE_PATH}: {row.stable_id} {row.encoding}: "
                             "witness does not match its row")
        if (set(row.requirement) | set(row.non_streaming_requirement)) - \
                set(FEATURES):
            raise TableError(f"{TABLE_PATH}: {row.stable_id}: unknown feature")
    fp_witnesses = [_number(item) for item in _array(
        text, "std::uint32_t", "fp_simd_streaming_witnesses")]
    fp_simd = []
    for index, line in enumerate(_array(
            text, "FpSimdStreamingClass", "fp_simd_streaming_classes")):
        match = _FP_SIMD.match(line)
        if match is None:
            raise TableError(f"{TABLE_PATH}: cannot read class {line!r}")
        mask, value, form, advsimd, stable_id, encoding = match.groups()
        row = FpSimdRow(index=index, mask=_number(mask), value=_number(value),
                        form=int(form), stable_id=stable_id,
                        encoding=encoding, advsimd=advsimd == "true",
                        witness=fp_witnesses[index])
        if row.witness & row.mask != row.value:
            raise TableError(f"{TABLE_PATH}: {stable_id} {encoding}: witness "
                             "does not match its class")
        fp_simd.append(row)
    return Table(rows=tuple(rows), fp_simd=tuple(fp_simd),
                 sha256=hashlib.sha256(data).hexdigest())
