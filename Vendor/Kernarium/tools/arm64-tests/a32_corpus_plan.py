#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Plan the cases of the AArch32 instruction corpus.

The planner turns the decoder's encoding table into cases for the oracle: for
each inventory entry it draws words from the table's stored samples and from
fills of its index rows, keeps those the Python reference decoder accepts,
and pairs each with starting states that make the instruction do something:
edge values, patterned values, flag combinations, and for memory forms base
registers that point into the corpus's data region. It decides nothing about
results; the oracle does that.

The suites are:

- alu: data processing, multiplies, bit fields, extension, saturation, packing,
  parallel arithmetic and the other register-only base instructions;
- memory: single and multiple loads and stores, exclusives and prefetch, at
  aligned and misaligned addresses, across a page, and into read-only and
  unmapped memory;
- branch: branches, interworking and PC writes, aimed at the exit stubs;
- conditions: every condition against every flag combination, and IT blocks;
- system: barriers, hints, CPS, the APSR, the permitted coprocessor registers.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import pathlib
import random
import sys
import tempfile
from dataclasses import dataclass

TOOLING = pathlib.Path(__file__).resolve().parents[2] / "tests/tooling/arm64"
sys.path.insert(0, str(TOOLING))

import a32_architecture_oracle as oracle  # noqa: E402
import a32_expected as expected  # noqa: E402
import a32_llvm_data as data  # noqa: E402
import a32_profile_policy as policy  # noqa: E402
import a32_table_model as tm  # noqa: E402
from a32_corpus_wire import Case, corpus  # noqa: E402

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
SAMPLES = REPOSITORY / "tests/unit/arm64/aarch32_decoder/samples.tsv"
SEED = "a32-corpus-v1"
FEATURES = set(policy.FEATURE_ORDER)
ISA_LETTER = {"A32": "A", "T16": "T", "T32": "T"}

# Flag values the cases start in: none, each of N Z C V, a mixed pair, all,
# Q, GE patterns, and everything.
FLAGS = (
    0x00000000, 0x80000000, 0x40000000, 0x20000000, 0x10000000, 0x60000000,
    0xA0000000, 0xF0000000, 0x08000000, 0x000F0000, 0x00050000, 0xF80F0000,
)


@dataclass
class Context:
    model: tm.Model
    reference: expected.Reference
    samples: dict[str, list[tuple[str, int]]]  # form -> [(isa, word)]
    recipes: dict[str, dict[str, int]]


# State recipes ---------------------------------------------------------------------


def pattern(seed: int, bits: int = 32) -> int:
    value = (seed * 0x9E3779B97F4A7C15 + 0x1234567) & 0xFFFFFFFFFFFFFFFF
    value ^= value >> 29
    value = value * 0xBF58476D1CE4E5B9 & 0xFFFFFFFFFFFFFFFF
    value ^= value >> 32
    return value & ((1 << bits) - 1)


FP_EDGES = (
    0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000,
    0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0x7FF0000000000001,
    0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF,
    0x3F8000003F800000, 0x7F8000007F800000, 0x7FC000007FC00000, 0x0000000100000001,
    0x00800000007FFFFF, 0x4000000040400000, 0xBF800000C0000000, 0x7F7FFFFF00800000,
)


EDGES = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFF0000, 0x0000FFFF, 0x55555555,
         0xAAAAAAAA, 31, 32, 33, 0x80000001)
