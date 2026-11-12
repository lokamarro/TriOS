#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the gadget engine's register-only streaming gadgets.

A program the gadget engine translates for SVCR.SM = 1 keeps the guest's Z
and P registers in the vCPU's register file and its ZA and ZT0 resident in
the host's (engine/sme.inc). This generator writes one gadget for each
register-only SVE, SVE2 and SME form the shared encoding table
(darwin/cpu/arm64/detail/sme_encodings.hpp) puts in the SME1 profile, and
one for each register-only FP&SIMD class that table marks legal in
streaming mode, for every value of the encoding's bits that are not
register numbers (sizes, immediates, indexes and ZA tiles).

A gadget loads the registers its instruction names from the register file
(or, for general registers, from CpuState) into fixed host registers, runs
the instruction with those host registers in its register fields, and
stores its destination back:

- Z, P and V operands come from the register file at offsets the builder
  computes from the run-time SVL, so nothing here depends on it; a V
  register is the low bits of its Z register, and a scalar result zeroes
  the rest of it as the host instruction does;
- a general register reads CpuState, XZR reads its zero slot, and a write
  of XZR restores the slot;
- an instruction that may write NZCV runs with the guest's flags in the
  host's and keeps what it leaves there;
- a floating-point instruction that can raise exceptions starts with the
  host FPSR clear, ORs what it raised into the guest's FPSR in CpuState,
  and runs again if FPSR.QC is set when it finishes: no streaming-mode
  instruction sets QC, and XNU resets FPSR to 0x0800009F on any kernel
  entry while a thread is in streaming mode. ZA's floating-point forms
  raise no exceptions.

The forms whose immediates would take hundreds of entries are lowered: the
builder computes the immediate (or a count from the run-time SVL) into the
stream, and the gadget runs an equivalent register form. The builder takes
everything else from the generated header, so the two cannot disagree.

The memory forms get one gadget per instruction the host runs for them
(--memory-assembly): each SVE and SME1 load and store shape, element size,
extension and ZA tile slice, and each SIMD&FP register and pair size. The
builder resolves the address mode into the stream (sme_layout.h), so a
gadget runs its native instruction with a host base of x7, no offset and
the transfer registers z0-z3 or p0, governed by p0, and leaves crossings,
misses and faults to the helper engine/sme.inc's slow path calls.

usage: generate_streaming.py --loader LOADER --table TABLE
                             --assembly OUT.inc --memory-assembly OUT.inc
                             --header OUT.hpp
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import re
import sys
from dataclasses import dataclass, field as dataclass_field
from pathlib import Path

SIZES = "bhsd"

# Host registers. The gadget template keeps x8 (operand offsets), x9 (the
# register file), x10 (the destination's address), x11 (scratch and the
# raised FPSR), x16 and x17 (the general destination's offset); x12-x15 hold
# ZA slice indexes; x5-x7, z29-z31, p6 and p7 are a lowered body's scratch.
GENERIC_POOLS = {"z": range(32), "p": range(16), "gpr": (3, 4, 5, 6, 7, 0)}
LOWERED_POOLS = {"z": range(29), "p": range(6), "gpr": (3, 4, 0)}
SLICE_REGISTERS = range(12, 16)

FP_EXCEPTION_NAME = re.compile(r"^(f|bf|scvtf|ucvtf)", re.I)
NZCV_ONLY = {"ptest__p_p_", "ctermeq_rr_", "ctermne_rr_"}


class GenerationError(Exception):
    pass


@dataclass(frozen=True)
class Segment:
    low: int
    width: int
    constant: int
    field: bool


@dataclass
class Operand:
    kind: str
    segments: tuple
    count: int = 1
    stride: int = 1
    dest: bool = False
    hosts: list = dataclass_field(default_factory=list)

    def field_bits(self) -> int:
        bits = 0
        for segment in self.segments:
            if segment.field:
                bits |= ((1 << segment.width) - 1) << segment.low
        return bits

    def encode(self, number: int) -> int | None:
        """The operand's field bits for register `number`, or None where
        its constant segments cannot name it."""
        bits = 0
        for segment in reversed(self.segments):
            part = number & ((1 << segment.width) - 1)
            number >>= segment.width
            if segment.field:
                bits |= part << segment.low
            elif part != segment.constant:
                return None
        return None if number else bits

    def decode(self, word: int) -> int:
        number = 0
        for segment in self.segments:
            part = segment.constant if not segment.field else \
                (word >> segment.low) & ((1 << segment.width) - 1)
            number = number << segment.width | part
        return number

    @property
    def restricted(self) -> bool:
        return any(not segment.field for segment in self.segments)

    @property
    def file(self) -> bool:
        return self.kind in ("z", "p", "v")

    def register(self, index: int = 0, view: str = "") -> str:
        host = self.hosts[index]
        if self.kind in ("z", "v"):
            return f"{view or 'z'}{host}"
        if self.kind == "p":
            return f"p{host}"
        return f"{view or 'x'}{host}"


def register_field(low: int, width: int) -> Segment:
    return Segment(low, width, 0, True)


@dataclass
class Variant:
    lines: list
    word: int | None = None


@dataclass
class Form:
    source: str
    index: int
    name: str
    operands: list
    lowering: str = "none"
    argument: int = 0
    variant_bits: list = dataclass_field(default_factory=list)
    variants: list = dataclass_field(default_factory=list)
    fp: bool = False
    nzcv: str = ""
    immediates: int = 0
    indices: list = dataclass_field(default_factory=list)

    @property
    def slots(self) -> list:
        """(operand, register index) per stream offset, in stream order."""
        return [(operand, index) for operand in self.operands
                if operand.kind != "za_tile"
                for index in range(operand.count)]


def scatter(value: int, bits: list) -> int:
    word = 0
    for position, bit in enumerate(bits):
        if value >> position & 1:
            word |= 1 << bit
    return word


