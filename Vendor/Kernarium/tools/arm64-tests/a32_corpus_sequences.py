#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The sequences suite: short programs whose instructions an engine may
translate together, so that the state any exit or fault sees has to be the
one the instructions give one by one.

- address: an ADD or SUB of an immediate that computes the base of the VLD1
  or VST1 after it, next to it or with Advanced SIMD data processing between,
  in place or from another register or SP, at the memory suites' addresses
  (inside a line, across the 4 KiB and 16 KiB lines, misaligned, at the end
  of the data region, read-only, unmapped, at zero, and an address that wraps
  around 2^32); and the shapes that keep the two apart: flags set, a core
  register read between, a VFP instruction between, another base register,
  writeback, an alignment hint, a second ADD and a PC base. A copy that
  faults does so with the ADD and everything between retired.
- compare: VCMP and VCMPE of each precision, against a register or zero, and
  the VMRS APSR_nzcv, FPSCR after it, over ordered, equal, unordered and
  signalling operands; Q and GE survive. Also apart, to a core register, and
  either of the two conditional.
- select: the conditional data-processing instructions that set no flags
  (MOV, MVN, ADD, SUB, RSB, AND, ORR, EOR, BIC of an immediate or a register,
  shifted or not, and MOVW) and the conditional VFP VMOV of a register, each
  under conditions that pass and fail; and conditional runs that mix them
  with loads and stores, in A32 and in Thumb IT blocks.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from a32_corpus_control import Prog, cond_flags  # noqa: E402
from a32_corpus_memory import D, READ_ONLY  # noqa: E402
from a32_corpus_plan import FLAGS, Case, Context, decode, pattern  # noqa: E402

MASK = 0xFFFFFFFF
ARM, THUMB = "A", "T"
SUITE = "sequences"
RECIPES = ("edges", "pattern", "edges3", "pattern1", "edges5", "int32")


def checked(context: Context, isa: str, word: int, form: str | None = None) -> str:
    """The inventory form of a word the planner assembled; a word the
    decoder does not accept as intended is a planner bug."""
    result, _ = decode(context, isa, word)
    if result.status == "undefined" or (form is not None and result.form != form):
        raise SystemExit(f"sequences: {isa} {word:08x} decodes as {result.status} {result.form}, not {form}")
    return result.form


class Program:
    """Instructions of one instruction set, with the ordinal of each."""

    def __init__(self, context: Context, state: str):
        self.context = context
        self.state = state
        self.prog = Prog()
        self.forms: list[str] = []

    def a32(self, word: int, form: str | None = None) -> int:
        self.forms.append(checked(self.context, "A32", word, form))
        self.prog.word(word)
        return len(self.forms) - 1

    def t32(self, word: int, form: str | None = None) -> int:
        self.forms.append(checked(self.context, "T32", word, form))
        self.prog.half(word >> 16, word & 0xFFFF)
        return len(self.forms) - 1

    def t16(self, half: int, form: str | None = None) -> int:
        self.forms.append(checked(self.context, "T16", half << 16, form))
        self.prog.half(half)
        return len(self.forms) - 1

    def insn(self, word: int, form: str | None = None) -> int:
        """An instruction given as its A32 encoding (condition 14 when it
        has one) in either instruction set: the Thumb encoding of an Advanced
        SIMD, structure or VFP instruction follows from the A32 one."""
        if self.state == ARM:
            return self.a32(word, form)
        if word >> 25 == 0x79:  # 1111 001U: Advanced SIMD data processing
            return self.t32(0xEF000000 | (word >> 24 & 1) << 28 | word & 0x00FFFFFF, form)
        if word >> 24 == 0xF4:  # 1111 0100: element and structure loads and stores
            return self.t32(0xF9000000 | word & 0x00FFFFFF, form)
        assert word >> 24 & 0xF == 0xE, f"{word:08x}"
        return self.t32(0xEE000000 | word & 0x00FFFFFF, form)

    def case(self, name: str, target: int, number: int, initial: dict) -> Case:
        return Case(SUITE, name, self.forms[target], self.state, self.prog.program(self.state), target,
                    f"{RECIPES[number % len(RECIPES)]}-{'a' if self.state == ARM else 't'}", initial)


# Encodings --------------------------------------------------------------------------------------------


