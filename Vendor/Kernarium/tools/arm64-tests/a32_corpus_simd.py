#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The floating-point and Advanced SIMD suites of the AArch32 corpus.

- simd: the register forms of VFPv4-D32 and Advanced SIMD (the `simd` level),
  each decoded word started in recipes whose D registers hold floating-point
  edge values (zeros, infinities, NaNs, denormals, extremes) and patterns,
  under every FPSCR rounding mode, flush-to-zero, default-NaN and half-
  precision setting; the fixed-width integer and element-size variants come
  from sampling the free fields of each form's rows;
- simd-steps: VRECPS and VRSQRTS with every infinity against zeros,
  denormals and one, in half and single precision (FZ16 clear: a half-
  precision denormal is a nonzero number, so its product with an infinity is
  infinite, where a single-precision one is flushed under the Standard FPSCR
  value);
- simd-convert: VCVT and VCVTR between single or double precision and 32-bit
  integers, with fractions halfway and either side of it, the integer limits
  and the values past them, a denormal, a NaN and an infinity, under each
  rounding mode and flush-to-zero; and integers to floating point that round;
- simd-memory: VLDR, VSTR, VLDM, VSTM, the deprecated FLDMX/FSTMX and the
  structure loads and stores VLD1-4/VST1-4, aimed with the memory suites'
  address scenarios, including alignment-hinted accesses at misaligned
  addresses;
- fpscr: VMRS and VMSR, including the exception trap enables the profile
  reads as zero and ignores writes to (decision D4), and FZ16, which holds
  wherever FEAT_FP16 is;
- fpscr-reserved: the same with Stride and Len set, whose behaviour is the
  implementation's and so is recorded from hardware only.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import random  # noqa: E402

import a32_corpus_plan as plan  # noqa: E402
from a32_corpus_memory import D, READ_ONLY, solve  # noqa: E402
from a32_corpus_plan import (  # noqa: E402
    FLAGS, ISA_LETTER, Case, Context, candidate_words, decode, fields_of, get_field, pattern, set_field, usable,
    word_of,
)

# FPSCR values a case starts in: rounding modes (23:22), FZ (24), DN (25),
# AHP (26), QC (27) and the cumulative flags. Stride and Len (21:20, 18:16)
# stay clear: whether they stick depends on the implementation, so the writes
# that probe them are the hardware-only fpscr-reserved suite. FZ16 (19) stays
# clear too: it is FEAT_FP16's, which an oracle may lack (the Pi 4's
# Cortex-A72 reads it as zero), so only the fpscr suite, recorded where
# FEAT_FP16 is, writes it.
FPSCR_MODES = (
    0x00000000, 0x00400000, 0x00800000, 0x00C00000, 0x01000000, 0x02000000, 0x03000000,
    0x04000000, 0x08000000, 0x0000009F, 0x01C00000, 0x07C0009F,
)
FPSCR_LEN_STRIDE = 0x00370000
FPSCR_FZ16 = 0x00080000
# Fields that choose what the instruction does rather than which registers it names.
NOT_VARIANT = {"D", "N", "M", "Vd", "Vn", "Vm", "Rn", "Rm", "Rt", "Rt2", "cond", "P", "W", "imm8", "imm6", "imm4",
               "imm4H", "imm4L", "imm3", "imm3H", "imm3L", "index_align"}
RECIPES = ("edges", "int8", "pattern", "int16", "edges3", "int32", "pattern1", "edges5")
MEMORY_OR_SYSTEM = {"loads", "stores", "exclusive", "barrier", "hint", "prefetch"}


def variant_fields(form):
    return [(field.low, field.width) for field in form.encoding.fields
            if field.name not in NOT_VARIANT and field.width <= 4]


# Operands that pick a lane, a shift amount or an offset: every value of each, per element size.
WATCH = ("index", "shift_amount", "position", "length", "frac_bits", "lsb", "sel", "lowbit")