def load_table(loader: Path, table: Path):
    spec = importlib.util.spec_from_file_location("sme_encoding_table", loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules["sme_encoding_table"] = module
    spec.loader.exec_module(module)
    return module.load(table)


def table_operands(row) -> list:
    result = []
    for operand in row.operands:
        segments = tuple(Segment(s.low, s.width, s.constant, s.field)
                         for s in operand.segments)
        result.append(Operand(operand.kind, segments, operand.count,
                              operand.stride))
    for operand in result:
        operand.dest = operand.kind != "za_tile" and any(
            s.field and s.low == 0 for s in operand.segments)
    return result


def allocate(form: Form, mask: int, value: int, pools: dict) -> None:
    """Fixed host registers for every operand that names one, each the
    lowest its fields can encode that no other operand holds."""
    used = {"z": set(), "p": set(), "gpr": set()}
    choices = []
    for operand in form.operands:
        if operand.kind == "za_tile":
            continue
        bank = {"v": "z", "gpr_or_sp": "gpr"}.get(operand.kind, operand.kind)
        if operand.kind in ("gpr", "gpr_or_sp") and operand.restricted:
            pool = SLICE_REGISTERS
        else:
            pool = pools[bank]
        bits = operand.field_bits()
        fixed = mask & bits
        candidates = []
        for number in pool:
            encoded = operand.encode(number)
            if encoded is None or encoded & fixed != value & fixed:
                continue
            candidates.append(number)
        choices.append((len(candidates), operand, bank, candidates))
    for _, operand, bank, candidates in sorted(choices, key=lambda c: c[0]):
        size = 32 if bank == "z" else 16 if bank == "p" else 32
        for number in candidates:
            registers = [(number + i * operand.stride) % size
                         for i in range(operand.count)]
            if not used[bank] & set(registers):
                operand.hosts = registers
                used[bank] |= set(registers)
                break
        else:
            raise GenerationError(f"{form.name}: no host register for "
                                  f"{operand.kind} operand")


def host_fields(form: Form) -> int:
    word = 0
    for operand in form.operands:
        if operand.kind != "za_tile":
            word |= operand.encode(operand.hosts[0])
    return word


def size_of(word: int) -> int:
    return word >> 22 & 3


# Lowered SVE forms: the builder computes the immediates (lowering) into x5,
# x6 and x7, and the gadget runs the lines below. `computed` forms take
# their variant from the builder too, in the order of `variants`.

def placeholders(form: Form, size: int | None) -> dict:
    values = {}
    if size is not None:
        values["T"] = SIZES[size]
        values["R"] = "x" if size == 3 else "w"
    sources = {"z": [], "p": [], "gpr": []}
    for operand in form.operands:
        if operand.kind == "za_tile":
            continue
        bank = {"v": "z", "gpr_or_sp": "gpr"}.get(operand.kind, operand.kind)
        if operand.dest:
            values[bank[0] + "d"] = operand.register()
            if bank == "gpr":
                values["gdw"] = operand.register(view="w")
        else:
            sources[bank].append(operand)
    for bank, operands in sources.items():
        for index, operand in enumerate(operands):
            values[f"{bank[0]}s{index}"] = operand.register()
            if bank == "gpr":
                values[f"gs{index}w"] = operand.register(view="w")
                values[f"Rs{index}"] = operand.register(
                    view=values.get("R", "x"))
            if bank == "z" and operand.count > 1:
                values[f"zg{index}"] = ", ".join(
                    f"z{operand.hosts[i]}.{values.get('T', 'b')}"
                    for i in range(operand.count))
    return values


ARITH = {
    "add_z_zi_": ("arith_unsigned", ["add {zd}.{T}, {zd}.{T}, z31.{T}"]),
    "sub_z_zi_": ("arith_unsigned", ["sub {zd}.{T}, {zd}.{T}, z31.{T}"]),
    "subr_z_zi_": ("arith_unsigned", ["sub {zd}.{T}, z31.{T}, {zd}.{T}"]),
    "uqadd_z_zi_": ("arith_unsigned",
                    ["uqadd {zd}.{T}, {zd}.{T}, z31.{T}"]),
    "uqsub_z_zi_": ("arith_unsigned",
                    ["uqsub {zd}.{T}, {zd}.{T}, z31.{T}"]),
    "mul_z_zi_": ("arith_signed", ["mul {zd}.{T}, {zd}.{T}, z31.{T}"]),
    "smax_z_zi_": ("arith_signed",
                   ["ptrue p7.{T}", "smax {zd}.{T}, p7/m, {zd}.{T}, z31.{T}"]),
    "smin_z_zi_": ("arith_signed",
                   ["ptrue p7.{T}", "smin {zd}.{T}, p7/m, {zd}.{T}, z31.{T}"]),
    "umax_z_zi_": ("arith_byte",
                   ["ptrue p7.{T}", "umax {zd}.{T}, p7/m, {zd}.{T}, z31.{T}"]),
    "umin_z_zi_": ("arith_byte",
                   ["ptrue p7.{T}", "umin {zd}.{T}, p7/m, {zd}.{T}, z31.{T}"]),
}

COUNT_SCALAR = {
    "inc": "add {gd}, {gd}, x5", "dec": "sub {gd}, {gd}, x5",
}
SATURATING = {"sqinc": "sqadd", "sqdec": "sqsub", "uqinc": "uqadd",
              "uqdec": "uqsub"}
SHIFTS = {
    # name: (lowering, instruction, accumulate)
    "asr_z_zi_": ("shift_right", "asr", False),
    "lsr_z_zi_": ("shift_right", "lsr", False),
    "lsl_z_zi_": ("shift_left", "lsl", False),
    "asr_z_p_zi_": ("shift_right", "asr", False),
    "lsr_z_p_zi_": ("shift_right", "lsr", False),
    "lsl_z_p_zi_": ("shift_left", "lsl", False),
    "srshr_z_p_zi_": ("shift_right_negated", "srshl", False),
    "urshr_z_p_zi_": ("shift_right_negated", "urshl", False),
    "sqshl_z_p_zi_": ("shift_left", "sqshl", False),
    "uqshl_z_p_zi_": ("shift_left", "uqshl", False),
    "ssra_z_zi_": ("shift_right", "asr", True),
    "usra_z_zi_": ("shift_right", "lsr", True),
    "srsra_z_zi_": ("shift_right_negated", "srshl", True),
    "ursra_z_zi_": ("shift_right_negated", "urshl", True),
}
COMPARES = {
    "eq": ("cmpeq", False), "ne": ("cmpne", False), "gt": ("cmpgt", False),
    "ge": ("cmpge", False), "lt": ("cmpgt", True), "le": ("cmpge", True),
    "hi": ("cmphi", False), "hs": ("cmphs", False), "lo": ("cmphi", True),
    "ls": ("cmphs", True),
}


def raw_size_variants(form: Form, row, lines) -> None:
    """Variants over the free bits of the size field, bits 23:22."""
    form.variant_bits = [bit for bit in (22, 23) if not row.mask >> bit & 1]
    for value in range(1 << len(form.variant_bits)):
        size = size_of(row.value | scatter(value, form.variant_bits))
        values = placeholders(form, size)
        form.variants.append(Variant([line.format(**values)
                                      for line in lines]))


def row_variant(form: Form, size: int, lines: list) -> None:
    values = placeholders(form, size)
    if size < 3:
        values["W"] = SIZES[size + 1]
        values["RW"] = "x" if size == 2 else "w"
        values["mask"] = hex((1 << (8 << size)) - 1)
        values["half"] = str(8 << size)
    form.variants.append(Variant([line.format(**values) for line in lines]))


# The immediate shifts' rows split their tsz field by its lowest set bit,
# but the element size is its highest: one form serves every row of an
# instruction, with a variant per element size the builder selects.
SIZED_LOWERINGS = {"shift_right", "shift_left", "shift_right_negated",
                   "rotate_right"}


def size_variants(form: Form, sizes: int, lines: list) -> None:
    for size in range(sizes):
        row_variant(form, size, lines)


NARROWING = {
    # name: (shift lowering, wide instruction, extract or None)
    "shrnb": ("shift_right", "lsr", None),
    "rshrnb": ("shift_right_negated", "urshl", None),
    "sqshrnb": ("shift_right", "asr", "sqxtnb"),
    "sqrshrnb": ("shift_right_negated", "srshl", "sqxtnb"),
    "uqshrnb": ("shift_right", "lsr", "uqxtnb"),
    "uqrshrnb": ("shift_right_negated", "urshl", "uqxtnb"),
    "sqshrunb": ("shift_right", "asr", "sqxtunb"),
    "sqrshrunb": ("shift_right_negated", "srshl", "sqxtunb"),
}


def lower(form: Form, row) -> bool:
    """Set up a lowered form; False for a form the generic gadgets take."""
    name = row.encoding
    allocate(form, row.mask, row.value, LOWERED_POOLS)
    if name in ("and_z_zi_", "orr_z_zi_", "eor_z_zi_", "dupm_z_i_"):
        form.lowering, form.immediates = "logical_immediate", 1
        operation = name.split("_")[0]
        lines = ["dup {zd}.d, x5"] if operation == "dupm" else \
            ["dup z31.d, x5", f"{operation} {{zd}}.d, {{zd}}.d, z31.d"]
        values = placeholders(form, None)
        form.variants.append(Variant([l.format(**values) for l in lines]))
        return True
    if name in ("index_z_ii_", "index_z_ir_", "index_z_ri_"):
        form.lowering = {"ii": "index_immediates", "ir": "index_start",
                         "ri": "index_step"}[name.split("_")[2]]
        form.immediates = 2 if name == "index_z_ii_" else 1
        line = {"index_z_ii_": "index {zd}.{T}, {R}5, {R}6",
                "index_z_ir_": "index {zd}.{T}, {R}5, {Rs0}",
                "index_z_ri_": "index {zd}.{T}, {Rs0}, {R}5"}[name]
        raw_size_variants(form, row, [line])
        return True
    if name in ARITH:
        form.lowering, lines = ARITH[name]
        form.immediates = 1
        raw_size_variants(form, row, ["dup z31.{T}, {R}5"] + lines)
        return True
    if name in ("sqadd_z_zi_", "sqsub_z_zi_"):
        # Three parts that each fit a signed element: saturation is
        # monotonic, so adding or subtracting them in turn saturates
        # exactly where the one unsigned immediate does.
        form.lowering, form.immediates = "arith_saturating", 3
        operation = name.split("_")[0]
        raw_size_variants(form, row, [
            "dup z29.{T}, {R}5", "dup z30.{T}, {R}6", "dup z31.{T}, {R}7",
            f"{operation} {{zd}}.{{T}}, {{zd}}.{{T}}, z29.{{T}}",
            f"{operation} {{zd}}.{{T}}, {{zd}}.{{T}}, z30.{{T}}",
            f"{operation} {{zd}}.{{T}}, {{zd}}.{{T}}, z31.{{T}}"])
        return True
    if name in ("dup_z_i_", "fdup_z_i_"):
        form.lowering = "dup_immediate" if name == "dup_z_i_" else \
            "fp_immediate"
        form.immediates = 1
        raw_size_variants(form, row, ["dup {zd}.{T}, {R}5"])
        return True
    if name in ("cpy_z_p_i_", "fcpy_z_p_i_"):
        # Merging.
        form.lowering = "dup_immediate" if name == "cpy_z_p_i_" else \
            "fp_immediate"
        form.immediates = 1
        raw_size_variants(form, row, ["cpy {zd}.{T}, {ps0}/m, {R}5"])
        return True
    if name == "cpy_z_o_i_":
        # Zeroing.
        form.lowering, form.immediates = "dup_immediate", 1
        raw_size_variants(form, row, [
            "dup z31.{T}, {R}5", "dup z30.b, #0",
            "sel {zd}.{T}, {ps0}, z31.{T}, z30.{T}"])
        return True
    match = re.fullmatch(r"cmp(\w\w)_p_p_zi_", name)
    if match:
        instruction, swapped = COMPARES[match.group(1)]
        form.lowering = "compare_signed" if match.group(1) in (
            "eq", "ne", "gt", "ge", "lt", "le") else "compare_unsigned"
        form.immediates, form.nzcv = 1, "both"
        operands = "z31.{T}, {zs0}.{T}" if swapped else "{zs0}.{T}, z31.{T}"
        raw_size_variants(form, row, [
            "dup z31.{T}, {R}5",
            f"{instruction} {{pd}}.{{T}}, {{ps0}}/z, {operands}"])
        return True
    match = re.fullmatch(r"(cnt|inc|dec|sqinc|sqdec|uqinc|uqdec)([bhwd])_"
                         r"(r_s|r_rs|z_zs)_(\w*)", name)
    if match:
        operation, _, shape, suffix = match.groups()
        form.lowering, form.immediates = "element_count", 1
        size = size_of(row.value)
        values = placeholders(form, size)
        if operation == "cnt":
            lines = ["mov {gd}, x5"]
        elif shape == "z_zs":
            instruction = {"inc": "add", "dec": "sub"}.get(
                operation, SATURATING.get(operation))
            lines = ["dup z31.{T}, {R}5",
                     f"{instruction} {{zd}}.{{T}}, {{zd}}.{{T}}, z31.{{T}}"]
        elif operation in COUNT_SCALAR:
            lines = [COUNT_SCALAR[operation]]
        elif suffix == "x":
            lines = ["fmov d31, {gd}", "dup z30.d, x5",
                     f"{SATURATING[operation]} z31.d, z31.d, z30.d",
                     "fmov {gd}, d31"]
        else:
            lines = ["fmov s31, {gdw}", "dup z30.s, w5",
                     f"{SATURATING[operation]} z31.s, z31.s, z30.s",
                     "fmov {gdw}, s31"]
            if suffix == "sx":
                lines.append("sxtw {gd}, {gdw}")
        form.variants.append(Variant([l.format(**values) for l in lines]))
        return True
    if name in ("ptrue_p_s_", "ptrues_p_s_"):
        form.lowering, form.immediates = "predicate_count", 1
        lines = ["whilelo {pd}.{T}, xzr, x5"]
        if name == "ptrues_p_s_":
            lines.append("ptest {pd}, {pd}.b")
            form.nzcv = "write"
        raw_size_variants(form, row, lines)
        return True
    if name in ("ext_z_zi_des", "ext_z_zi_con"):
        form.lowering, form.immediates = "extract", 1
        values = placeholders(form, 0)
        tail = "splice {zd}.b, p7, {zd}.b, {zs0}.b" if name.endswith("des") \
            else "splice {zd}.b, p7, {{{zg0}}}"
        lines = ["ptrue p6.b", "whilelo p7.b, xzr, x5",
                 "eor p7.b, p6/z, p7.b, p6.b", tail]
        form.variants.append(Variant([l.format(**values) for l in lines]))
        return True
    if name == "dup_z_zi_":
        form.lowering, form.immediates = "broadcast_index", 1
        tsz = row.value >> 16 & 0x1F
        size = (tsz & -tsz).bit_length() - 1
        if size < 4:
            lines = ["dup z31.{T}, {R}5",
                     "tbl {zd}.{T}, {{{zs0}.{T}}}, z31.{T}"]
        else:
            # A quadword: doubleword indexes 2i, 2i + 1 repeated.
            lines = ["index z31.d, #0, #1", "and z31.d, z31.d, #1",
                     "dup z30.d, x5", "add z31.d, z31.d, z30.d",
                     "tbl {zd}.d, {{{zs0}.d}}, z31.d"]
        row_variant(form, min(size, 3), lines)
        return True
    if name in SHIFTS:
        form.lowering, instruction, accumulate = SHIFTS[name]
        form.immediates = 1
        predicated = "_p_" in name
        form.argument = 1 if predicated else 0
        if accumulate:
            lines = ["ptrue p7.{T}", "dup z31.{T}, {R}5",
                     "mov z30.d, {zs0}.d",
                     f"{instruction} z30.{{T}}, p7/m, z30.{{T}}, z31.{{T}}",
                     "add {zd}.{T}, {zd}.{T}, z30.{T}"]
        elif predicated:
            lines = ["dup z31.{T}, {R}5",
                     f"{instruction} {{zd}}.{{T}}, {{ps0}}/m, {{zd}}.{{T}}, "
                     "z31.{T}"]
        else:
            lines = ["ptrue p7.{T}", "dup z31.{T}, {R}5",
                     "mov {zd}.d, {zs0}.d",
                     f"{instruction} {{zd}}.{{T}}, p7/m, {{zd}}.{{T}}, "
                     "z31.{T}"]
        size_variants(form, 4, lines)
        return True
    match = re.fullmatch(r"(\w+?)(b|t)_z_zi_", name)
    if match and match.group(1) + "b" in NARROWING:
        # Shift the wide elements, then narrow: the saturating forms with
        # SVE2's extract-narrow, the others by masking the low halves
        # (bottom) or inserting them into the odd elements (top).
        form.lowering, wide, extract = NARROWING[match.group(1) + "b"]
        form.immediates, form.argument = 1, 2
        top = match.group(2) == "t"
        lines = ["ptrue p7.{W}", "dup z31.{W}, {RW}5", "mov z30.d, {zs0}.d",
                 f"{wide} z30.{{W}}, p7/m, z30.{{W}}, z31.{{W}}"]
        if extract:
            lines.append(f"{extract[:-1]}{match.group(2)} {{zd}}.{{T}}, "
                         "z30.{W}")
        elif top:
            lines += ["lsl z30.{W}, z30.{W}, #{half}",
                      "and {zd}.{W}, {zd}.{W}, #{mask}",
                      "orr {zd}.d, {zd}.d, z30.d"]
        else:
            lines += ["and z30.{W}, z30.{W}, #{mask}", "mov {zd}.d, z30.d"]
        size_variants(form, 3, lines)
        return True
    match = re.fullmatch(r"([su])shll(b|t)_z_zi_", name)
    if match:
        form.lowering, form.immediates, form.argument = "shift_left", 1, 2
        size_variants(form, 3, [
            f"{match.group(1)}shll{match.group(2)} {{zd}}.{{W}}, "
            "{zs0}.{T}, #0",
            "ptrue p7.{W}", "dup z31.{W}, {RW}5",
            "lsl {zd}.{W}, p7/m, {zd}.{W}, z31.{W}"])
        return True
    if name in ("sri_z_zzi_", "sli_z_zzi_"):
        # The shifted source where a shifted all-ones mask is set, the
        # destination elsewhere.
        right = name.startswith("sri")
        form.lowering = "shift_right" if right else "shift_left"
        form.immediates = 1
        instruction = "lsr" if right else "lsl"
        size_variants(form, 4, [
            "ptrue p7.{T}", "dup z31.{T}, {R}5", "mov z30.d, {zs0}.d",
            f"{instruction} z30.{{T}}, p7/m, z30.{{T}}, z31.{{T}}",
            "dup z29.b, #-1",
            f"{instruction} z29.{{T}}, p7/m, z29.{{T}}, z31.{{T}}",
            "bsl z30.d, z30.d, {zd}.d, z29.d", "mov {zd}.d, z30.d"])
        return True
    if name == "xar_z_zzi_":
        # Rotate right by the shift: x >> shift | x << (esize - shift).
        form.lowering, form.immediates = "rotate_right", 2
        size_variants(form, 4, [
            "eor z30.d, {zd}.d, {zs0}.d", "ptrue p7.{T}",
            "mov {zd}.d, z30.d", "dup z31.{T}, {R}5",
            "lsr z30.{T}, p7/m, z30.{T}, z31.{T}", "dup z31.{T}, {R}6",
            "lsl {zd}.{T}, p7/m, {zd}.{T}, z31.{T}",
            "orr {zd}.d, {zd}.d, z30.d"])
        return True
    if name == "movprfx_z_z_":
        values = placeholders(form, None)
        form.variants.append(Variant(["mov {zd}.d, {zs0}.d".format(**values)]))
        form.lowering = "plain"
        return True
    if name == "movprfx_z_p_z_":
        form.lowering = "plain"
        form.variant_bits = [bit for bit in (16, 22, 23)
                             if not row.mask >> bit & 1]
        for value in range(1 << len(form.variant_bits)):
            word = row.value | scatter(value, form.variant_bits)
            values = placeholders(form, size_of(word))
            if word >> 16 & 1:
                lines = ["sel {zd}.{T}, {ps0}, {zs0}.{T}, {zd}.{T}"]
            else:
                lines = ["dup z31.b, #0",
                         "sel {zd}.{T}, {ps0}, {zs0}.{T}, z31.{T}"]
            form.variants.append(Variant([l.format(**values) for l in lines]))
        return True
    return False


# The control core takes these rows in program_builder_sme.cpp.
CONTROL_CORE = {"rdsvl_r_i_", "addsvl_r_ri_", "addspl_r_ri_", "zero_za_i_",
                "rdvl_r_i_", "addvl_r_ri_", "addpl_r_ri_"}


def sve_forms(table) -> list:
    forms = []
    sized = {}
    for row in table.rows:
        if not row.in_profile or row.memory is not None or \
                not row.streaming or "sme2" in row.requirement or \
                row.encoding in CONTROL_CORE:
            continue
        if row.encoding in sized:
            sized[row.encoding].indices.append(row.index)
            continue
        form = Form("sve", row.index, row.encoding, table_operands(row))
        if lower(form, row):
            if form.lowering in SIZED_LOWERINGS:
                sized[row.encoding] = form
        else:
            for operand in form.operands:
                operand.hosts = []
            allocate(form, row.mask, row.value, GENERIC_POOLS)
            generic(form, row.mask, row.value, set())
        form.fp = bool(FP_EXCEPTION_NAME.match(row.encoding)) and \
            not row.stable_id.startswith("SME-")
        dests = [o for o in form.operands if o.dest]
        if len(dests) > 1:
            raise GenerationError(f"{row.encoding}: several destinations")
        if not form.nzcv and form.lowering == "none":
            if dests and dests[0].kind == "p":
                form.nzcv = "both"
            elif row.encoding in NZCV_ONLY:
                form.nzcv = "both"
            elif not dests and not row.za:
                raise GenerationError(f"{row.encoding}: no destination")
        forms.append(form)
    return forms


def generic(form: Form, mask: int, value: int, ignored: set) -> None:
    """Variants over every free bit that is not a register field."""
    register_bits = 0
    for operand in form.operands:
        if operand.kind != "za_tile":
            register_bits |= operand.field_bits()
    ignored_bits = 0
    for bit in ignored:
        ignored_bits |= 1 << bit
    free = ~mask & 0xFFFFFFFF & ~register_bits & ~ignored_bits
    form.variant_bits = [bit for bit in range(32) if free >> bit & 1]
    hosts = host_fields(form)
    for variant in range(1 << len(form.variant_bits)):
        word = value | scatter(variant, form.variant_bits) | hosts
        if word & mask != value:
            raise GenerationError(f"{form.name}: host word {word:08x} "
                                  "leaves its row")
        for operand in form.operands:
            if operand.kind != "za_tile" and \
                    operand.decode(word) != operand.hosts[0]:
                raise GenerationError(f"{form.name}: host word {word:08x} "
                                      "names the wrong register")
        form.variants.append(Variant([f".inst 0x{word:08x}"], word))


# Register-only FP&SIMD classes that execute in streaming mode.

def V(low, dest=False):
    return Operand("v", (register_field(low, 5),), dest=dest)


def G(low, dest=False):
    return Operand("gpr", (register_field(low, 5),), dest=dest)


def fp_operands(name: str) -> tuple[list, set, str]:
    """Operands, ignored bits and NZCV use of an FP&SIMD class."""
    instruction, _, rest = name.partition("_")
    shape = rest.rsplit("_", 1)[0] if rest.count("_") else ""
    if name.endswith("floatdp1") or "asisdmisc" in name:
        return [V(0, True), V(5)], set(), ""
    if name.endswith("floatdp2") or "asisdsame" in name:
        return [V(0, True), V(5), V(16)], set(), ""
    if name.endswith("floatdp3"):
        return [V(0, True), V(5), V(16), V(10)], set(), ""
    if name.endswith("floatcmp"):
        if shape.endswith("Z"):
            return [V(5)], set(range(16, 21)), "write"
        return [V(5), V(16)], set(), "write"
    if name.endswith("floatsel"):
        return [V(0, True), V(5), V(16)], set(), "read"
    if name.endswith("floatimm"):
        # Native for every immediate: with FPCR.NEP set, FEAT_AFP decides
        # what the result keeps of the destination's other bits.
        return [V(0, True)], set(), ""
    if name.endswith("floatccmp"):
        return None, set(), ""
    if name.endswith("float2int") or name.endswith("float2fix"):
        if instruction in ("SCVTF", "UCVTF"):
            return [V(0, True), G(5)], set(), ""
        if instruction == "FMOV":
            destination = shape[0]
            if destination.isdigit():
                return [G(0, True), V(5)], set(), ""
            return [V(0, True), G(5)], set(), ""
        return [G(0, True), V(5)], set(), ""
    if "asimdins" in name:
        return [G(0, True), V(5)], set(), ""
    raise GenerationError(f"{name}: unknown FP&SIMD class shape")


MEMORY_CLASS = re.compile(r"ldst|loadlit|^LDF|^STF|^LDBF|^STBF")
FIXED_TO_FLOAT = re.compile(r"([SU]CVTF)_([SD])(32|64)_float2fix")


def fp_forms(table) -> list:
    forms = []
    shared = {}
    for row in table.fp_simd:
        if row.advsimd or MEMORY_CLASS.search(row.encoding):
            continue
        match = FIXED_TO_FLOAT.fullmatch(row.encoding)
        if match:
            # SCVTF and UCVTF to single or double precision with fbits:
            # the integer conversion rounds once, and scaling by the exact
            # power of two 2^-fbits stays normal (at least 2^-64), so the
            # product is the fixed-point conversion, flags included. The
            # 32-bit source's 32 scale rows share one gadget.
            if match.groups() in shared:
                shared[match.groups()].indices.append(row.index)
                continue
            instruction, precision, width = match.groups()
            form = Form("fp", row.index, row.encoding, [V(0, True), G(5)])
            allocate(form, row.mask, row.value, LOWERED_POOLS)
            form.lowering, form.immediates, form.fp = \
                "fp_fixed_to_float", 1, True
            form.argument = 1 if precision == "D" else 0
            low = precision.lower()
            destination = form.operands[0].register(view=low)
            source = form.operands[1].register(
                view="w" if width == "32" else "x")
            scale = "x5" if precision == "D" else "w5"
            form.variants.append(Variant([
                f"{instruction.lower()} {destination}, {source}",
                f"fmov {low}31, {scale}",
                f"fmul {destination}, {destination}, {low}31"]))
            shared[match.groups()] = form
        elif row.encoding.endswith("floatccmp"):
            form = Form("fp", row.index, row.encoding, [V(5), V(16)])
            allocate(form, row.mask, row.value, LOWERED_POOLS)
            form.lowering, form.immediates = "fp_conditional_compare", 1
            form.fp, form.nzcv = True, "custom"
            form.variant_bits = list(range(12, 16))
            for condition in range(16):
                word = row.value | condition << 12 | host_fields(form)
                # FCMP or FCMPE (bit 4) of the same type and registers.
                compare = 0x1E202000 | (row.value & 0x00C00000) | \
                    (form.operands[1].hosts[0] << 16) | \
                    (form.operands[0].hosts[0] << 5) | \
                    (row.value >> 4 & 1) << 4
                variant = Variant([], word)
                variant.condition = condition
                variant.compare = compare
                form.variants.append(variant)
        else:
            operands, ignored, nzcv = fp_operands(row.encoding)
            form = Form("fp", row.index, row.encoding, operands)
            allocate(form, row.mask, row.value, GENERIC_POOLS)
            generic(form, row.mask, row.value, ignored)
            form.nzcv = nzcv
            form.fp = bool(FP_EXCEPTION_NAME.match(row.encoding))
        forms.append(form)
    return forms


# Assembly.

def offset_register(slot: int, destination: bool, general: bool) -> list:
    """Lines that leave the slot's stream offset (general) or register
    file address in x10/x17 for a destination, x11 otherwise."""
    target = ("x17" if general else "x10") if destination else "x11"
    if slot == 0:
        if general:
            return [f"and {target}, x8, #0xffff"]
        return [f"add {target}, x9, w8, uxth"]
    lines = [f"ubfx {target}, x8, #{16 * slot}, #16"]
    if not general:
        lines.append(f"add {target}, x9, {target}")
    return lines


def emit_variant(form: Form, variant: Variant, label: str) -> list:
    slots = form.slots
    if len(slots) > 4:
        raise GenerationError(f"{form.name}: {len(slots)} operand slots")
    out = ["\t.p2align 4", f"{label}:"]
    if slots:
        out.append("\tldr x8, [_stream], #8")
    if any(operand.file for operand, _ in slots):
        out.append("\tldr x9, [_vcpu, #UML_ARM64_CPU_SME_REGISTERS]")
    if form.immediates == 1:
        out.append("\tldr x5, [_stream], #8")
    elif form.immediates == 2:
        out.append("\tldp x5, x6, [_stream], #16")
    elif form.immediates == 3:
        out.append("\tldp x5, x6, [_stream], #16")
        out.append("\tldr x7, [_stream], #8")
    loads, stores = [], []
    for slot, (operand, index) in enumerate(slots):
        general = not operand.file
        destination = operand.dest
        lines = offset_register(slot, destination, general)
        address = lines[0].split()[1].rstrip(",")
        register = operand.register(index)
        if general:
            lines.append(f"ldr {register}, [_vcpu, {address}]")
            if destination:
                stores.append(f"str {register}, [_vcpu, {address}]")
                if operand.kind == "gpr":
                    stores.append("str xzr, [_vcpu, #UML_GADGET_CPU_ZERO]")
        else:
            lines.append(f"ldr {register}, [{address}]")
            if destination:
                stores.append(f"str {register}, [{address}]")
        loads.extend(lines)
    if form.nzcv == "custom":
        out.extend("\t" + line for line in loads)
        out.extend("\t" + line for line in conditional_compare(variant))
        out.extend(["\tldr x8, [_stream], #8", "\tbr x8"])
        return out
    body = []
    if form.fp:
        body.append("1:")
        body.append("msr fpsr, xzr")
    body.extend(loads)
    if form.nzcv in ("read", "both"):
        body.append("msr nzcv, x27")
    body.extend(variant.lines)
    if form.nzcv in ("write", "both"):
        body.append("mrs x27, nzcv")
    if form.fp:
        body.append("mrs x11, fpsr")
        body.append("tbnz w11, #27, 1b")
    body.extend(stores)
    if form.fp:
        body.append("ldr w16, [_vcpu, #UML_GADGET_CPU_FPSR]")
        body.append("orr w16, w16, w11")
        body.append("str w16, [_vcpu, #UML_GADGET_CPU_FPSR]")
    out.extend(("\t" + line) if not line.endswith(":") else line
               for line in body)
    out.extend(["\tldr x8, [_stream], #8", "\tbr x8"])
    return out


def conditional_compare(variant: Variant) -> list:
    """FCCMP and FCCMPE: the comparison where the condition holds on the
    guest's flags, the stream's NZCV otherwise."""
    condition = variant.condition
    lines = ["msr nzcv, x27"]
    if condition >= 14:
        lines.append("b 1f")
    else:
        # B.cond to 1f: three words ahead.
        lines.append(f".inst 0x{0x54000060 | condition:08x}")
    lines += ["mov x27, x5", "b 2f",
              "1:", "msr fpsr, xzr", f".inst 0x{variant.compare:08x}",
              "mrs x11, fpsr", "tbnz w11, #27, 1b", "mrs x27, nzcv",
              "ldr w16, [_vcpu, #UML_GADGET_CPU_FPSR]", "orr w16, w16, w11",
              "str w16, [_vcpu, #UML_GADGET_CPU_FPSR]", "2:"]
    return [line if line.endswith(":") else line for line in lines]


LOWERINGS = ["none", "plain", "logical_immediate", "index_immediates",
             "index_start", "index_step", "arith_unsigned", "arith_signed",
             "arith_byte", "arith_saturating", "dup_immediate",
             "fp_immediate", "compare_signed", "compare_unsigned",
             "element_count", "predicate_count", "extract",
             "broadcast_index", "shift_right", "shift_left",
             "shift_right_negated", "rotate_right",
             "fp_conditional_compare", "fp_fixed_to_float"]


def render_assembly(forms: list) -> str:
    out = ["/* SPDX-License-Identifier: GPL-2.0-or-later */",
           "/* Generated by darwin/cpu/arm64/gadgets/engine/"
           "generate_streaming.py. Do not edit. */",
           "\t.p2align 4",
           "\t.globl CSYM(uml_arm64_gadget_sm_generated_begin)",
           "CSYM(uml_arm64_gadget_sm_generated_begin):"]
    labels = []
    for number, form in enumerate(forms):
        out.append(f"/* {form.source} {form.index} {form.name} */")
        for index, variant in enumerate(form.variants):
            label = f"Lsm_generated_{number}_{index}"
            labels.append(label)
            out.extend(emit_variant(form, variant, label))
    out += ["\t.p2align 4",
            "\t.globl CSYM(uml_arm64_gadget_sm_generated_end)",
            "CSYM(uml_arm64_gadget_sm_generated_end):",
            "",
            ".pushsection __DATA_CONST,__gadget_sme",
            "\t.p2align 3",
            "\t.globl CSYM(uml_arm64_gadget_sm_generated_entries)",
            "CSYM(uml_arm64_gadget_sm_generated_entries):"]
    out.extend(f"\t.quad {label}" for label in labels)
    out += ["\t.globl CSYM(uml_arm64_gadget_sm_generated_entries_end)",
            "CSYM(uml_arm64_gadget_sm_generated_entries_end):",
            ".popsection", ""]
    return "\n".join(out)


def segments_text(operand: Operand) -> str:
    parts = []
    for segment in operand.segments:
        if segment.field:
            parts.append(f"detail::sme_field({segment.low}, {segment.width})")
        else:
            parts.append(f"detail::sme_constant({segment.width}, "
                         f"{segment.constant})")
    return ", ".join(parts)


def render_header(forms: list, table) -> str:
    fp_operands = []
    form_lines = []
    entry = 0
    row_forms = ["0xffff"] * len(table.rows)
    class_forms = ["0xffff"] * len(table.fp_simd)
    for number, form in enumerate(forms):
        mask = 0
        for bit in form.variant_bits:
            mask |= 1 << bit
        if form.source == "fp":
            begin = len(fp_operands)
            fp_operands.extend(form.operands)
            for index in [form.index] + form.indices:
                class_forms[index] = str(number)
        else:
            begin = 0
            for index in [form.index] + form.indices:
                row_forms[index] = str(number)
        form_lines.append(
            f"\tForm{{0x{mask:08x}U, {entry}U, {begin}U, "
            f"{len(form.operands)}U, "
            f"{'Source::fp_class' if form.source == 'fp' else 'Source::sve_row'}, "
            f"Lowering::{form.lowering}, {form.argument}U, "
            f"{len(form.variants)}U}}, // {form.name}")
        entry += len(form.variants)
    out = ["// SPDX-License-Identifier: GPL-2.0-or-later",
           "// Generated by darwin/cpu/arm64/gadgets/engine/"
           "generate_streaming.py. Do not edit.",
           "#pragma once", "",
           "#include <array>", "#include <cstdint>", "",
           '#include "cpu/arm64/detail/sme_encoding_types.hpp"', "",
           "namespace uml::cpu::arm64::gadgets::streaming {", "",
           "enum class Lowering : std::uint8_t {"]
    out.extend(f"\t{name}," for name in LOWERINGS)
    out += ["};", "",
            "enum class Source : std::uint8_t { sve_row, fp_class };", "",
            "/* One generated form: its variants are entries",
            " * [first_entry, first_entry + variants) of",
            " * uml_arm64_gadget_sm_generated_entries, selected by the word's",
            " * variant_mask bits in order (or, for a shift, by its element",
            " * size), and its",
            " * operands are the row's in the encoding table or",
            " * fp_operands[operand_begin, + operand_count). */",
            "struct Form final {",
            "\tstd::uint32_t variant_mask;",
            "\tstd::uint32_t first_entry;",
            "\tstd::uint16_t operand_begin;",
            "\tstd::uint8_t operand_count;",
            "\tSource source;",
            "\tLowering lowering;",
            "\tstd::uint8_t argument;",
            "\tstd::uint16_t variants;",
            "};", "",
            f"inline constexpr std::uint32_t entry_count = {entry};",
            "inline constexpr std::uint16_t no_form = 0xffff;", ""]
    out.append(f"inline constexpr std::array<detail::SmeOperand, "
               f"{max(1, len(fp_operands))}> fp_operands{{{{")
    for operand in fp_operands or [Operand("v", (register_field(0, 5),))]:
        kind = operand.kind
        out.append(f"\tdetail::SmeOperand{{detail::SmeOperandKind::{kind}, "
                   f"{operand.count}, {operand.stride}, "
                   f"{len(operand.segments)}, {{{{{segments_text(operand)}}}}}}},")
    out.append("}};")
    out.append("")
    out.append(f"inline constexpr std::array<Form, {len(forms)}> forms{{{{")
    out.extend(form_lines)
    out.append("}};")
    out.append("")
    for name, values in (("sve_row_forms", row_forms),
                         ("fp_class_forms", class_forms)):
        out.append(f"inline constexpr std::array<std::uint16_t, "
                   f"{len(values)}> {name}{{{{")
        for start in range(0, len(values), 12):
            out.append("\t" + ", ".join(values[start:start + 12]) + ",")
        out.append("}};")
        out.append("")
    out.append("} // namespace uml::cpu::arm64::gadgets::streaming")
    out.append("")
    return "\n".join(out)


# --------------------------------------------------------------------------
# Memory

MEMORY_LETTERS = {8: "b", 16: "h", 32: "w", 64: "d", 128: "q"}
ELEMENT_LETTERS = {8: "b", 16: "h", 32: "s", 64: "d", 128: "q"}
FP_LETTERS = {8: "b", 16: "h", 32: "s", 64: "d", 128: "q"}
# The shapes the vector memory gadgets take (sme_layout.h). Prefetches
# need no gadget; LDR and STR of ZA and ZT0 are the control core's.
MEMORY_SHAPES = {"contiguous": 0, "structure": 1, "replicate": 2,
                 "replicate_quad": 3, "fill_z": 4, "fill_p": 5,
                 "za_slice": 6}
FP_SINGLE_SIZES = (8, 16, 32, 64, 128)
FP_PAIR_SIZES = (32, 64, 128)


@dataclass(frozen=True)
class MemoryKind:
    """One vector memory gadget: the instruction the host runs."""
    shape: str
    store: bool
    msize: int
    esize: int
    sign: bool = False
    registers: int = 1
    nontemporal: bool = False
    tile: int = 0
    vertical: bool = False

    @property
    def name(self) -> str:
        if self.shape == "fp":
            pair = "p" if self.registers == 2 else "r"
            return (f"mem_fp_{'st' if self.store else 'ld'}{pair}_"
                    f"{FP_LETTERS[self.msize]}")
        return "mem_" + self.native().split()[0].replace(".", "_") + (
            "" if self.shape in ("fill_z", "fill_p", "za_slice") else
            "_" + ELEMENT_LETTERS[self.esize]) + (
            "_p" if self.shape == "fill_p" else "") + (
            f"_za{self.tile}{'v' if self.vertical else 'h'}"
            if self.shape == "za_slice" else "")

    def native(self) -> str:
        """The instruction on host registers: base x7, no offset."""
        op = "st" if self.store else "ld"
        letter = MEMORY_LETTERS[self.msize]
        element = ELEMENT_LETTERS[self.esize]
        governed = "p0" if self.store else "p0/z"
        if self.shape == "fp":
            reg = FP_LETTERS[self.msize]
            if self.registers == 2:
                return f"{op}p {reg}0, {reg}1, [x7]"
            return f"{op}r {reg}0, [x7]"
        if self.shape == "fill_z":
            return f"{op}r z0, [x7]"
        if self.shape == "fill_p":
            return f"{op}r p0, [x7]"
        if self.shape == "za_slice":
            hv = "v" if self.vertical else "h"
            return (f"{op}1{letter} {{za{self.tile}{hv}.{element}[w12, 0]}}, "
                    f"{governed}, [x7]")
        if self.shape == "contiguous":
            mnemonic = f"{op}nt1{letter}" if self.nontemporal else \
                f"{op}1{'s' if self.sign else ''}{letter}"
        elif self.shape == "structure":
            mnemonic = f"{op}{self.registers}{letter}"
        elif self.shape == "replicate":
            mnemonic = f"ld1r{'s' if self.sign else ''}{letter}"
        else:
            mnemonic = f"ld1rq{letter}"
        group = ", ".join(f"z{k}.{element}" for k in range(self.registers))
        return f"{mnemonic} {{{group}}}, {governed}, [x7]"

    def transfers(self) -> list:
        """Each transfer register and the stream field of its offset."""
        if self.shape == "fill_p":
            return [("p0", 0)]
        if self.shape == "za_slice":
            return []
        return [(f"z{k}", k) for k in range(self.registers)]


def stream_field(field: int, target: str = "x8") -> str:
    if field == 0:
        return f"and {target}, x16, #0xffff"
    return f"ubfx {target}, x16, #{16 * field}, #16"


def emit_memory(kind: MemoryKind) -> list:
    body = []
    if kind.shape == "za_slice":
        body += [stream_field(0), "ldr w12, [_vcpu, x8]", stream_field(1),
                 "add w12, w12, w8"]
    if kind.shape not in ("fill_z", "fill_p", "fp"):
        body += ["and x8, x17, #0xffff", "add x8, x9, x8", "ldr p0, [x8]"]
    if kind.store:
        for register, field in kind.transfers():
            body += [stream_field(field), "add x8, x9, x8",
                     f"ldr {register}, [x8]"]
    body.append(kind.native())
    if not kind.store:
        for register, field in kind.transfers():
            body += [stream_field(field), "add x8, x9, x8",
                     f"str {register}, [x8]"]
    if kind.shape == "fp":
        body.append("sm_vector_writeback")
    out = [f"/* {kind.native()} */", f"gadget {kind.name}",
           "\tldp x14, x15, [_stream]",
           f"\tsm_vector_address {int(kind.store)}, 8f",
           "\tldp x16, x17, [_stream, #16]",
           "\tldr x9, [_vcpu, #UML_ARM64_CPU_SME_REGISTERS]"]
    out += ["\t" + line for line in body]
    out += ["\tadd _stream, _stream, #40", "\tnext_gadget", "8:",
            "\tsm_vector_slow"]
    return out


@dataclass
class MemoryForm:
    """A memory row's gadgets: entries [first, first + variants) of
    uml_arm64_gadget_sm_memory_entries, selected by its element size
    field (less base) or a ZA slice's tile and direction."""
    name: str
    first: int
    variants: int
    variant: str
    base: int = 0


def field_values(row, segment) -> list:
    """The values of a field the row's words can hold."""
    field_mask = ((1 << segment.width) - 1) << segment.low
    return [value for value in range(1 << segment.width)
            if ((value << segment.low) ^ row.value) & row.mask &
            field_mask == 0]


def memory_rows(table) -> list:
    return [row for row in table.rows
            if row.in_profile and row.memory is not None and
            "sme2" not in row.requirement and
            row.memory.shape in MEMORY_SHAPES and
            not (row.memory.shape == "contiguous" and
                 row.memory.registers != 1)]


def row_esizes(row) -> list:
    """The element sizes, in bits, the row's words can name."""
    memory = row.memory
    if memory.esize_shift is None:
        return [memory.esize]
    return [memory.esize << value
            for value in field_values(row, memory.esize_shift)]


def memory_kinds(table) -> tuple[list, dict, list, list]:
    """Every gadget, each memory row's form, and the SIMD&FP single and
    pair entries ([size][store]). The gadgets of one instruction family
    (shape, direction, memory size, extension, registers) are adjacent, one
    per element size from the smallest any row needs to the largest, so a
    row whose size field selects among them takes field - base from its
    first."""
    kinds: list[MemoryKind] = []
    family_esizes: dict[tuple, set] = {}
    for row in memory_rows(table):
        memory = row.memory
        if memory.shape == "za_slice":
            continue
        key = (memory.shape, memory.direction == "store", memory.msize,
               memory.sign_extend, memory.registers, memory.nontemporal)
        family_esizes.setdefault(key, set()).update(row_esizes(row))
    family_first: dict[tuple, int] = {}
    for key, esizes in family_esizes.items():
        family_first[key] = len(kinds)
        shape, store, msize, sign, registers, nontemporal = key
        esize = min(esizes)
        while esize <= max(esizes):
            kinds.append(MemoryKind(shape, store, msize, esize, sign,
                                    registers, nontemporal))
            esize *= 2
    forms: dict[int, MemoryForm] = {}
    slices: dict[tuple, int] = {}
    for row in memory_rows(table):
        memory = row.memory
        store = memory.direction == "store"
        if memory.shape == "za_slice":
            key = (store, memory.msize)
            if key not in slices:
                slices[key] = len(kinds)
                kinds.extend(MemoryKind("za_slice", store, memory.msize,
                                        memory.esize, tile=tile,
                                        vertical=bool(vertical))
                             for tile in range(memory.esize // 8)
                             for vertical in (0, 1))
            forms[row.index] = MemoryForm(row.encoding, slices[key],
                                          memory.esize // 4, "za_slice")
            continue
        key = (memory.shape, store, memory.msize, memory.sign_extend,
               memory.registers, memory.nontemporal)
        smallest = min(family_esizes[key])
        esizes = row_esizes(row)
        first = family_first[key] + (min(esizes) // smallest).bit_length() - 1
        if memory.esize_shift is None:
            forms[row.index] = MemoryForm(row.encoding, first, 1, "none")
        else:
            values = field_values(row, memory.esize_shift)
            forms[row.index] = MemoryForm(row.encoding, first,
                                          values[-1] - values[0] + 1,
                                          "element_size", values[0])
    single = []
    for size in FP_SINGLE_SIZES:
        for store in (False, True):
            single.append(len(kinds))
            kinds.append(MemoryKind("fp", store, size, size))
    pair = []
    for size in FP_PAIR_SIZES:
        for store in (False, True):
            pair.append(len(kinds))
            kinds.append(MemoryKind("fp", store, size, size, registers=2))
    names = [kind.name for kind in kinds]
    if len(set(names)) != len(names):
        raise GenerationError("two memory gadgets share a name")
    return kinds, forms, single, pair


def render_memory_assembly(kinds: list) -> str:
    out = ["/* SPDX-License-Identifier: GPL-2.0-or-later */",
           "/* Generated by darwin/cpu/arm64/gadgets/engine/"
           "generate_streaming.py. Do not edit. */",
           "\t.p2align 4",
           "\t.globl CSYM(uml_arm64_gadget_sm_memory_begin)",
           "CSYM(uml_arm64_gadget_sm_memory_begin):"]
    for kind in kinds:
        out.extend(emit_memory(kind))
    out += ["\t.p2align 4",
            "\t.globl CSYM(uml_arm64_gadget_sm_memory_end)",
            "CSYM(uml_arm64_gadget_sm_memory_end):",
            "",
            ".pushsection __DATA_CONST,__gadget_sme",
            "\t.p2align 3",
            "\t.globl CSYM(uml_arm64_gadget_sm_memory_entries)",
            "CSYM(uml_arm64_gadget_sm_memory_entries):"]
    out.extend(f"\t.quad CSYM(uml_arm64_gadget_sm_{kind.name})"
               for kind in kinds)
    out += ["\t.globl CSYM(uml_arm64_gadget_sm_memory_entries_end)",
            "CSYM(uml_arm64_gadget_sm_memory_entries_end):",
            ".popsection", ""]
    return "\n".join(out)


def render_memory_header(kinds: list, forms: dict, single: list,
                         pair: list, table) -> list:
    numbered = sorted(forms.items())
    row_forms = ["0xffff"] * len(table.rows)
    for number, (row_index, _form) in enumerate(numbered):
        row_forms[row_index] = str(number)
    out = ["/* A memory row's gadgets: entries [first_entry, first_entry +",
           " * variants) of uml_arm64_gadget_sm_memory_entries, selected by",
           " * the row's element size field less base, or by a ZA slice's",
           " * tile x 2 + 1 for a vertical slice. */",
           "enum class MemoryVariant : std::uint8_t {",
           "\tnone,", "\telement_size,", "\tza_slice,", "};", "",
           "struct MemoryForm final {",
           "\tstd::uint16_t first_entry;",
           "\tstd::uint8_t variants;",
           "\tMemoryVariant variant;",
           "\tstd::uint8_t base;",
           "};", "",
           f"inline constexpr std::uint32_t memory_entry_count = "
           f"{len(kinds)};", "",
           f"inline constexpr std::array<MemoryForm, {len(numbered)}> "
           f"memory_forms{{{{"]
    for _row_index, form in numbered:
        out.append(f"\tMemoryForm{{{form.first}U, {form.variants}U, "
                   f"MemoryVariant::{form.variant}, {form.base}U}}, "
                   f"// {form.name}")
    out += ["}};", ""]
    out.append(f"inline constexpr std::array<std::uint16_t, "
               f"{len(row_forms)}> memory_row_forms{{{{")
    for start in range(0, len(row_forms), 12):
        out.append("\t" + ", ".join(row_forms[start:start + 12]) + ",")
    out += ["}};", "",
            "/* The SIMD&FP entries, [size][store]: single registers of 1,",
            " * 2, 4, 8 and 16 bytes, pairs of 4, 8 and 16. */",
            f"inline constexpr std::array<std::uint16_t, {len(single)}> "
            f"fp_memory_single{{{{" +
            ", ".join(f"{value}U" for value in single) + "}};",
            f"inline constexpr std::array<std::uint16_t, {len(pair)}> "
            f"fp_memory_pair{{{{" +
            ", ".join(f"{value}U" for value in pair) + "}};", ""]
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--loader", type=Path, required=True)
    parser.add_argument("--table", type=Path, required=True)
    parser.add_argument("--assembly", type=Path, required=True)
    parser.add_argument("--memory-assembly", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    arguments = parser.parse_args()
    table = load_table(arguments.loader, arguments.table)
    forms = sve_forms(table) + fp_forms(table)
    kinds, memory_forms, single, pair = memory_kinds(table)
    header = render_header(forms, table).replace(
        "} // namespace uml::cpu::arm64::gadgets::streaming\n",
        "\n".join(render_memory_header(kinds, memory_forms, single, pair,
                                       table)) +
        "\n} // namespace uml::cpu::arm64::gadgets::streaming\n")
    for path, text in ((arguments.assembly, render_assembly(forms)),
                       (arguments.memory_assembly,
                        render_memory_assembly(kinds)),
                       (arguments.header, header)):
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GenerationError as error:
        print(f"generate_streaming.py: {error}", file=sys.stderr)
        sys.exit(1)