# Lane values of the integer SIMD states: zero, one, the signed and unsigned
# limits and their neighbours, and the alternating bit patterns.
LANE_EDGES = {
    8: (0x00, 0x01, 0x7F, 0x80, 0xFF, 0x55, 0xAA, 0xFE, 0x81, 0x7E, 0x0F, 0xF0, 0x02),
    16: (0x0000, 0x0001, 0x7FFF, 0x8000, 0xFFFF, 0x00FF, 0xFF00, 0x8001, 0x7FFE, 0x5555, 0xAAAA, 0x0100,
         0x00FE),
    32: (0x00000000, 0x00000001, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x0000FFFF, 0xFFFF0000, 0x80000001,
         0x7FFFFFFE, 0x55555555, 0xAAAAAAAA, 0x00010000, 0xFFFFFFFE),
}
# Half-precision lanes: zeros, one, minus one, infinities, quiet and signalling
# NaNs, the smallest denormal, the largest denormal and normal, and the largest.
HALF_EDGES = (0x0000, 0x8000, 0x3C00, 0xBC00, 0x7C00, 0xFC00, 0x7E00, 0x7C01, 0x0001, 0x03FF, 0x0400, 0x7BFF,
              0xFBFF)
ROTATIONS = (0, 3, 5, 8)
ALU_VARIANTS = ("edges", "edges3", "edges5", "edges8", "pattern", "pattern1")