def a32_dp_imm(opcode: int, rd: int, rn: int, imm12: int, cond: int = 14, s: int = 0) -> int:
    return cond << 28 | 0x02000000 | opcode << 21 | s << 20 | rn << 16 | rd << 12 | imm12


def a32_dp_reg(opcode: int, rd: int, rn: int, rm: int, cond: int = 14, shift: int = 0, kind: int = 0) -> int:
    return cond << 28 | opcode << 21 | rn << 16 | rd << 12 | shift << 7 | kind << 5 | rm


ADD, SUB, RSB, AND, ORR, EOR, BIC, MOV, MVN = 4, 2, 3, 0, 12, 1, 14, 13, 15


def t32_add_sub(sub: bool, rd: int, rn: int, imm: int, s: int = 0) -> int:
    """ADD.W/SUB.W (T3) of an immediate below 256, else, or from the PC,
    ADDW/SUBW (T4)."""
    if imm < 256 and rn != 15:
        return (0xF1A00000 if sub else 0xF1000000) | s << 20 | rn << 16 | rd << 8 | imm
    assert not s and imm < 4096
    return ((0xF2A00000 if sub else 0xF2000000) | (imm >> 11 & 1) << 26 | rn << 16 | (imm >> 8 & 7) << 12
            | rd << 8 | imm & 0xFF)


def neon3(base: int, d: int, n: int, m: int, q: int) -> int:
    """An Advanced SIMD three-register word: D:Vd, N:Vn, M:Vm."""
    return (base | (d >> 4 & 1) << 22 | (n & 15) << 16 | (d & 15) << 12 | (n >> 4 & 1) << 7 | q << 6
            | (m >> 4 & 1) << 5 | m & 15)


VMUL_F32, VADD_F32, VORR, VADD_I32 = 0xF3000D10, 0xF2000D00, 0xF2200110, 0xF2200800
TYPE_OF = {1: 0x7, 2: 0xA, 3: 0x6, 4: 0x2}


def structure(store: bool, d: int, n: int, regs: int, size: int, align: int = 0, m: int = 15) -> int:
    """VLD1 or VST1 (multiple single elements) of `regs` D registers from Dd."""
    return ((0xF4000000 if store else 0xF4200000) | (d >> 4 & 1) << 22 | n << 16 | (d & 15) << 12
            | TYPE_OF[regs] << 8 | size << 6 | align << 4 | m)


def vdup32(d: int, t: int, q: int) -> int:
    return 0xEE800B10 | q << 21 | (d & 15) << 16 | t << 12 | (d >> 4 & 1) << 7


def vfp_s(base: int, d: int, m: int, n: int = 0) -> int:
    """A single-precision VFP word with Sd, Sn and Sm."""
    return base | (d & 1) << 22 | (n >> 1) << 16 | (d >> 1) << 12 | (n & 1) << 7 | (m & 1) << 5 | m >> 1


def vfp_d(base: int, d: int, m: int, n: int = 0) -> int:
    """A double-precision VFP word with Dd, Dn and Dm."""
    return base | (d >> 4 & 1) << 22 | (n & 15) << 16 | (d & 15) << 12 | (n >> 4 & 1) << 7 | (m >> 4 & 1) << 5 | m & 15


VCMP = {"S": 0xEEB40A40, "D": 0xEEB40B40, "H": 0xEEB40940}
VCMP_ZERO = {"S": 0xEEB50A40, "D": 0xEEB50B40, "H": 0xEEB50940}
VMOV_REG = {"S": 0xEEB00A40, "D": 0xEEB00B40}
VADD_S = 0xEE300A00


def vfp_reg(precision: str, base: int, d: int, m: int) -> int:
    if precision == "D":
        return vfp_d(base, d, m)
    return vfp_s(base, d, m)


def vmrs(t: int, cond: int = 14) -> int:
    return cond << 28 | 0x0EF10A10 | t << 12


def it(firstcond: int, mask: int = 0x8) -> int:
    return 0xBF00 | firstcond << 4 | mask


def with_cond(word: int, cond: int) -> int:
    return word & 0x0FFFFFFF | cond << 28


# address ----------------------------------------------------------------------------------------------

