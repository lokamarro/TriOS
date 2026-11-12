#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Armv8 additions of the AArch32 corpus (the `complete` level).

- armv8: the register forms (CRC32, AES, SHA-1, SHA-2, VRINT*, VCVT[ANPM],
  VSEL, VMAXNM, VMINNM, the half-precision arithmetic, VCADD, VCMLA, VQRDMLAH,
  VQRDMLSH, VJCVT, the dot-product, matrix-multiply and half-to-single
  multiply-accumulate forms), started in the simd suite's floating-point
  states under every FPSCR mode;
- armv8-memory: the half-precision VLDR and VSTR, with the memory suites'
  address scenarios (LDA, STL, LDAEX, STLEX and SEVL are base, and in the
  memory, exclusive and system suites);
- armv8-system: SB;
- armv8-fz16: VFMAL and VFMSL, vector and by scalar, with FPSCR.FZ16 set and
  clear and half-precision denormals among the operands: the instruction
  unpacks them with FZ16 in force, where the simd suites keep FZ16 clear;
- armv8-convert16: the Advanced SIMD conversions from half precision to 16-bit
  integers and fixed point, with FZ16 set and clear and lanes of one
  instruction that overflow after lanes that are inexact, so that the flags of
  one lane must survive the saturation of another.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import a32_corpus_simd as simd  # noqa: E402
import a32_corpus_system as system  # noqa: E402
from a32_corpus_plan import (  # noqa: E402
    FLAGS, ISA_LETTER, Case, Context, fields_of, pattern, set_field, usable, word_of,
)

LEVELS = ("complete",)
RECIPES = ("edges", "half", "int8", "pattern", "half", "int16", "edges3", "int32", "pattern1", "edges5")


def suite_armv8(context: Context) -> list[Case]:
    return simd.suite_simd(context, LEVELS, "armv8", RECIPES)


def suite_armv8_memory(context: Context) -> list[Case]:
    """The half-precision VLDR and VSTR: the acquire/release loads and stores are base."""
    return simd.suite_simd_memory(context, LEVELS, "armv8-memory")


def suite_armv8_system(context: Context) -> list[Case]:
    forms = [form for form in context.model.forms
             if form.encoding is not None and form.level == "complete" and form.availability == "implemented"
             and (form.traits & {"barrier", "hint"} or form.nop) and not form.check]
    return system.generic(context, "armv8-system", forms, 6, 2)


# VFMAL and VFMSL under FPSCR.FZ16 ------------------------------------------------------------------------------------

# The forms: the vector and by-scalar encodings of each instruction, in A32 and Thumb, on a D register of results
# (the operands are S registers, one half-precision pair each) or a Q register (the operands are D registers).
FZ16_FORMS = tuple(f"{mnemonic}{scalar}_{isa}1_{size}" for mnemonic in ("VFMAL", "VFMSL") for scalar in ("", "_s")
                   for isa in ("A", "T") for size in ("D", "Q"))
# FPSCR: FZ16 alone, then with the alternative half-precision format, each other rounding mode, flush-to-zero,
# default NaN, everything and the cumulative flags (the Standard FPSCR value ignores all but AHP and FZ16, and
# FPUnpack ignores AHP); then FZ16 clear, with and without AHP, where a denormal half is a nonzero number.
FZ16_MODES = (0x00080000, 0x04080000, 0x00C80000, 0x01880000, 0x02080000, 0x07C80000, 0x0008009F, 0x08080000,
              0x00000000, 0x04000000)
# (half, half, single) lanes: addend + a * b. A half denormal is flushed to zero under FZ16 and widened exactly
# otherwise, so each lane has a denormal factor whose product changes the sum, the sum's rounding, or the case
# (infinity, NaN, a single denormal addend, which FZ flushes in the Standard FPSCR value, and a signed zero).
FZ16_LANES = (
    (0x0001, 0x3C00, 0x00000000), (0x8001, 0x3C00, 0x00000000), (0x03FF, 0xBC00, 0x35800000),
    (0x83FF, 0xBC00, 0xB5800000), (0x3C00, 0x0001, 0x00000000), (0x3C00, 0x8200, 0x35800000),
    (0x0001, 0x0001, 0x3F800000), (0x0200, 0x7C00, 0x00000000), (0xFC00, 0x03FF, 0x3F800000),
    (0x0001, 0x7E00, 0x3F800000), (0x0001, 0x7BFF, 0x00000000), (0x03FF, 0x03FF, 0x00800000),
    (0x0001, 0x3C00, 0x00000001), (0x8000, 0x0001, 0x80000000), (0x7C01, 0x0001, 0x00000000),
    (0x0001, 0x3C00, 0x7FC00000), (0x8200, 0x3C00, 0x00000000), (0x0200, 0x0200, 0x33800000),
)
# The elements of a by-scalar register the instruction does not select.
FZ16_DECOYS = (0x3C00, 0x4000, 0xC000, 0x3800)
# (d, n, m) of each shape: D-form operands are S registers, Q-form results and operands D registers; a by-scalar
# m is S0-S15 or D0-D7. None of a case's registers overlap.
FZ16_REGISTERS = {
    ("D", False): ((16, 4, 7), (20, 31, 10), (0, 9, 22), (26, 13, 9)),
    ("D", True): ((16, 4, 7), (20, 31, 10), (0, 9, 12), (26, 13, 3)),
    ("Q", False): ((16, 4, 7), (20, 31, 10), (0, 9, 22), (26, 13, 8)),
    ("Q", True): ((16, 4, 7), (20, 31, 2), (0, 9, 5), (26, 13, 0)),
}