def make_recipes() -> dict[str, dict[str, int]]:
    """Complete starting states: registers hold edge values (rotated among
    r0-r12 so that every register takes each edge in some recipe) or
    pseudo-random patterns; D registers hold FP edge values or patterns; SP
    points into the data region and LR at the exit stub of the instruction set."""
    recipes: dict[str, dict[str, int]] = {}
    stack = corpus.DATA_BASE + 0x6000
    for suffix, link in (("a", corpus.ARM_EXIT), ("t", corpus.THUMB_EXIT | 1)):
        base = {f"r{i}": 0 for i in range(corpus.GPRS)}
        base.update(apsr=0, fpscr=0, tpidruro=0x70000000, tpidrurw=0x70000100)
        base.update({f"d{i}": 0 for i in range(corpus.D_REGISTERS)})
        base.update(r13=stack, r14=link)
        recipes[f"zero-{suffix}"] = dict(base)
        for rotation in ROTATIONS:
            state = dict(base)
            for index in range(13):
                state[f"r{index}"] = EDGES[(index + rotation) % 13]
            for index in range(corpus.D_REGISTERS):
                state[f"d{index}"] = FP_EDGES[(index + rotation) % len(FP_EDGES)]
            name = "edges" if rotation == 0 else f"edges{rotation}"
            recipes[f"{name}-{suffix}"] = state
        for width, lanes in LANE_EDGES.items():
            state = dict(recipes[f"edges-{suffix}"])
            for index in range(corpus.D_REGISTERS):
                state[f"d{index}"] = sum(lanes[(index + 5 * lane) % len(lanes)] << width * lane
                                         for lane in range(64 // width))
            recipes[f"int{width}-{suffix}"] = state
        state = dict(recipes[f"edges-{suffix}"])
        for index in range(corpus.D_REGISTERS):
            state[f"d{index}"] = sum(HALF_EDGES[(index + 5 * lane) % len(HALF_EDGES)] << 16 * lane
                                     for lane in range(4))
        recipes[f"half-{suffix}"] = state
        for variant in (0, 1):
            state = dict(base)
            for index in range(13):
                state[f"r{index}"] = pattern(index + 100 + 50 * variant)
            for index in range(corpus.D_REGISTERS):
                state[f"d{index}"] = pattern(index + 200 + 50 * variant, 64)
            recipes["pattern-" + suffix if variant == 0 else f"pattern{variant}-{suffix}"] = state
    return recipes


# Context ------------------------------------------------------------------------------


def build_context(archive: pathlib.Path) -> Context:
    scratch = tempfile.TemporaryDirectory(prefix="a32-corpus-")
    root = oracle.archive_root(archive, pathlib.Path(scratch.name))
    catalog = oracle.ArchitectureCatalog(root)
    index = oracle.DecodeIndex(root)
    model = tm.build_model(catalog, index)
    samples: dict[str, list[tuple[str, int]]] = {}
    with SAMPLES.open(encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            if row["status"] in ("ok", "nop") and row["violation"] == "0":
                samples.setdefault(row["form"], []).append((row["isa"], int(row["word"], 16)))
    context = Context(model, expected.Reference(model, catalog), samples, make_recipes())
    context.scratch = scratch  # keep the extracted XML until the process ends
    return context


# Helpers -------------------------------------------------------------------------------


def word_of(isa: str, word: int) -> tuple[int, ...]:
    """The program of one instruction: a word, or Thumb halfwords."""
    if isa == "A32":
        return (word,)
    if isa == "T16":
        return (word >> 16,)
    return (word >> 16, word & 0xFFFF)


def decode(context: Context, isa: str, word: int):
    return context.reference.operands(isa, word, 0, FEATURES)


def fields_of(form: tm.FormModel) -> dict[str, tuple[int, int]]:
    return {field.name: (field.low, field.width) for field in form.encoding.fields}


def set_field(word: int, low: int, width: int, value: int) -> int:
    mask = ((1 << width) - 1) << low
    return word & ~mask | (value << low & mask)


def get_field(word: int, low: int, width: int) -> int:
    return word >> low & ((1 << width) - 1)


def condition_passes(condition: int, flags: int) -> bool:
    """The ARM condition code against the N, Z, C and V bits of an APSR."""
    n, z, c, v = (flags >> 31 & 1, flags >> 30 & 1, flags >> 29 & 1, flags >> 28 & 1)
    result = (z == 1, c == 1, n == 1, v == 1, c == 1 and z == 0, n == v, n == v and z == 0,
              True)[condition >> 1]
    return not result if condition & 1 and condition != 15 else result


def rows_of(context: Context, form: tm.FormModel):
    isa = form.encoding.isa
    return [row for row in context.model.rows[isa]
            if context.model.forms[row.form] is form and row.kind == "instruction"]


def candidate_words(context: Context, form: tm.FormModel, count: int) -> list[int]:
    """Words of the form: stored samples first, then edge and random fills of
    its rows, then the same with the register fields all equal."""
    rng = random.Random(f"{SEED}:{form.name}")
    isa = form.encoding.isa
    words = [word for sample_isa, word in context.samples.get(form.name, []) if sample_isa == isa]
    for row in rows_of(context, form):
        for mode in ("zeros", "ones", "random", "random"):
            word = data._fill(rng, row, form, mode, True)
            if word is not None:
                words.append(word)
    fields = fields_of(form)
    registers = [name for name in fields if data.REGISTER_FIELDS.match(name) and fields[name][1] == 4]
    for word in list(words[:6]):
        if len(registers) >= 2:
            low = fields[registers[0]][0]
            value = get_field(word, low, 4)
            if value in (13, 15):
                value = 3
            aliased = word
            for name in registers:
                aliased = set_field(aliased, fields[name][0], 4, value)
            words.append(aliased)
    seen = []
    for word in words:
        if word not in seen:
            seen.append(word)
    return seen[:count * 4]


def always(form: tm.FormModel) -> bool:
    return form.cond_low == 28 and form.conditional


def usable(context: Context, form: tm.FormModel, word: int, allow_pc_write: bool = False):
    """The word with an always-true condition and its operands, or None."""
    isa = form.encoding.isa
    if isa == "A32" and always(form):
        word = set_field(word, 28, 4, 14)
    result, operands = decode(context, isa, word)
    if result.status == "undefined" or result.form != form.name:
        return None
    if not allow_pc_write and writes_pc(operands, form):
        return None
    return word, operands


def writes_pc(operands: dict, form: tm.FormModel) -> bool:
    for name in ("d", "dHi", "dLo", "t", "t2"):
        if operands.get(name) == 15 and name != "t" or (name == "t" and operands.get(name) == 15
                                                          and "loads" in form.traits):
            return True
    registers = operands.get("registers")
    if isinstance(registers, int) and registers & 0x8000 and "loads" in form.traits:
        return True
    return False


# The alu suite ---------------------------------------------------------------------------

ALU_EXCLUDED = {"IT_T1"}  # an IT block needs its instructions (conditions suite)
NOT_ALU = {"loads", "stores", "prefetch", "exclusive", "branch", "interwork", "exception",
           "barrier", "hint", "load_pc"}


def alu_forms(context: Context):
    for form in context.model.forms:
        if (form.encoding is None or form.level != "base" or form.availability != "implemented"
                or form.name in policy.NEVER_EXECUTED or form.name in ALU_EXCLUDED
                or form.traits & NOT_ALU or form.check or form.nop):
            continue
        yield form


def suite_alu(context: Context) -> list[Case]:
    cases = []
    for form in alu_forms(context):
        isa = form.encoding.isa
        chosen = []
        for word in candidate_words(context, form, 8):
            found = usable(context, form, word)
            if found is not None and found[0] not in [w for w, _ in chosen]:
                chosen.append(found)
            if len(chosen) == 8:
                break
        for index, (word, _operands) in enumerate(chosen):
            for variant in range(2):
                number = len(cases)
                recipe = f"{ALU_VARIANTS[(index + 2 * variant + pattern(len(cases)) % 3) % 6]}-" \
                    f"{'a' if isa == 'A32' else 't'}"
                flags = FLAGS[(pattern(number) >> 8) % len(FLAGS)]
                cases.append(Case(
                    "alu", f"{form.name}/{index}.{variant}", form.name, ISA_LETTER[isa],
                    word_of(isa, word), 0, recipe, {"apsr": flags}))
    return cases + move_pairs(context)


# MOVW then MOVT: of one register and of two, apart, twice, under a condition and in
# IT blocks, and with the block limit between them.
def movw(rd: int, value: int, thumb: bool, cond: int = 14) -> tuple[int, ...]:
    if not thumb:
        return (cond << 28 | 0x03000000 | value >> 12 << 16 | rd << 12 | value & 0xFFF,)
    return (0xF240 | (value >> 11 & 1) << 10 | value >> 12, (value >> 8 & 7) << 12 | rd << 8 | value & 0xFF)


def movt(rd: int, value: int, thumb: bool, cond: int = 14) -> tuple[int, ...]:
    word = movw(rd, value, thumb, cond)
    return (word[0] | 0x00400000,) if not thumb else (word[0] | 0x0080, word[1])


def move_pairs(context: Context) -> list[Case]:
    cases = []
    for thumb in (False, True):
        state, nop = ("T", (0xBF00,)) if thumb else ("A", (0xE320F000,))
        add = (0xF103, 0x0301) if thumb else (0xE2833001,)  # add r3, r3, #1
        programs = {
            "same": [movw(0, 0x1234, thumb), movt(0, 0xABCD, thumb)],
            "lr": [movw(14, 0xFFFF, thumb), movt(14, 0x8000, thumb)],
            "two": [movw(1, 0x5678, thumb), movt(2, 0x9ABC, thumb)],
            "apart": [movw(3, 0x0FFF, thumb), add, movt(3, 0x7001, thumb)],
            "twice": [movw(5, 0x8001, thumb), movt(5, 0x0101, thumb), movt(5, 0xFEDC, thumb)],
            "limit": [nop] * (58 if thumb else 63) + [movw(6, 0x4321, thumb), movt(6, 0x8765, thumb)],
        }
        if thumb:
            programs["itt"] = [(0xBF04,), movw(7, 0x1111, True), movt(7, 0x2222, True)]
            programs["ite"] = [(0xBF0C,), movw(8, 0x3333, True), movt(8, 0x4444, True)]
        else:
            programs["eq"] = [movw(7, 0x1111, False, 0), movt(7, 0x2222, False, 0)]
        for name, parts in programs.items():
            target = len(parts) - 1
            form = "MOVT_T1" if thumb else "MOVT_A1"
            for flags in ((0, 0x40000000) if name in ("eq", "itt", "ite") else (0x60000000,)):
                program = tuple(unit for part in parts for unit in part)
                cases.append(Case("alu", f"{form}/pair-{name}.{flags >> 28:x}", form, state,
                                  program, target, f"pattern-{state.lower()}", {"apsr": flags}))
    return cases


SUITES = {"alu": suite_alu}