def operand_words(context: Context, form, chosen, limit: int = 12):
    """Words that add values of the watched operands to those of the chosen words."""
    keys = [key for key in WATCH if key in chosen[0][1]]
    if not keys:
        return []

    def signature(operands):
        return {(key, str(operands.get(key)), operands.get("esize"), operands.get("ebytes")) for key in keys}

    seen = set()
    for _word, operands in chosen:
        seen |= signature(operands)
    rng = random.Random(f"{plan.SEED}:{form.name}:operands")
    extra = []
    for row in plan.rows_of(context, form):
        for _attempt in range(160):
            word = plan.data._fill(rng, row, form, "random", True)
            found = None if word is None else usable(context, form, word)
            if found is not None and signature(found[1]) - seen and len(extra) < limit:
                seen |= signature(found[1])
                extra.append(found)
    return extra


def pick_words(context: Context, form, count: int):
    """Up to `count` usable words of the form, as (word, operands): the first
    candidate, then that word with each variant field (size, U, op, opc, ...)
    taking its other values in turn, then the remaining candidates."""
    candidates = []
    for word in candidate_words(context, form, count + 6):
        found = usable(context, form, word)
        if found is not None and found[0] not in [w for w, _ in candidates]:
            candidates.append(found)
    if not candidates:
        return []
    chosen = [candidates[0]]
    base = candidates[0][0]
    pools = []
    for low, width in variant_fields(form):
        current = get_field(base, low, width)
        pool = []
        for value in range(1 << width):
            if value != current:
                found = usable(context, form, set_field(base, low, width, value))
                if found is not None:
                    pool.append(found)
        if pool:
            pools.append(pool)
    for depth in range(16):
        for pool in pools:
            if depth < len(pool) and pool[depth][0] not in [w for w, _ in chosen] and len(chosen) < count:
                chosen.append(pool[depth])
    for found in candidates[1:]:
        if len(chosen) < count and found[0] not in [w for w, _ in chosen]:
            chosen.append(found)
    return chosen + [found for found in operand_words(context, form, chosen)
                     if found[0] not in [w for w, _ in chosen]]


def simd_forms(context: Context, levels=("simd",)):
    for form in context.model.forms:
        if (form.encoding is None or form.level not in levels or form.availability != "implemented"
                or form.check or form.nop):
            continue
        yield form


def words_for(form) -> int:
    return 10 if variant_fields(form) else 6


def suite_simd(context: Context, levels=("simd",), suite="simd", recipes=RECIPES) -> list[Case]:
    cases = []
    for form in simd_forms(context, levels):
        if form.traits & MEMORY_OR_SYSTEM:
            continue
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        for index, (word, _operands) in enumerate(pick_words(context, form, words_for(form))):
            for variant in range(2):
                number = len(cases)
                recipe = f"{recipes[(index + 2 * variant + number) % len(recipes)]}-{suffix}"
                initial = {"fpscr": FPSCR_MODES[(index * 3 + variant * 5 + number) % len(FPSCR_MODES)],
                           "apsr": FLAGS[pattern(number) >> 8 & 3]}
                cases.append(Case(suite, f"{form.name}/{index}.{variant}", form.name, ISA_LETTER[isa],
                                  word_of(isa, word), 0, recipe, initial))
    return cases


# VRECPS and VRSQRTS: infinity against zero ------------------------------------------------------------------------------

STEP_FORMS = ("VRECPS_A1_D", "VRECPS_A1_Q", "VRECPS_T1_D", "VRECPS_T1_Q",
              "VRSQRTS_A1_D", "VRSQRTS_A1_Q", "VRSQRTS_T1_D", "VRSQRTS_T1_Q")