def fz16_word(context: Context, form, d: int, n: int, m: int, index: int) -> int:
    """The form's word with the result in D d and the operands in registers n and m (S registers of a D form,
    D registers of a Q form), and the selected element of a by-scalar m."""
    fields = fields_of(form)
    quad = form.name.endswith("_Q")
    scalar = "_s_" in form.name
    word = simd.pick_words(context, form, 1)[0][0]
    values = {"D": d >> 4, "Vd": d & 15}
    values.update({"N": n >> 4, "Vn": n & 15} if quad else {"N": n & 1, "Vn": n >> 1})
    if scalar:
        values.update({"M": index >> 1, "Vm": m | (index & 1) << 3} if quad
                      else {"M": m & 1, "Vm": m >> 1 | index << 3})
    else:
        values.update({"M": m >> 4, "Vm": m & 15} if quad else {"M": m & 1, "Vm": m >> 1})
    for name, value in values.items():
        word = set_field(word, *fields[name], value)
    found = usable(context, form, word)
    expected = {"d": d, "n": n, "m": m} | ({"index": index} if scalar else {})
    if found is None or any(found[1][key] != value for key, value in expected.items()):
        raise SystemExit(f"{form.name}: {word:08x} does not decode to {expected}")
    return word


def suite_armv8_fz16(context: Context) -> list[Case]:
    """VFMAL and VFMSL under FZ16: the library widens each half with FPUnpack, which flushes a denormal under FZ16
    (a host FCVT would not), so the JIT, which runs them in the library, and the host's FMLAL must agree."""
    cases = []
    for form_index, name in enumerate(FZ16_FORMS):
        form = context.model.by_name[name]
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        quad = name.endswith("_Q")
        scalar = "_s_" in name
        lanes = 4 if quad else 2
        for chunk, mode in enumerate(FZ16_MODES):
            d, n, m = FZ16_REGISTERS["Q" if quad else "D", scalar][chunk % 4]
            index = (chunk + form_index) % lanes if scalar else 0
            first = (chunk * 5 + form_index * 3) % len(FZ16_LANES)
            group = [FZ16_LANES[(first + 3 * lane) % len(FZ16_LANES)] for lane in range(lanes)]
            if scalar:  # one element of m takes part in every lane: the first lane's, among decoys
                second = [FZ16_DECOYS[(position + chunk) % len(FZ16_DECOYS)] for position in range(lanes)]
                second[index] = group[0][1]
                group = [(lane[0], group[0][1], lane[2]) for lane in group]
            else:
                second = [lane[1] for lane in group]
            registers: dict[int, int] = {}
            for number, halves in ((n, [lane[0] for lane in group]), (m, second)):
                for position, half in enumerate(halves):
                    register, shift = (number, 16 * position) if quad else (  # an S register is half a D register
                        number >> 1, 32 * (number & 1) + 16 * position)
                    registers[register] = registers.get(register, 0) | half << shift
            for position, lane in enumerate(group):
                registers[d + position // 2] = registers.get(d + position // 2, 0) | lane[2] << 32 * (position % 2)
            initial = {"fpscr": mode, "apsr": FLAGS[pattern(len(cases)) >> 8 & 3]}
            initial.update({f"d{register}": value for register, value in registers.items()})
            word = fz16_word(context, form, d, n, m, index)
            cases.append(Case("armv8-fz16", f"{name}/z{chunk}", name, ISA_LETTER[isa], word_of(isa, word), 0,
                              f"zero-{suffix}", initial))
    return cases


# Half precision to a 16-bit integer, lane by lane ---------------------------------------------------------------------

# The instructions: VCVTA, VCVTN, VCVTP and VCVTM to an integer, VCVT to an integer rounding toward zero, and VCVT to
# 16-bit fixed point, in A32 and Thumb, on D and Q registers.
CONVERT16_FORMS = (tuple(f"VCVT{rounding}_asimd_{isa}1_{size}" for rounding in "ANPM" for isa in "AT" for size in "DQ")
                   + tuple(f"VCVT_{kind}_{isa}1_{size}" for kind in ("is", "xs") for isa in "AT" for size in "DQ"))
# Eight half-precision lanes (a D form converts the first four). A lane out of the 16-bit range saturates and is
# invalid, never inexact; a lane before it that was inexact keeps its flag. Each set puts the kinds in a different
# order: an inexact lane (2^-14, one third), an exact one, an overflow (the largest half, infinities, NaNs), and
# denormals, which FZ16 flushes to an exact zero and which are otherwise inexact.
CONVERT16_LANES = (
    (0x0400, 0x3C00, 0x7BFF, 0x0001, 0xBC00, 0x7E00, 0x3555, 0xFBFF),  # inexact, then overflow, then both
    (0x7BFF, 0x0400, 0x3C00, 0x0001, 0x7C00, 0xB555, 0x03FF, 0x4000),  # overflow first
    (0x3555, 0x4248, 0xFC00, 0x0000, 0x8001, 0x7C01, 0x5640, 0xFBFF),  # infinity, a signalling NaN
    (0x0001, 0x03FF, 0x7BFF, 0x8001, 0x83FF, 0xFBFF, 0x3C00, 0x0400),  # denormals, then overflow
    (0x3C00, 0x4000, 0x7BFF, 0xC000, 0x4400, 0xFBFF, 0x4500, 0x4200),  # nothing but exact and overflow
)
# (set of lanes, FPSCR) of each case: FZ16 set and clear, and the alternative half-precision format.
CONVERT16_CASES = ((0, 0x00080000), (0, 0x00000000), (1, 0x00080000), (2, 0x00000000), (3, 0x00080000),
                   (3, 0x00000000), (4, 0x00080000), (0, 0x04080000))
CONVERT16_FRACTION_BITS = (1, 8, 15, 4, 12, 16, 3, 10)
CONVERT16_REGISTERS = {"D": ((16, 4), (21, 31), (0, 9), (27, 13)), "Q": ((16, 4), (20, 30), (0, 10), (26, 12))}


def convert16_word(context: Context, form, d: int, m: int, unsigned: bool, fraction_bits: int) -> int:
    """The form's word converting half-precision lanes of D (or Q) m to 16 bits in d."""
    fields = fields_of(form)
    word = simd.pick_words(context, form, 1)[0][0]
    values = {"D": d >> 4, "Vd": d & 15, "M": m >> 4, "Vm": m & 15}
    if "_asimd_" in form.name:
        values.update({"size": 1, "op": int(unsigned)})
    elif "_is_" in form.name:
        values.update({"size": 1, "op": 2 + int(unsigned)})
    else:
        values.update({"U": int(unsigned), "op": 1, "imm6": 64 - fraction_bits})
    for name, value in values.items():
        word = set_field(word, *fields[name], value)
    found = usable(context, form, word)
    expected = {"d": d, "m": m, "esize": 16, "unsigned": unsigned}
    if "_xs_" in form.name:
        expected.update({"to_fixed": True, "frac_bits": fraction_bits})
    elif "_is_" in form.name:
        expected["to_integer"] = True
    if found is None or any(found[1].get(key) != value for key, value in expected.items()):
        raise SystemExit(f"{form.name}: {word:08x} does not decode to {expected}")
    return word


def suite_armv8_convert16(context: Context) -> list[Case]:
    """Half precision to 16-bit integers: the library converts each lane on the host's H-register forms, which
    saturate and report Invalid Operation for that lane alone."""
    cases = []
    for form_index, name in enumerate(CONVERT16_FORMS):
        form = context.model.by_name[name]
        isa = form.encoding.isa
        suffix = "a" if isa == "A32" else "t"
        quad = name.endswith("_Q")
        for chunk, (lane_set, mode) in enumerate(CONVERT16_CASES):
            d, m = CONVERT16_REGISTERS["Q" if quad else "D"][(chunk + form_index) % 4]
            unsigned = chunk // 2 % 2 == 1
            fraction_bits = CONVERT16_FRACTION_BITS[(chunk + form_index) % 8]
            lanes = CONVERT16_LANES[lane_set][:8 if quad else 4]
            initial = {"fpscr": mode, "apsr": FLAGS[pattern(len(cases)) >> 8 & 3]}
            for register in range(2 if quad else 1):
                initial[f"d{m + register}"] = sum(half << 16 * position for position, half
                                                  in enumerate(lanes[4 * register:4 * register + 4]))
            word = convert16_word(context, form, d, m, unsigned, fraction_bits)
            cases.append(Case("armv8-convert16", f"{name}/c{chunk}", name, ISA_LETTER[isa], word_of(isa, word), 0,
                              f"zero-{suffix}", initial))
    return cases


SUITES = {
    "armv8": suite_armv8,
    "armv8-memory": suite_armv8_memory,
    "armv8-system": suite_armv8_system,
    "armv8-fz16": suite_armv8_fz16,
    "armv8-convert16": suite_armv8_convert16,
}