ADDRESS_SHAPES = ("adj", "neon", "inplace", "sub", "subin", "feeds", "sp",
                  "dup", "vfp", "flags", "other", "twice", "wback", "hint", "pc")
ADDRESS_SCENARIOS = ("a", "u1", "x4", "x4o", "x16", "last", "rox", "ro", "unmapped", "zero", "wrap")
IMMEDIATES = (0x10, 0x40, 0x3FC, 0x8, 0xFF0, 0x100)
FIRST_D = (0, 16, 29, 5)


def target_address(scenario: str, extent: int) -> int | None:
    return {
        "a": D + 0x1040, "u1": D + 0x1041, "x4": D + 0x2000 - extent // 2, "x4o": D + 0x2000 - 3,
        "x16": D + 0x4000 - extent // 2, "last": D + 0x8000 - extent, "rox": D + 0x8000 - extent // 2,
        "ro": READ_ONLY, "unmapped": 0x60000000, "zero": 0,
    }.get(scenario)


def a32_imm(value: int) -> int:
    """The A32 modified-immediate field of a value that has one."""
    for rotation in range(16):
        rotated = (value << 2 * rotation | value >> (32 - 2 * rotation)) & MASK
        if rotated < 256:
            return rotation << 8 | rotated
    raise AssertionError(f"{value:#x} has no A32 immediate")


def add_sub(p: Program, sub: bool, rd: int, rn: int, imm: int, s: int = 0) -> int:
    if p.state == ARM:
        return p.a32(a32_dp_imm(SUB if sub else ADD, rd, rn, a32_imm(imm), s=s))
    if rn == 13 and not sub and not s and imm < 1024 and imm % 4 == 0 and rd < 8:
        return p.t16(0xA800 | rd << 8 | imm >> 2)  # add rd, sp, #imm (ADD_SP_i_T1)
    return p.t32(t32_add_sub(sub, rd, rn, imm, s))