# The operands of an infinity times a zero (a zero product, not NaN) and of an infinity times a denormal
# (flushed to a zero only in single precision, where the Standard FPSCR value sets FZ, and in half precision
# only when FZ16 is set): +-infinity, then +-0, +-the smallest, a mid-range and the largest denormal, and one.
STEP_INFINITIES = {16: (0x7C00, 0xFC00), 32: (0x7F800000, 0xFF800000)}
STEP_FINITE = {
    16: (0x0000, 0x8000, 0x0001, 0x8001, 0x0080, 0x8080, 0x03FF, 0x83FF, 0x3C00),
    32: (0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x00400000, 0x80400000, 0x007FFFFF, 0x807FFFFF,
         0x3F800000),
}
# (d, n, m) D registers; the Q forms use the same numbers, all even.
STEP_REGISTERS = ((0, 4, 0), (6, 2, 8), (10, 10, 12), (14, 2, 6))


def step_pairs(width: int) -> list[tuple[int, int]]:
    """Every infinity against every finite operand, in both operand orders."""
    pairs = []
    for infinity in STEP_INFINITIES[width]:
        for finite in STEP_FINITE[width]:
            pairs.append((infinity, finite))
            pairs.append((finite, infinity))
    return pairs


def suite_simd_steps(context: Context) -> list[Case]:
    cases = []
    for form_index, name in enumerate(STEP_FORMS):
        form = context.model.by_name[name]
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        fields = fields_of(form)
        base = pick_words(context, form, 1)[0][0]
        registers = 2 if name.endswith("_Q") else 1
        for sz, width in ((1, 16), (0, 32)):
            pairs = step_pairs(width)
            lanes = 64 // width * registers
            for chunk, start in enumerate(range(0, len(pairs), lanes)):
                d, n, m = STEP_REGISTERS[chunk % len(STEP_REGISTERS)]
                word = base
                for field, value in (("sz", sz), ("Vd", d & 15), ("Vn", n & 15), ("Vm", m & 15),
                                     ("D", 0), ("N", 0), ("M", 0)):
                    word = set_field(word, *fields[field], value)
                if usable(context, form, word) is None:
                    raise SystemExit(f"{name}: {word:08x} is not a usable word")
                group = [pairs[(start + lane) % len(pairs)] for lane in range(lanes)]
                initial = {"fpscr": FPSCR_MODES[(chunk + 5 * sz + form_index) % len(FPSCR_MODES)],
                           "apsr": FLAGS[(chunk + form_index) % len(FLAGS)]}
                for operand, register in ((0, n), (1, m)):
                    for part in range(registers):
                        lane_values = group[part * (64 // width):(part + 1) * (64 // width)]
                        initial[f"d{register + part}"] = sum(
                            lane[operand] << width * index for index, lane in enumerate(lane_values))
                cases.append(Case("simd-steps", f"{name}/f{width}.{chunk}", name, ISA_LETTER[isa],
                                  word_of(isa, word), 0, f"zero-{suffix}", initial))
    return cases


# VCVT between floating point and 32-bit integers ------------------------------------------------------------------------

# Floating point to integer: halves and either side of them (round to nearest takes the even one), the largest values
# below the integer limits and the first past them (saturation, IOC), a negative fraction for the unsigned forms, a
# denormal (FZ flushes it), a NaN and an infinity.
CONVERT_SINGLES = (0x3F000000, 0xBF000000, 0x3FC00000, 0xBFC00000, 0x40200000, 0xC0200000, 0x3F400000, 0xBF400000,
                   0x3EFFFFFF, 0x3F000001, 0x4AFFFFFF, 0x4EFFFFFF, 0x4F000000, 0xCF000000, 0xCF000001, 0x4F7FFFFF,
                   0x4F800000, 0x00000001, 0x80400000, 0x7FC00000, 0xFF800000)
CONVERT_DOUBLES = (0x3FE0000000000000, 0xBFE0000000000000, 0x3FF8000000000000, 0xBFF8000000000000,
                   0x4004000000000000, 0xC004000000000000, 0x41DFFFFFFFE00000, 0x41DFFFFFFFC00000,
                   0xC1E0000000100000, 0xC1E0000000000000, 0x41EFFFFFFFF00000, 0x41EFFFFFFFE00000,
                   0xBFD0000000000000, 0x0000000000000001, 0x7FF8000000000000, 0xFFF0000000000000)
# Integer to floating point: values a single cannot hold, which round (a double holds every one).
CONVERT_INTEGERS = (0x00000000, 0x00000001, 0xFFFFFFFF, 0x01000001, 0x01000003, 0x7FFFFFFF, 0x80000000, 0x80000001,
                    0xFEFFFFFF, 0x7FFFFFC1)
# FPSCR: each rounding mode, and flush-to-zero with round to nearest and toward minus infinity.
CONVERT_ROUNDING = (0x00000000, 0x00400000, 0x00800000, 0x00C00000, 0x01000000, 0x01800000)
# VCVT rounds toward zero whatever the mode: one directed mode and flush-to-zero.
CONVERT_TOWARD_ZERO = (0x00400000, 0x01800000)
# (destination, source) register numbers in the form's sizes: lanes and halves other than the first.
CONVERT_REGISTERS = ((0, 1), (5, 2), (7, 7), (12, 30), (31, 17))


def convert_word(context: Context, form, d: int, m: int) -> int:
    """The form's word with S/D d and S/D m (Vd:D or D:Vd as the form's sizes say)."""
    fields = fields_of(form)
    word = pick_words(context, form, 1)[0][0]
    double_destination = form.name.startswith("VCVT_vi_") and form.name.endswith("_D")
    double_source = not form.name.startswith("VCVT_vi_") and form.name.endswith("_D")
    for (vector, bit), number, double in ((("Vd", "D"), d, double_destination), (("Vm", "M"), m, double_source)):
        if double:
            word = set_field(word, *fields[vector], number & 15)
            word = set_field(word, *fields[bit], number >> 4)
        else:
            word = set_field(word, *fields[vector], number >> 1)
            word = set_field(word, *fields[bit], number & 1)
    if usable(context, form, word) is None:
        raise SystemExit(f"{form.name}: {word:08x} is not a usable word")
    return word


def suite_simd_convert(context: Context) -> list[Case]:
    cases = []
    to_integer = [f"{mnemonic}_{kind}_{isa}1_{size}" for mnemonic in ("VCVT", "VCVTR") for kind in ("siv", "uiv")
                  for size in ("S", "D") for isa in ("A", "T")]
    for name in to_integer:
        form = context.model.by_name[name]
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        double = name.endswith("_D")
        values = CONVERT_DOUBLES if double else CONVERT_SINGLES
        modes = CONVERT_ROUNDING if name.startswith("VCVTR") else CONVERT_TOWARD_ZERO
        for index, value in enumerate(values):
            d, m = CONVERT_REGISTERS[index % len(CONVERT_REGISTERS)]
            if double:
                m &= 31
            word = convert_word(context, form, d, m)
            for mode_index, mode in enumerate(modes):
                number = len(cases)
                initial = {"fpscr": mode, "apsr": FLAGS[(index + mode_index) % len(FLAGS)]}
                if double:
                    initial[f"d{m}"] = value
                else:
                    initial[f"d{m >> 1}"] = (context.recipes[f"pattern-{suffix}"][f"d{m >> 1}"]
                                             & ~(0xFFFFFFFF << 32 * (m & 1)) | value << 32 * (m & 1))
                cases.append(Case("simd-convert", f"{name}/{index}.{mode_index}", name, ISA_LETTER[isa],
                                  word_of(isa, word), 0, f"pattern-{suffix}", initial))
    for name in ("VCVT_vi_A1_S", "VCVT_vi_T1_S", "VCVT_vi_A1_D", "VCVT_vi_T1_D"):
        form = context.model.by_name[name]
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        fields = fields_of(form)
        for signed in (0, 1):
            for index, value in enumerate(CONVERT_INTEGERS):
                d, m = CONVERT_REGISTERS[index % len(CONVERT_REGISTERS)]
                if name.endswith("_D"):
                    d &= 31
                word = set_field(convert_word(context, form, d, m), *fields["op"], signed)
                if usable(context, form, word) is None:
                    raise SystemExit(f"{name}: {word:08x} is not a usable word")
                for mode_index, mode in enumerate(CONVERT_ROUNDING[:4]):
                    initial = {"fpscr": mode, "apsr": FLAGS[(index + mode_index) % len(FLAGS)],
                               f"d{m >> 1}": (context.recipes[f"pattern-{suffix}"][f"d{m >> 1}"]
                                              & ~(0xFFFFFFFF << 32 * (m & 1)) | value << 32 * (m & 1))}
                    cases.append(Case("simd-convert", f"{name}/{signed}.{index}.{mode_index}", name,
                                      ISA_LETTER[isa], word_of(isa, word), 0, f"pattern-{suffix}", initial))
    return cases


# Memory ----------------------------------------------------------------------------------------------------------------

SCENARIOS = ("a", "x4", "u1", "x16", "ro", "unmapped", "zero", "last")


def extent_of(operands: dict) -> int:
    """The bytes a transfer touches, at most."""
    if "regs" in operands and "imm32" in operands:  # VLDM, VSTM: imm32 is the whole block
        return max(operands["imm32"], 8)
    return 32


def scenario_address(scenario: str, extent: int) -> int:
    half = extent // 2 & ~7  # aligned, so the access straddles the line
    return {
        "a": D + 0x1040, "x4": D + 0x2000 - half, "u1": D + 0x1041, "x16": D + 0x4000 - half,
        "ro": READ_ONLY, "unmapped": 0x60000000, "zero": 0, "last": D + 0x8000 - (extent + 7 & ~7),
    }[scenario]


def aim_memory(operands: dict, address: int, apsr: int, choice: int):
    """Registers that make a SIMD transfer start at `address`."""
    n = operands.get("n")
    if n is None or n == 15:
        return {}
    if "regs" in operands and "imm32" in operands:  # VLDM, VSTM, FLDMX, FSTMX
        return {f"r{n}": (address if operands["add"] else address + operands["imm32"]) & 0xFFFFFFFF}
    if "m" in operands:  # VLDn, VSTn: the address is the base register
        base = {f"r{n}": address & 0xFFFFFFFF}
        m = operands["m"]
        if operands.get("register_index") and m not in (13, 15) and m != n:
            base[f"r{m}"] = (0x20, 0x100, 8, 0x1000)[choice % 4]
        return base
    return solve(operands, address, apsr, choice)  # VLDR, VSTR


def suite_simd_memory(context: Context, levels=("simd",), suite="simd-memory") -> list[Case]:
    cases = []
    used = set()
    for form_index, form in enumerate(simd_forms(context, levels)):
        if not form.traits & {"loads", "stores"} or form.name[0] not in "VF" or form.traits & {"exclusive"}:
            continue
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        for index, (word, operands) in enumerate(pick_words(context, form, 6)):
            for step in range(2):
                number = len(cases)
                scenario = "a" if step == 0 and index % 2 == 0 else \
                    SCENARIOS[(index * 3 + step * 5 + form_index) % len(SCENARIOS)]
                name = f"{form.name}/{index}.{scenario}"
                if name in used:
                    continue
                recipe = f"{RECIPES[(index + step + form_index) % len(RECIPES)]}-{suffix}"
                apsr = FLAGS[pattern(number) >> 8 & 3]
                initial = {"apsr": apsr}
                if operands.get("n") not in (None, 15):
                    aim = aim_memory(operands, scenario_address(scenario, extent_of(operands)), apsr,
                                     number)
                    if aim is None:
                        continue
                    initial.update(aim)
                used.add(name)
                cases.append(Case(suite, name, form.name, ISA_LETTER[isa], word_of(isa, word), 0, recipe,
                                  initial))
    return cases


# VMRS and VMSR ----------------------------------------------------------------------------------------------------------

def fpscr_cases(context: Context, suite: str, values, tag: str = "") -> list[Case]:
    cases = []
    for name in ("VMRS_A1_AS", "VMRS_T1_AS", "VMSR_A1_AS", "VMSR_T1_AS"):
        form = context.model.by_name[name]
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        fields = fields_of(form)
        found = None
        for base in candidate_words(context, form, 12):
            found = usable(context, form, base)
            if found is not None:
                break
        for register in (0, 3, 12, 14, 15):
            word = set_field(found[0], *fields["Rt"], register)
            if usable(context, form, word) is None:
                continue
            for index, value in enumerate(values):
                number = len(cases)
                initial = {"apsr": FLAGS[pattern(number) >> 8 & 3], "fpscr": values[(index + 3) % len(values)]}
                if name.startswith("VMSR"):
                    initial[f"r{register}"] = value
                cases.append(Case(suite, f"{name}/r{register}.{tag}{index}", name, ISA_LETTER[isa],
                                  word_of(isa, word), 0, f"{RECIPES[index % 4]}-{suffix}", initial))
    return cases


def suite_fpscr(context: Context) -> list[Case]:
    """VMRS and VMSR: the trap-enable bits (14:8 and 15) read as zero and ignore
    writes, and FZ16 holds what is written to it."""
    values = (0x00000000, 0x03C09F9F, 0xF7C0FFFF, 0x0000FF00, 0x00001F00,
              0xFFFFFFFF & ~(FPSCR_LEN_STRIDE | FPSCR_FZ16), 0x07C0009F, 0x00C00000, 0x0000009F, 0x08000000)
    half = (FPSCR_FZ16, 0xFFFFFFFF & ~FPSCR_LEN_STRIDE, 0x03C80000, 0x0008009F)
    return fpscr_cases(context, "fpscr", values) + fpscr_cases(context, "fpscr", half, "fz16.")


def suite_fpscr_reserved(context: Context) -> list[Case]:
    """VMRS and VMSR with Stride and Len in play: what sticks is the implementation's."""
    values = (0x00370000, 0xFFF7FFFF, 0x00300000, 0x00040000, 0x00170000, 0x00010000, 0x00200000, 0x00070000)
    return fpscr_cases(context, "fpscr-reserved", values)


# Alignment hints ------------------------------------------------------------------------------------------------------

# The hints of a VLD1 or VST1 copy by register count (the align field: none,
# 64, 128, 256 bits), and the addresses each runs at: a line's start, 8, 16
# and 1 byte past it, across the 4 KiB and 16 KiB lines on 8- and 16-byte
# boundaries, read-only, unmapped, and unmapped at an odd address, where the
# alignment fault comes first.
HINT_ALIGNS = {1: (0, 1), 2: (0, 1, 2), 3: (0, 1), 4: (0, 1, 2, 3)}
HINT_SCENARIOS = {
    "a": D + 0x1040, "m8": D + 0x1048, "m16": D + 0x1050, "u1": D + 0x1041, "x4": D + 0x2000 - 16,
    "x16": D + 0x4000 - 8, "ro": READ_ONLY, "unmapped": 0x60000000, "uu": 0x60000001,
}
# A VLD1 to all lanes: element size by its field, and the addresses: aligned,
# one and two bytes past, the last bytes before the 4 KiB and 16 KiB lines
# (an element without a hint crosses them), read-only, unmapped and odd.
LANE_SCENARIOS = {
    "a": D + 0x1040, "u1": D + 0x1041, "u2": D + 0x1042, "x4": D + 0x2000 - 1, "x16": D + 0x4000 - 2,
    "ro": READ_ONLY + 0x3FFC, "unmapped": 0x60000000, "uu": 0x60000001,
}
HINT_INDEX = (0x20, 0xFFFFFFF8, 0x1000, 4)


def hint_word(isa: str, word: int) -> tuple[int, ...]:
    """An A32 structure word (1111 0100 ...) in the instruction set."""
    return (word,) if isa == "A32" else (0xF900 | word >> 16 & 0xFF, word & 0xFFFF)


def suite_simd_hints(context: Context) -> list[Case]:
    """VLD1 and VST1 copies with each alignment hint and none, and VLD1 to
    all lanes with and without one, at addresses the hint accepts and
    refuses, across lines and faulting, with each writeback."""
    cases = []
    number = 0
    for isa in ("A32", "T32"):
        suffix = "a" if isa == "A32" else "t"
        for store in (False, True):
            for regs, aligns in HINT_ALIGNS.items():
                for align in aligns:
                    for index, (scenario, address) in enumerate(HINT_SCENARIOS.items()):
                        if scenario == "ro" and not store:
                            continue
                        writeback = (index + regs + align) % 3
                        m = (15, 13, 6)[writeback]
                        size = (index + align) % 4
                        first = (0, 16, 7, 28)[(index + regs) % 4]
                        first = min(first, 32 - regs)
                        word = ((0xF4000000 if store else 0xF4200000) | (first >> 4 & 1) << 22 | 4 << 16
                                | (first & 15) << 12 | {1: 0x7, 2: 0xA, 3: 0x6, 4: 0x2}[regs] << 8 | size << 6
                                | align << 4 | m)
                        initial = {"apsr": FLAGS[pattern(number) >> 8 & 3], "r4": address}
                        if m == 6:
                            initial["r6"] = HINT_INDEX[number % len(HINT_INDEX)]
                        form = found_form(context, isa, word)
                        cases.append(Case("simd-hints", f"{form}/{regs}.{align}.{scenario}", form, ISA_LETTER[isa],
                                          hint_word(isa, word), 0,
                                          f"{RECIPES[number % len(RECIPES)]}-{suffix}", initial))
                        number += 1
        for size in (0, 1, 2):
            for regs in (1, 2):
                for hinted in ((False, True) if size else (False,)):
                    for index, (scenario, address) in enumerate(LANE_SCENARIOS.items()):
                        writeback = (index + size + regs) % 3
                        m = (15, 13, 6)[writeback]
                        first = (3, 31, 16, 8)[(index + size) % 4]
                        first = min(first, 32 - regs)
                        word = (0xF4A00C00 | (first >> 4 & 1) << 22 | 4 << 16 | (first & 15) << 12 | size << 6
                                | (regs - 1) << 5 | int(hinted) << 4 | m)
                        initial = {"apsr": FLAGS[pattern(number) >> 8 & 3], "r4": address}
                        if m == 6:
                            initial["r6"] = HINT_INDEX[number % len(HINT_INDEX)]
                        form = found_form(context, isa, word)
                        cases.append(Case("simd-hints",
                                          f"{form}/{8 << size}.{regs}.{'h' if hinted else 'n'}.{scenario}", form,
                                          ISA_LETTER[isa], hint_word(isa, word), 0,
                                          f"{RECIPES[number % len(RECIPES)]}-{suffix}", initial))
                        number += 1
    return cases


def found_form(context: Context, isa: str, word: int) -> str:
    """The inventory form of an A32 structure word in the instruction set; a
    word that does not decode is a planner bug."""
    program = hint_word(isa, word)
    value = program[0] if isa == "A32" else program[0] << 16 | program[1]
    result, _ = decode(context, isa, value)
    if result.status == "undefined":
        raise SystemExit(f"simd-hints: {isa} {value:08x} does not decode")
    return result.form


SUITES = {
    "simd": suite_simd,
    "simd-steps": suite_simd_steps,
    "simd-convert": suite_simd_convert,
    "simd-memory": suite_simd_memory,
    "fpscr": suite_fpscr,
    "fpscr-reserved": suite_fpscr_reserved,
    "simd-hints": suite_simd_hints,
}