def address_cases(context: Context, results: list[Case]) -> None:
    number = 0
    for state in (ARM, THUMB):
        for store in (False, True):
            for shape_index, shape in enumerate(ADDRESS_SHAPES):
                for scenario_index, scenario in enumerate(ADDRESS_SCENARIOS):
                    rotation = shape_index + scenario_index + (2 if store else 0) + (1 if state == THUMB else 0)
                    regs = 1 + rotation % 4
                    size = (rotation // 2) % 4
                    first = FIRST_D[(rotation // 3 + scenario_index) % 4]
                    if first + regs > 32:
                        first = 32 - regs
                    imm = IMMEDIATES[rotation % len(IMMEDIATES)]
                    extent = 8 * regs
                    if shape == "pc" and scenario != "a":
                        continue
                    if shape == "hint" and scenario not in ("a", "u1", "x4", "unmapped"):
                        continue
                    if store and scenario == "rox":
                        continue  # a store that faults part-way: the straddle suite's
                    if shape == "sp":
                        imm &= 0x3FC
                    results.append(address_case(context, state, store, shape, scenario, regs, size, first,
                                                imm, target_address(scenario, extent), number))
                    number += 1


def address_case(context, state, store, shape, scenario, regs, size, first, imm, address, number):
    p = Program(context, state)
    rd, rn = 3, {"inplace": 3, "subin": 3, "sp": 13, "pc": 15}.get(shape, 5)
    sub = shape in ("sub", "subin")
    if shape == "flags" and imm >= 256:
        imm = 0x20
    initial = {"apsr": FLAGS[pattern(number) >> 8 & 7]}
    # Advanced SIMD registers between the two, apart from the copy's.
    q = (16, 18, 20) if first + regs <= 16 else (0, 2, 4)
    if scenario == "wrap":
        # An ADD past 2^32 to a low address, a SUB below zero to a high one.
        address = 0xFFFFFFF0 if sub else 0x10
    # The base register value that makes the copy start at `address`.
    start = (address + imm if sub else address - imm) & MASK
    if shape == "twice":
        start = (start - 8) & MASK
    if rn != 15:
        initial[f"r{rn}"] = start
    add_sub(p, sub, rd, rn, imm, s=1 if shape == "flags" else 0)
    if shape == "twice":
        add_sub(p, False, rd, rd, 8)
    if shape in ("neon", "inplace", "subin"):
        p.insn(neon3(VMUL_F32, q[0], q[1], q[2], 1))
    if shape == "neon":
        p.insn(neon3(VADD_F32, q[1], q[0], q[2], 1))
    if shape == "subin":
        p.insn(neon3(VADD_I32, q[2], q[1], q[0], 0))
    if shape == "feeds":
        p.insn(neon3(VORR, first, first ^ 1, first ^ 1, 0))  # writes the copy's first register
    if shape == "dup":
        p.insn(vdup32(q[0], rd, 1))  # reads the ADD's result
    if shape == "vfp":
        p.insn(vfp_s(VADD_S, 2, 3, 4))
    base = rd
    if shape == "other":
        base = rn
        initial[f"r{rn}"] = address
    align = 0
    if shape == "hint":
        align, size = 1, 3  # vld1.64 [rN:64], which every register count accepts
    copy = p.insn(structure(store, first, base, regs, size, align, 13 if shape == "wback" else 15))
    name = f"{'vst1' if store else 'vld1'}/{shape}.{scenario}.{'a' if state == ARM else 't'}"
    return p.case(name, copy, number, initial)


# compare ----------------------------------------------------------------------------------------------

# (Fd, Fm) pairs of each precision: less, greater, equal, zeros of both
# signs, a quiet NaN, a signalling NaN, infinities, a denormal against zero
# (equal under flush-to-zero) and minus infinity.
COMPARE_VALUES = {
    "S": ((0x3F800000, 0x40000000), (0x40000000, 0x3F800000), (0x3F800000, 0x3F800000), (0x80000000, 0x00000000),
          (0x7FC00000, 0x3F800000), (0x3F800000, 0x7F800001), (0x7F800000, 0x7F800000), (0x00000001, 0x00000000),
          (0xFF800000, 0xBF800000)),
    "D": ((0x3FF0000000000000, 0x4000000000000000), (0x4000000000000000, 0x3FF0000000000000),
          (0xBFF0000000000000, 0xBFF0000000000000), (0x8000000000000000, 0x0000000000000000),
          (0x7FF8000000000000, 0x3FF0000000000000), (0x3FF0000000000000, 0x7FF0000000000001),
          (0x7FF0000000000000, 0x7FF0000000000000), (0x0000000000000001, 0x0000000000000000),
          (0xFFF0000000000000, 0xBFF0000000000000)),
    "H": ((0x3C00, 0x4000), (0x4000, 0x3C00), (0x3C00, 0x3C00), (0x8000, 0x0000), (0x7E00, 0x3C00),
          (0x3C00, 0x7C01), (0x7C00, 0x7C00), (0x0001, 0x0000), (0xFC00, 0xBC00)),
}
COMPARE_FPSCR = (0x00000000, 0x01000000, 0x0000009F, 0x02000000, 0x00C00000, 0x03C0001F)
COMPARE_REGISTERS = {"S": ((0, 1), (31, 6), (17, 30)), "D": ((1, 2), (31, 16), (7, 20)),
                     "H": ((0, 1), (31, 6), (17, 30))}


def place(initial: dict, precision: str, register: int, value: int, recipe_d: dict) -> None:
    """Put a value in S, D or H register `register`, keeping the rest of its D register."""
    if precision == "D":
        initial[f"d{register}"] = value
        return
    key = f"d{register >> 1}"
    shift = 32 * (register & 1)
    width = 0xFFFFFFFF if precision == "S" else 0xFFFF
    current = initial.get(key, recipe_d[key])
    initial[key] = current & ~(width << shift) & 0xFFFFFFFFFFFFFFFF | value << shift


def compare_cases(context: Context, results: list[Case]) -> None:
    number = 0
    for state in (ARM, THUMB):
        letter = "a" if state == ARM else "t"
        for precision in ("S", "D", "H"):
            for zero in (False, True):
                for signalling in (0, 1):
                    for index, (fd, fm) in enumerate(COMPARE_VALUES[precision]):
                        if zero and index >= 6:
                            continue
                        d, m = COMPARE_REGISTERS[precision][(index + signalling) % 3]
                        p = Program(context, state)
                        base = (VCMP_ZERO if zero else VCMP)[precision] | signalling << 7
                        word = vfp_d(base, d, 0 if zero else m) if precision == "D" else \
                            vfp_s(base, d, 0 if zero else m)
                        target = p.insn(word)
                        p.insn(vmrs(15))
                        recipe = f"{RECIPES[number % len(RECIPES)]}-{letter}"
                        initial = {"apsr": FLAGS[(number * 5 + index) % len(FLAGS)],
                                   "fpscr": COMPARE_FPSCR[(number + index) % len(COMPARE_FPSCR)]}
                        place(initial, precision, d, fd, context.recipes[recipe])
                        if not zero:
                            place(initial, precision, m, fm, context.recipes[recipe])
                        kind = ("vcmpe" if signalling else "vcmp") + ("z" if zero else "")
                        results.append(p.case(f"{kind}.{precision.lower()}/{index}.{letter}", target, number,
                                              initial))
                        number += 1
    # The shapes that keep the compare and the transfer apart.
    for state in (ARM, THUMB):
        letter = "a" if state == ARM else "t"
        for index, (fd, fm) in enumerate(COMPARE_VALUES["S"][:6]):
            for shape in ("apart", "core", "vcmpcc", "vmrscc"):
                if state == THUMB and shape in ("vcmpcc", "vmrscc"):
                    shape = "it" + shape[:-2]
                p = Program(context, state)
                passes = index % 2 == 0
                cond = (0, 1, 10, 11, 12, 13)[index]
                flags = cond_flags(cond, passes, number)
                compare = vfp_s(VCMP["S"], 0, 1)
                if shape == "vcmpcc":
                    target = p.a32(with_cond(compare, cond))
                elif shape == "itvcmp":
                    p.t16(it(cond))
                    target = p.insn(compare)
                else:
                    target = p.insn(compare)
                if shape == "apart":
                    add_sub(p, False, 4, 4, 1)
                if shape == "vmrscc":
                    p.a32(vmrs(15, cond))
                elif shape == "itvmrs":
                    p.t16(it(cond))
                    p.insn(vmrs(15))
                else:
                    p.insn(vmrs(3 if shape == "core" else 15))
                recipe = f"{RECIPES[number % len(RECIPES)]}-{letter}"
                initial = {"apsr": flags | FLAGS[8 + index % 4] & 0x0FFFFFFF,
                           "fpscr": COMPARE_FPSCR[index % len(COMPARE_FPSCR)] | 0x30000000}
                place(initial, "S", 0, fd, context.recipes[recipe])
                place(initial, "S", 1, fm, context.recipes[recipe])
                results.append(p.case(f"vcmp.s/{shape}.{index}.{letter}", target, number, initial))
                number += 1


# select -----------------------------------------------------------------------------------------------

SELECT_OPS = (("mov", MOV), ("mvn", MVN), ("add", ADD), ("sub", SUB), ("rsb", RSB), ("and", AND),
              ("orr", ORR), ("eor", EOR), ("bic", BIC))
SELECT_IMMEDIATES = (0xFF, 0x3FC, 0xF000000F, 0x1, 0x80000000, 0x10000)
# (rd, rn, rm): distinct, rd = rn, rd = rm, high registers, rn = PC.
SELECT_REGISTERS = ((2, 7, 9), (4, 4, 6), (8, 1, 8), (14, 12, 11), (0, 15, 3), (10, 5, 5))


def select_cases(context: Context, results: list[Case]) -> None:
    number = 0
    for name, opcode in SELECT_OPS:
        for shape in ("imm", "reg", "shifted"):
            for slot in range(4):
                cond = (number + slot * 5) % 14
                rd, rn, rm = SELECT_REGISTERS[(number + slot) % len(SELECT_REGISTERS)]
                if opcode in (MOV, MVN):
                    rn = 0
                for passes in (True, False):
                    p = Program(context, ARM)
                    if shape == "imm":
                        word = a32_dp_imm(opcode, rd, rn, a32_imm(SELECT_IMMEDIATES[(number + slot) % 6]), cond)
                    else:
                        amount, kind = ((0, 0) if shape == "reg" else (1 + slot * 7 % 31, slot % 4))
                        word = a32_dp_reg(opcode, rd, rn, rm, cond, amount, kind)
                    target = p.a32(word)
                    initial = {"apsr": cond_flags(cond, passes, number) | FLAGS[(number + slot) % len(FLAGS)]
                               & 0x0FFFFFFF}
                    results.append(p.case(f"{name}.{shape}/{slot}.c{cond}{'p' if passes else 'f'}", target,
                                          number, initial))
                    number += 1
    # MOVW.
    for cond in range(14):
        for passes in (True, False):
            p = Program(context, ARM)
            rd = (cond * 3) % 15
            imm16 = pattern(cond, 16)
            target = p.a32(cond << 28 | 0x03000000 | (imm16 >> 12) << 16 | rd << 12 | imm16 & 0xFFF)
            results.append(p.case(f"movw/c{cond}{'p' if passes else 'f'}", target, number,
                                  {"apsr": cond_flags(cond, passes, number)}))
            number += 1
    # VMOV of a register, A32 and in an IT block.
    for state in (ARM, THUMB):
        letter = "a" if state == ARM else "t"
        for precision in ("S", "D"):
            for cond in range(14):
                for passes in (True, False):
                    p = Program(context, state)
                    d, m = ((0, 1), (31, 16), (17, 30), (5, 20))[(cond + passes) % 4]
                    word = vfp_reg(precision, VMOV_REG[precision], d, m)
                    if state == ARM:
                        target = p.a32(with_cond(word, cond))
                    else:
                        p.t16(it(cond))
                        target = p.insn(word)
                    results.append(p.case(f"vmov.{precision.lower()}/c{cond}{'p' if passes else 'f'}.{letter}",
                                          target, number, {"apsr": cond_flags(cond, passes, number)}))
                    number += 1
    # Runs that mix selects with conditional loads and stores. CMP sets the
    # flags: r0 against r1.
    pairs = ((1, 2), (2, 1), (5, 5), (0x80000000, 1))
    for index, (r0, r1) in enumerate(pairs):
        p = Program(context, ARM)
        p.a32(0xE1500001)  # cmp r0, r1
        target = p.a32(a32_dp_imm(ADD, 2, 2, 1, 11))  # addlt r2, r2, #1
        p.a32(0xB59D4008)  # ldrlt r4, [sp, #8]
        p.a32(a32_dp_imm(MOV, 5, 0, 3, 11))  # movlt r5, #3
        p.a32(0xA58D6004)  # strge r6, [sp, #4]
        p.a32(a32_dp_reg(ADD, 7, 7, 8, 10))  # addge r7, r7, r8
        p.a32(0xA3019234)  # movwge r9, #0x1234
        p.a32(with_cond(vfp_s(VMOV_REG["S"], 0, 1), 11))  # vmovlt.f32 s0, s1
        p.a32(0xA59DA000)  # ldrge r10, [sp]
        p.a32(a32_dp_imm(MOV, 3, 0, 0x40, 11))  # movlt r3, #0x40
        p.a32(0xB79D4003)  # ldrlt r4, [sp, r3]
        p.a32(a32_dp_reg(ADD, 3, 3, 4, 11))  # addlt r3, r3, r4
        results.append(p.case(f"run/{index}.a", target, number, {"r0": r0, "r1": r1}))
        number += 1
        p = Program(context, THUMB)
        p.t16(0x4288)  # cmp r0, r1
        p.t16(it(11, 0xA))  # itte lt
        target = p.t32(t32_add_sub(False, 2, 2, 1))  # addlt.w r2, r2, #1
        p.t32(0xF8DD4008)  # ldrlt.w r4, [sp, #8]
        p.t16(0x2503)  # movge r5, #3
        p.t16(it(11, 0xB))  # ittet lt
        p.insn(vfp_s(VMOV_REG["S"], 0, 1))  # vmovlt.f32 s0, s1
        p.t16(0x9e01)  # ldrlt r6, [sp, #4]
        p.t32(t32_add_sub(True, 7, 7, 0x123))  # subwge r7, r7, #0x123
        p.insn(vfp_d(VMOV_REG["D"], 3, 4))  # vmovlt.f64 d3, d4
        results.append(p.case(f"run/{index}.t", target, number, {"r0": r0, "r1": r1}))
        number += 1


def suite_sequences(context: Context) -> list[Case]:
    results: list[Case] = []
    address_cases(context, results)
    compare_cases(context, results)
    select_cases(context, results)
    return results


SUITES = {SUITE: suite_sequences}
