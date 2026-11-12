#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The control-flow suites of the AArch32 instruction corpus.

A program that changes the PC needs somewhere defined to land. Every program
here ends in an explicit tail, a branch to the oracle's exit stub of the
instruction set it is in (`b` to 0x4f000800 in A32, `b.w` to 0x4f000900 in
Thumb). The instruction under test jumps to that tail over a filler that adds
one to r11, so a taken branch and a fall-through differ in the final state.
The planner finds the instruction words that reach the tail by solving the
decoder's own operand: probing which word bits the decoded offset follows (it
is affine over GF(2), including the J1/J2 XOR of the Thumb branches) and
solving for the offset the layout needs.

- branch: B, BL, BLX and BX in both instruction sets, conditional branches
  under passing and failing flags, CBZ/CBNZ, TBB/TBH (also in an IT block
  whose condition passes and fails), loads and ALU instructions that write
  the PC (the multiple loads also under a condition that fails and passes),
  interworking in both directions and jumps to unmapped addresses;
- conditions: every condition against every NZCV, every IT block, and runs
  of conditional instructions of two to eleven stream words that a skip steps
  over, under every condition;
- pc: instructions that read the PC, among them the PC as a register operand
  under a condition that fails and passes.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import struct

from a32_corpus_memory import MULTIPLE, mnemonic, multiple_mode, solve, solve_multiple  # noqa: E402
from a32_corpus_plan import (  # noqa: E402
    FEATURES, Case, Context, candidate_words, condition_passes, corpus, fields_of, pattern,
    set_field, usable,
)

MASK = 0xFFFFFFFF
FILL_A = 0xE28BB001  # add r11, r11, #1
FILL_T = (0xF10B, 0x0B01)  # add.w r11, r11, #1
FILL2_T = (0xF10A, 0x0A01)  # add.w r10, r10, #1
NOP_T = 0xBF00
ARM, THUMB = "A", "T"
CODE = corpus.CODE_BASE
D = corpus.DATA_BASE


# Programs ----------------------------------------------------------------------------


class Prog:
    """A program under assembly: halfwords with byte offsets."""

    def __init__(self):
        self.halves: list[int] = []

    @property
    def here(self) -> int:
        return 2 * len(self.halves)

    def half(self, *values) -> "Prog":
        self.halves += list(values)
        return self

    def word(self, value: int) -> "Prog":
        self.halves += [value & 0xFFFF, value >> 16]
        return self

    def insn(self, word: int, isa: str) -> "Prog":
        """An instruction word: A32, or hw1 << 16 | hw2 (hw2 unused for T16)."""
        if isa == "A32":
            return self.word(word)
        return self.half(word >> 16) if isa == "T16" else self.half(word >> 16, word & 0xFFFF)

    def fill(self, state: str, count: int = 1) -> "Prog":
        for _ in range(count):
            if state == ARM:
                self.word(FILL_A)
            else:
                self.half(*FILL_T)
        return self

    def align(self) -> "Prog":
        if self.here % 4:
            self.half(NOP_T)
        return self

    def tail(self, state: str) -> "Prog":
        """The exit tail of an instruction set at the current offset."""
        if state == ARM:
            assert self.here % 4 == 0
            return self.word(arm_tail(self.here))
        return self.half(*thumb_tail(self.here))

    def program(self, state: str) -> tuple[int, ...]:
        halves = list(self.halves)
        if state == THUMB:
            return tuple(halves)
        halves += [NOP_T] * (len(halves) % 2)
        return tuple(halves[i] | halves[i + 1] << 16 for i in range(0, len(halves), 2))


def arm_tail(offset: int) -> int:
    """`b ARM_EXIT` at a byte offset of the program."""
    distance = corpus.ARM_EXIT - (CODE + offset + 8)
    return 0xEA000000 | distance >> 2 & 0xFFFFFF


def thumb_tail(offset: int) -> tuple[int, int]:
    """`b.w THUMB_EXIT` at a byte offset of the program (B.W, encoding T4)."""
    distance = corpus.THUMB_EXIT - (CODE + offset + 4) & 0x1FFFFFF
    sign = distance >> 24 & 1
    i1 = distance >> 23 & 1
    i2 = distance >> 22 & 1
    j1 = (i1 ^ 1 ^ sign) & 1
    j2 = (i2 ^ 1 ^ sign) & 1
    first = 0xF000 | sign << 10 | distance >> 12 & 0x3FF
    second = 0x9000 | j1 << 13 | j2 << 11 | distance >> 1 & 0x7FF
    return first, second


def recipe_of(index: int, state: str) -> str:
    return ("edges", "pattern", "edges3", "pattern1")[index % 4] + ("-t" if state == THUMB else "-a")


def make(results, suite, name, form, state, prog, initial, executed=0, check=True, target=0, **extra) -> Case:
    """A case whose program branches: `executed` fillers run, and with them
    the branch and the tail retire."""
    if "fault_retired" not in extra:
        extra.setdefault("retired", 2 + executed)
        extra["path"] = executed if check else None
    case = Case(suite, name, form, state, prog.program(state), target,
                recipe_of(len(results), state), initial, straight=False, **extra)
    results.append(case)
    return case


# Solving an operand ----------------------------------------------------------------------


class Affine:
    """An operand of a form as an affine function of the word's free bits."""

    def __init__(self, context: Context, form, base: int, operand: str):
        self.context, self.form, self.base, self.operand = context, form, base, operand
        self.isa = form.encoding.isa
        self.constant = self.value(base)
        row = next(row for row in context.model.rows[self.isa]
                   if row.kind == "instruction" and row.matches(base)
                   and context.model.forms[row.form] is form)
        universe = 0xFFFF0000 if self.isa == "T16" else MASK
        self.columns: dict[int, int] = {}
        if self.constant is None:
            return
        for bit in range(32):
            if not universe >> bit & 1 or row.mask >> bit & 1 or (self.isa == "A32" and bit >= 28):
                continue
            probe = self.value(base ^ 1 << bit)
            if probe is not None:
                self.columns[bit] = (probe ^ self.constant) & MASK

    def value(self, word: int):
        result, operands = self.context.reference.operands(self.isa, word, 0, FEATURES)
        if result.status == "undefined" or result.form != self.form.name:
            return None
        value = operands.get(self.operand)
        return None if value is None else int(value) & MASK

    def solve(self, want: int):
        """A word of the form whose operand is `want`, or None."""
        if self.constant is None:
            return None
        need = (want ^ self.constant) & MASK
        basis: list[tuple[int, int]] = []  # (column value, word bits), by leading bit
        for bit, column in self.columns.items():
            value, bits = column, 1 << bit
            for other, other_bits in basis:
                if value >> (other.bit_length() - 1) & 1:
                    value ^= other
                    bits ^= other_bits
            if value:
                basis.append((value, bits))
                basis.sort(reverse=True)
        word = self.base
        for value, bits in basis:
            if need >> (value.bit_length() - 1) & 1:
                need ^= value
                word ^= bits
        if need:
            return None
        return word if self.value(word) == want & MASK else None


def form_named(context: Context, name: str):
    return context.model.by_name[name]


def base_word(context: Context, form, require=None, allow_pc=True):
    """A decodable word of the form (optionally satisfying `require(operands)`)."""
    for word in candidate_words(context, form, 12):
        found = usable(context, form, word, allow_pc)
        if found is not None and (require is None or require(found[1])):
            return found
    return None


def cond_flags(condition: int, passes: bool, seed: int) -> int:
    """NZCV bits (in the APSR) for which the condition passes or fails."""
    start = pattern(seed) & 15
    for step in range(16):
        nzcv = (start + step) & 15
        if condition_passes(condition, nzcv << 28) == passes:
            return nzcv << 28
    raise AssertionError("no flags")


# Direct branches -----------------------------------------------------------------------------


def a32_branches(context, suite, results):
    for name in ("B_A1", "BL_i_A1"):
        form = form_named(context, name)
        affine = Affine(context, form, base_word(context, form)[0], "imm32")
        for condition in range(15):
            for passes in (True, False):
                if condition == 14 and not passes:
                    continue
                for fillers in ((1, 3) if condition < 14 else (0, 1, 2, 3, 5)):
                    word = affine.solve(4 + 4 * fillers - 8)
                    word = set_field(word, 28, 4, condition)
                    prog = Prog().word(word).fill(ARM, fillers).tail(ARM)
                    flags = cond_flags(condition, passes, 7 * condition)
                    make(results, suite, f"{name}/c{condition}{'p' if passes else 'f'}.{fillers}",
                         name, ARM, prog, {"apsr": flags}, 0 if passes else fillers)


def thumb_branches(context, suite, results):
    for name, conditional in (("B_T1", True), ("B_T2", False), ("B_T3", True), ("B_T4", False),
                              ("BL_i_T1", False)):
        form = form_named(context, name)
        isa = form.encoding.isa
        affine = Affine(context, form, base_word(context, form)[0], "imm32")
        size = 2 if isa == "T16" else 4
        for condition in (range(14) if conditional else (None,)):
            for passes in ((True, False) if conditional else (True,)):
                for fillers in ((1, 3) if conditional else (0, 1, 2, 3, 6, 40)):
                    word = affine.solve(size + 4 * fillers - 4)  # the PC reads as the address + 4
                    if word is None:
                        continue
                    if conditional:
                        word = set_field(word, *fields_of(form)["cond"], condition)
                    prog = Prog().insn(word, isa).fill(THUMB, fillers).tail(THUMB)
                    flags = cond_flags(condition, passes, 11 * len(results)) if conditional else 0
                    tag = f"c{condition}{'p' if passes else 'f'}" if conditional else "x"
                    make(results, suite, f"{name}/{tag}.{fillers}", name, THUMB, prog, {"apsr": flags},
                         0 if passes else fillers)
    # BLX (immediate): A32 into Thumb and Thumb into A32, at both halfword parities.
    form = form_named(context, "BL_i_A2")
    affine = Affine(context, form, base_word(context, form)[0], "imm32")
    for lead in (0, 1):
        word = affine.solve(8 + 2 * lead - 8)
        prog = Prog().word(word).fill(ARM)
        if lead:
            prog.half(NOP_T)
        make(results, suite, f"BL_i_A2/h{lead}", "BL_i_A2", ARM, prog.tail(THUMB), {})
    form = form_named(context, "BL_i_T2")
    affine = Affine(context, form, base_word(context, form)[0], "imm32")
    for lead in (0, 1):
        prog = Prog().half(*[NOP_T] * lead)
        position = prog.here
        landing = (position + 8 + 3) & ~3
        word = affine.solve(landing - ((position + 4) & ~3))
        prog.insn(word, "T32").fill(THUMB)
        while prog.here < landing:
            prog.half(NOP_T)
        make(results, suite, f"BL_i_T2/l{lead}", "BL_i_T2", THUMB, prog.tail(ARM), {}, target=lead,
             retired=2 + lead)  # the leading NOPs, BLX and the branch to the stub


def register_branches(context, suite, results):
    """BX, BLX (register) and BXJ to a tail of each instruction set."""
    for name in ("BX_A1", "BLX_r_A1", "BXJ_A1"):
        form = form_named(context, name)
        used = 0
        for word in candidate_words(context, form, 12):
            found = usable(context, form, word, True)
            if found is None or found[1]["m"] in (13, 14, 15) or used >= 3:
                continue
            used += 1
            for state in (ARM, THUMB):
                prog = Prog().word(found[0]).fill(ARM)
                landing = prog.here
                make(results, suite, f"{name}/{used}{state}", name, ARM, prog.tail(state),
                     {f"r{found[1]['m']}": CODE + landing + (state == THUMB)})
        found = base_word(context, form, lambda ops: ops["m"] not in (13, 14, 15))
        for condition in (0, 1):
            prog = Prog().word(set_field(found[0], 28, 4, condition)).fill(ARM)
            landing = prog.here
            make(results, suite, f"{name}/skip{condition}", name, ARM, prog.tail(ARM),
                 {"apsr": cond_flags(condition, False, 99 + condition),
                  f"r{found[1]['m']}": CODE + landing}, 1)
    for name in ("BX_T1", "BLX_r_T1", "BXJ_T1"):
        form = form_named(context, name)
        isa = form.encoding.isa
        used = 0
        for word in candidate_words(context, form, 12):
            found = usable(context, form, word, True)
            if found is None or found[1]["m"] in (13, 14, 15) or used >= 3:
                continue
            used += 1
            for state in (ARM, THUMB):
                prog = Prog().insn(found[0], isa)
                if state == ARM:
                    prog.align()
                prog.fill(THUMB)
                landing = prog.here
                make(results, suite, f"{name}/{used}{state}", name, THUMB, prog.tail(state),
                     {f"r{found[1]['m']}": CODE + landing + (state == THUMB)})


def compare_branches(context, suite, results):
    for name in ("CBZ_T1", "CBNZ_T1"):
        form = form_named(context, name)
        found = base_word(context, form)
        affine = Affine(context, form, found[0], "imm32")
        for fillers in (1, 2, 5):
            word = affine.solve(2 + 4 * fillers - 4)
            if word is None:
                continue
            for number in (0, 1, 0x80000000):
                prog = Prog().insn(word, "T16").fill(THUMB, fillers).tail(THUMB)
                taken = (number == 0) == (name == "CBZ_T1")
                make(results, suite, f"{name}/f{fillers}v{number:x}", name, THUMB, prog,
                     {f"r{found[1]['n']}": number}, 0 if taken else fillers)


def table_branches(context, suite, results):
    for name, scale in (("TBB_T1", 1), ("TBH_T1", 2)):
        form = form_named(context, name)
        fields = fields_of(form)
        base = base_word(context, form)[0]
        # Halfword distances from the table: the tail, filler 2, both fillers, the tail.
        entries = [6, 4, 2, 6][:4 // scale]
        table = b"".join(bytes([e]) if scale == 1 else struct.pack("<H", e) for e in entries)
        for register in (15, 1):
            word = set_field(set_field(base, *fields["Rn"], register), *fields["Rm"], 2)
            for index in range(len(entries)):
                prog = Prog().insn(word, "T32")
                prog.half(*[table[i] | table[i + 1] << 8 for i in range(0, len(table), 2)])
                prog.half(*FILL_T).half(*FILL2_T).tail(THUMB)
                initial = {"r2": index}
                patches = []
                if register != 15:
                    initial["r1"] = D + 0x100
                    patches = [(0x100, table)]
                make(results, suite, f"{name}/r{register}i{index}", name, THUMB, prog, initial,
                     (0, 1, 2, 0)[index], data_patches=patches)


def conditional_table_branches(context, suite, results):
    """TBB and TBH under IT EQ, from a table in the data region: skipped, they fall through both
    fillers; taken, they leave instructions behind them in their block."""
    for name, scale in (("TBB_T1", 1), ("TBH_T1", 2)):
        form = form_named(context, name)
        fields = fields_of(form)
        base = base_word(context, form)[0]
        entries = [4, 2, 0, 4][:4 // scale]
        table = b"".join(bytes([e]) if scale == 1 else struct.pack("<H", e) for e in entries)
        word = set_field(set_field(base, *fields["Rn"], 1), *fields["Rm"], 2)
        for passes in (False, True):
            for index in range(len(entries)) if passes else (0,):
                prog = Prog().half(0xBF08).insn(word, "T32").half(*FILL_T).half(*FILL2_T).tail(THUMB)
                executed = (0, 1, 2, 0)[index] if passes else 2
                make(results, suite, f"{name}/it-eq{'p' if passes else 'f'}i{index}", name, THUMB, prog,
                     {"r1": D + 0x100, "r2": index, "apsr": 0x40000000 if passes else 0}, executed,
                     target=1, retired=3 + executed, data_patches=[(0x100, table)])


# Loads and ALU instructions that write the PC ----------------------------------------------------------


def force_pc_register(form, word: int):
    """The word with its destination register (Rt) set to PC, or None."""
    fields = fields_of(form)
    if "Rt" in fields and fields["Rt"][1] == 4:
        return set_field(word, *fields["Rt"], 15)
    return None


def force_pc_in_list(form, word: int):
    fields = fields_of(form)
    if "register_list" in fields and fields["register_list"][1] == 16:
        return set_field(word, fields["register_list"][0] + 15, 1, 1)
    if "P" in fields and "register_list" in fields:  # the Thumb forms keep it in a bit of its own
        return set_field(word, *fields["P"], 1)
    return None


def pc_loads(context, suite, results):
    for form in context.model.forms:
        if (form.encoding is None or form.level != "base" or "load_pc" not in form.traits
                or form.name.startswith("LDR_l")):
            continue
        isa = form.encoding.isa
        state = ARM if isa == "A32" else THUMB
        multiple = MULTIPLE.fullmatch(mnemonic(form.name)) is not None
        count = 0
        for word in candidate_words(context, form, 16):
            if count >= 4:
                break
            forced = force_pc_in_list(form, word) if multiple else force_pc_register(form, word)
            if forced is None:
                continue
            if isa == "A32":
                forced = set_field(forced, 28, 4, 14)
            found = usable(context, form, forced, True)
            if found is None:
                continue
            operands = found[1]
            address = D + 0x1040
            if multiple:
                registers = bin(operands["registers"]).count("1")
                aim = solve_multiple(operands, multiple_mode(form.name), address)
                slot = address + 4 * (registers - 1)
            else:
                aim = solve(operands, address, 0, count)
                slot = address
            if aim is None:
                continue
            for target_state in (ARM, THUMB):
                count += 1
                prog = Prog().insn(found[0], isa).fill(state)
                if target_state == ARM:
                    prog.align()
                landing = prog.here
                make(results, suite, f"{form.name}/{count}{target_state}", form.name, state,
                     prog.tail(target_state), aim, check=False,
                     data_patches=[(slot - D, struct.pack("<I", CODE + landing + (target_state == THUMB)))])


def conditional_pc_loads(context, suite, results):
    """The multiple loads of the PC under EQ (in A32) or IT EQ (in Thumb): a failing condition
    runs the filler, a passing one lands past it."""
    for form in context.model.forms:
        if (form.encoding is None or form.level != "base" or "load_pc" not in form.traits
                or MULTIPLE.fullmatch(mnemonic(form.name)) is None):
            continue
        isa = form.encoding.isa
        state = ARM if isa == "A32" else THUMB
        for word in candidate_words(context, form, 16):
            forced = force_pc_in_list(form, word)
            if forced is None:
                continue
            if isa == "A32":
                forced = set_field(forced, 28, 4, 14)
            found = usable(context, form, forced, True)
            if found is None:
                continue
            word, operands = found
            address = D + 0x1040
            aim = solve_multiple(operands, multiple_mode(form.name), address)
            slot = address + 4 * (bin(operands["registers"]).count("1") - 1)
            for passes in (False, True):
                prog = Prog()
                if state == THUMB:
                    prog.half(0xBF08)
                prog.insn(set_field(word, 28, 4, 0) if state == ARM else word, isa).fill(state)
                landing = prog.here
                apsr = cond_flags(0, passes, len(results))
                make(results, suite, f"{form.name}/eq{'p' if passes else 'f'}", form.name, state,
                     prog.tail(state), {**aim, "apsr": apsr}, target=1 if state == THUMB else 0,
                     check=False, retired=(state == THUMB) + (2 if passes else 3),
                     data_patches=[(slot - D, struct.pack("<I", CODE + landing + (state == THUMB)))])
            break


# ALU instructions with the PC as destination: the result must be the landing address.
ARM_PC_WRITES = (
    ("MOV_r_A1", 0xE1A0F002, lambda t: {"r2": t}),
    ("ADD_r_A1", 0xE081F002, lambda t: {"r1": 0, "r2": t}),
    ("ORR_r_A1", 0xE181F002, lambda t: {"r1": 0, "r2": t}),
    ("EOR_r_A1", 0xE021F002, lambda t: {"r1": 0, "r2": t}),
    ("RSB_r_A1", 0xE061F002, lambda t: {"r1": 0, "r2": t}),
    ("SUB_r_A1", 0xE042F001, lambda t: {"r2": t, "r1": 0}),
    ("AND_r_A1", 0xE001F002, lambda t: {"r1": t, "r2": MASK}),
    ("BIC_r_A1", 0xE1C1F002, lambda t: {"r1": t, "r2": 0}),
    ("MVN_r_A1", 0xE1E0F002, lambda t: {"r2": ~t & MASK}),
    ("ADC_r_A1", 0xE0A1F002, lambda t: {"r1": 0, "r2": t, "apsr": 0}),
    ("RSC_r_A1", 0xE0E1F002, lambda t: {"r1": 0, "r2": t, "apsr": 0x20000000}),
    ("SUB_i_A1", 0xE242F004, lambda t: {"r2": t + 4}),
)


def alu_pc_writes(context, suite, results):
    for name, word, setup in ARM_PC_WRITES:
        for state in (ARM, THUMB):
            prog = Prog().word(word).fill(ARM)
            landing = prog.here
            make(results, suite, f"{name}/{state}", name, ARM, prog.tail(state),
                 setup(CODE + landing + (state == THUMB)))
        # A condition that fails leaves the PC alone: the filler runs.
        prog = Prog().word(set_field(word, 28, 4, 0)).fill(ARM)
        make(results, suite, f"{name}/skip", name, ARM, prog.tail(ARM),
             {**setup(CODE + 8), "apsr": cond_flags(0, False, 13)}, 1)
    # Thumb: MOV pc, rm and ADD pc, rm (the PC reads as the address + 4); bit 0 is ignored.
    for name, word in (("MOV_r_T1", 0x468F), ("MOV_r_T1", 0x46B7), ("ADD_r_T2", 0x448F),
                       ("ADD_r_T2", 0x44B7)):
        m = word >> 3 & 15
        for bit in (0, 1):
            prog = Prog().half(word).fill(THUMB)
            landing = prog.here
            value = CODE + landing + bit if name == "MOV_r_T1" else landing - 4
            make(results, suite, f"{name}/m{m}b{bit}", name, THUMB, prog.tail(THUMB), {f"r{m}": value})


def unmapped_targets(context, suite, results):
    """The PC leaves the program for an address nothing maps: the fault is an
    instruction fetch."""
    for name, isa, word in (("BX_A1", "A32", 0xE12FFF11), ("BX_T1", "T16", 0x47080000)):
        state = ARM if isa == "A32" else THUMB
        for target in (0x60000000, 0x60000001, 0x4EFFFFF0, CODE + corpus.CODE_SIZE + 0x10, 0):
            make(results, suite, f"{name}/{target:x}", name, state, Prog().insn(word, isa),
                 {"r1": target}, fault_retired=1)


# Conditions and IT blocks ------------------------------------------------------------------------------

# One A32 instruction per behavior a failing condition must suppress.
CONDITIONAL_A32 = (
    ("add", 0x028BB001, {}),  # add r11, r11, #1
    ("adds", 0x0290A102, {"r0": 0x80000000}),  # adds r10, r0, #0x80000000: Z, C and V
    ("ldr", 0x05941000, {"r4": 0}),  # ldr r1, [r4]: faults at address zero when it runs
    ("str", 0x05841000, {"r4": corpus.READ_ONLY_BASE}),  # str r1, [r4]: faults in read-only memory
)


def condition_table(context, suite, results):
    for name, body, setup in CONDITIONAL_A32:
        for condition in range(15):
            for nzcv in range(16):
                flags = nzcv << 28
                passes = condition_passes(condition, flags)
                word = condition << 28 | body
                case = Case(suite, f"{name}/c{condition}n{nzcv:x}", "ADD_i_A1" if name == "add" else
                            "ADDS_i_A1" if name == "adds" else "LDR_i_A1_off" if name == "ldr" else
                            "STR_i_A1_off", ARM, (word,), 0, recipe_of(len(results), ARM),
                            {"apsr": flags, **setup})
                if name == "add":
                    case.expect = {"r11": 1 if passes else 0}
                results.append(case)


def it_conditions(firstcond: int, mask: int) -> list[int]:
    """The condition of each instruction of an IT block (the architecture's
    ITAdvance on ITSTATE)."""
    state = firstcond << 4 | mask
    result = []
    while state & 0xF:
        result.append(state >> 4)
        state = 0 if state & 7 == 0 else state & 0xE0 | state << 1 & 0x1F
    return result


def it_blocks(context, suite, results):
    form = form_named(context, "IT_T1")
    for firstcond in range(15):
        for mask in range(1, 16):
            word = 0xBF00 | firstcond << 4 | mask
            found = usable(context, form, word << 16, True)
            if found is None:
                continue
            conditions = it_conditions(firstcond, mask)
            for mixed in (False, True):
                for passes in (True, False):
                    if firstcond == 14 and not passes:
                        continue
                    flags = cond_flags(firstcond, passes, 17 * firstcond + mask)
                    prog = Prog().half(word)
                    expect = {}
                    for index, condition in enumerate(conditions):
                        if mixed and index % 2:
                            prog.half(0xF100 | index, index << 8 | 1)  # add.w r_i, r_i, #1
                        else:
                            prog.half(0x3001 | index << 8)  # adds r_i, #1: no flags in an IT block
                        expect[f"r{index}"] = 1 if condition_passes(condition, flags) else 0
                    prog.half(0x3401)  # adds r4, #1: after the block, unconditional
                    expect["r4"] = 1
                    prog.tail(THUMB)
                    make(results, suite, f"IT_T1/{firstcond:x}{mask:x}{'m' if mixed else 't'}"
                         f"{'p' if passes else 'f'}", "IT_T1", THUMB, prog, {"apsr": flags, "r4": 7},
                         check=False, retired=1 + len(conditions) + 2, expect=expect)
    # IT ... B (T2) as the last instruction: taken only if its condition passes.
    form_b = form_named(context, "B_T2")
    affine = Affine(context, form_b, base_word(context, form_b)[0], "imm32")
    for firstcond in (0, 1, 10, 11):
        for passes in (True, False):
            flags = cond_flags(firstcond, passes, 91 + firstcond)
            word = affine.solve(8 - 6)  # the tail at 8, the PC at 2 + 4
            prog = Prog().half(0xBF08 | firstcond << 4).insn(word, "T16").fill(THUMB).tail(THUMB)
            executed = 0 if passes else 1
            make(results, suite, f"IT_T1/b{firstcond:x}{'p' if passes else 'f'}", "IT_T1", THUMB, prog,
                 {"apsr": flags}, executed, retired=3 + executed)


# Instructions that read the PC -------------------------------------------------------------------------------

PC_READS_A = (
    0xE1A0000F, 0xE28F0000, 0xE24F0004, 0xE08F1002, 0xE04F1002, 0xE59F0000, 0xE51F0008,
    0xE59F0010, 0xE55F0004, 0xE18F1002, 0xE15F0001, 0xE1CF00D0, 0xE1DF00B0,
)
PC_READS_T = (  # T16 words hold the halfword in their upper half
    0x46780000, 0x44780000, 0xA0010000, 0x48000000, 0x48010000, 0xF20F0004, 0xF2AF0004,
    0xF8DF0004, 0xF8DF1FFC,
)


def pc_reads(context, suite, results):
    for words, state, isa in ((PC_READS_A, ARM, "A32"), (PC_READS_T, THUMB, None)):
        for word in words:
            isa_word = isa or ("T32" if word & 0xFFFF or word >> 27 >= 0x1D else "T16")
            if isa_word != "A32" and not (word >> 27 >= 0x1D) and word & 0xFFFF == 0:
                isa_word = "T16"
            result, _operands = context.reference.operands(isa_word, word, 0, FEATURES)
            if result.status == "undefined":
                raise ValueError(f"{word:08x} is not a legal {isa_word} word")
            name = result.form
            for lead in (0, 1, 2, 3):
                prog = Prog()
                for _ in range(lead):
                    prog.word(0xE320F000) if state == ARM else prog.half(NOP_T)
                prog.insn(word, isa_word).tail(state)
                make(results, suite, f"{name}/{word:08x}.{lead}", name, state, prog, {},
                     retired=lead + 2, check=False, target=lead)


# PC as an unshifted register operand: the second operand of any operation, the
# first of a commuting one or of SUB and RSB, flag-setting and conditional, and
# [PC, Rm] addressing, with the forms beside them that keep the PC in a register
# (a shift, a subtracted index, an operation that does not commute). The loads'
# index puts the address in the data region, or past it for a fault.
PC_OPERANDS_A = (
    (0xE082100F, {}), (0xE052100F, {}), (0xE022100F, {}), (0xE1E0000F, {}),
    (0xE1B0000F, {}), (0xE151000F, {}), (0xE0D2100F, {}), (0xE09F1002, {}),
    (0xE0AF1002, {}), (0xE01F1002, {}), (0xE06F1002, {}), (0xE07F1002, {}),
    (0xE05F1002, {}), (0xE11F0001, {}), (0xE13F0001, {}), (0xE17F0001, {}),
    (0xE1CF1002, {}), (0xE08F1102, {}), (0x008F1002, {}), (0x1082100F, {}),
    (0xE79F0001, {"r1": 0x01000000}), (0xE7DF0001, {"r1": 0x01000001}),
    (0xE19F00B1, {"r1": 0x01000002}), (0xE78F0001, {"r1": 0x01000000}),
    (0xE79F0101, {"r1": 0x00400000}), (0xE71F0001, {"r1": 0xFF000000}),
)
PC_OPERANDS_T = (0x447B0000, 0x44FC0000)
# Each fold under EQ, whose condition fails and passes: data processing that
# sets no flags runs as a select, everything else behind a skip.
PC_CONDITIONAL_A = (
    (0x0082100F, {}), (0x0042100F, {}), (0x0022100F, {}), (0x01A0000F, {}), (0x01E0000F, {}),
    (0x008F1002, {}), (0x000F1002, {}), (0x00AF1002, {}), (0x004F1002, {}), (0x006F1002, {}),
    (0x011F0001, {}), (0x079F0001, {"r1": 0x01000000}), (0x078F0001, {"r1": 0x01000000}),
    (0x019F00B1, {"r1": 0x01000002}),
)
PC_CONDITIONAL_T = (0x46780000, 0x447B0000, 0x44FC0000)
# Each fold whose other operand is its destination, which an in-place ADD or SUB
# of an immediate would run (ADD, ADDS, SUB and RSB, which becomes a SUB).
PC_IN_PLACE_A = (0xE08F1001, 0xE081100F, 0xE091100F, 0xE041100F, 0xE06F1001)


def pc_operands(context, suite, results):
    for word, initial in PC_OPERANDS_A:
        result, _operands = context.reference.operands("A32", word, 0, FEATURES)
        if result.status == "undefined":
            raise ValueError(f"{word:08x} is not a legal A32 word")
        for lead in (0, 1, 2, 3):
            prog = Prog()
            for _ in range(lead):
                prog.word(0xE320F000)
            prog.insn(word, "A32").tail(ARM)
            make(results, suite, f"{result.form}/{word:08x}.{lead}", result.form, ARM, prog,
                 initial, retired=lead + 2, check=False, target=lead)
    # A load whose [PC, Rm] address is unmapped faults at the instruction.
    for lead in (0, 1):
        prog = Prog()
        for _ in range(lead):
            prog.word(0xE320F000)
        prog.insn(0xE79F0001, "A32").tail(ARM)
        make(results, suite, f"LDR_r_A1_off/e79f0001.fault{lead}", "LDR_r_A1_off", ARM, prog,
             {"r1": 0x11000000}, fault_retired=lead, target=lead)
    for word in PC_OPERANDS_T:
        result, _operands = context.reference.operands("T16", word, 0, FEATURES)
        if result.status == "undefined":
            raise ValueError(f"{word:08x} is not a legal T16 word")
        for lead in (0, 1, 2, 3):
            prog = Prog()
            for _ in range(lead):
                prog.half(NOP_T)
            prog.insn(word, "T16").tail(THUMB)
            make(results, suite, f"{result.form}/{word:08x}.{lead}", result.form, THUMB, prog, {},
                 retired=lead + 2, check=False, target=lead)
    for word, initial in PC_CONDITIONAL_A:
        result, _operands = context.reference.operands("A32", word, 0, FEATURES)
        if result.status == "undefined":
            raise ValueError(f"{word:08x} is not a legal A32 word")
        for flags in (0, 0x40000000):
            for lead in (0, 1):
                prog = Prog()
                for _ in range(lead):
                    prog.word(0xE320F000)
                prog.insn(word, "A32").tail(ARM)
                make(results, suite, f"{result.form}/{word:08x}.eq{flags >> 28:x}.{lead}", result.form, ARM,
                     prog, {**initial, "apsr": flags}, retired=lead + 2, check=False, target=lead)
    for word in PC_CONDITIONAL_T:
        result, _operands = context.reference.operands("T16", word, 0, FEATURES)
        if result.status == "undefined":
            raise ValueError(f"{word:08x} is not a legal T16 word")
        for flags in (0, 0x40000000):
            for lead in (0, 1):
                prog = Prog()
                for _ in range(lead):
                    prog.half(NOP_T)
                prog.half(0xBF08).insn(word, "T16").tail(THUMB)
                make(results, suite, f"{result.form}/{word:08x}.it-eq{flags >> 28:x}.{lead}", result.form,
                     THUMB, prog, {"apsr": flags}, retired=lead + 3, check=False, target=lead + 1)
    # ADD Rdn, PC under IT EQ, whose condition fails and passes.
    for flags in (0, 0x40000000):
        for lead in (0, 1):
            prog = Prog()
            for _ in range(lead):
                prog.half(NOP_T)
            prog.half(0xBF08).insn(0x44780000, "T16").tail(THUMB)
            make(results, suite, f"ADD_r_T2/it-eq-{flags >> 28:x}.{lead}", "ADD_r_T2", THUMB, prog,
                 {"apsr": flags}, retired=lead + 3, check=False, target=lead + 1)
    for word in PC_IN_PLACE_A:
        result, _operands = context.reference.operands("A32", word, 0, FEATURES)
        if result.status == "undefined":
            raise ValueError(f"{word:08x} is not a legal A32 word")
        for lead in (0, 1, 2, 3):
            prog = Prog()
            for _ in range(lead):
                prog.word(0xE320F000)
            prog.insn(word, "A32").tail(ARM)
            make(results, suite, f"{result.form}/{word:08x}.{lead}", result.form, ARM, prog, {},
                 retired=lead + 2, check=False, target=lead)


# Runs of conditional instructions that a skip steps over, of two to eleven
# stream words: MUL (two words) and LDM of one register in A32 or two in T32
# (three), then an unconditional ADD the stream resumes at. Each run is under
# its own condition, A32 and Thumb seven apart, so the runs cover every
# condition.
SKIP_RUNS = ("M", "L", "MM", "ML", "LL", "MML", "MLL", "LLL", "MMML", "MLLL")
CONDITION_NAMES = ("eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le")


def it_all_then(firstcond: int, count: int) -> int:
    """IT with `count` instructions, all under `firstcond`."""
    then = (firstcond & 1) * ((1 << (count - 1)) - 1) << (5 - count)
    return 0xBF00 | firstcond << 4 | then | 1 << (4 - count)


def skip_runs(context, suite, results):
    for passes in (False, True):
        for index, run in enumerate(SKIP_RUNS):
            outcome = "p" if passes else "f"
            condition = index % 14
            flags = cond_flags(condition, passes, index)
            prog = Prog()
            for kind in run:  # mul<c> r1, r1, r2 / ldm<c> r8, {r3}
                prog.word(condition << 28 | (0x00010291 if kind == "M" else 0x08980008))
            prog.word(0xE2844001).tail(ARM)  # add r4, r4, #1
            make(results, suite, f"skip/a-{run}.{CONDITION_NAMES[condition]}{outcome}",
                 "MUL_A1" if run[0] == "M" else "LDM_A1", ARM, prog,
                 {"apsr": flags, "r4": 7, "r8": D + 0x100}, check=False, retired=len(run) + 2,
                 expect={"r4": 1})
            condition = (index + 7) % 14
            flags = cond_flags(condition, passes, index + 7)
            prog = Prog().half(it_all_then(condition, len(run)))
            for kind in run:  # mul r1, r1, r2 / ldm.w r8, {r3, r5}
                prog.half(*((0xFB01, 0xF102) if kind == "M" else (0xE898, 0x0028)))
            prog.half(0x3401).tail(THUMB)  # adds r4, #1
            make(results, suite, f"skip/t-{run}.{CONDITION_NAMES[condition]}{outcome}", "IT_T1", THUMB,
                 prog, {"apsr": flags, "r4": 7, "r8": D + 0x100}, check=False, retired=len(run) + 3,
                 expect={"r4": 1})


def suite_conditions(context: Context) -> list[Case]:
    results: list[Case] = []
    condition_table(context, "conditions", results)
    it_blocks(context, "conditions", results)
    skip_runs(context, "conditions", results)
    return results


def suite_pc(context: Context) -> list[Case]:
    results: list[Case] = []
    pc_reads(context, "pc", results)
    pc_operands(context, "pc", results)
    return results


def suite_branch(context: Context) -> list[Case]:
    results: list[Case] = []
    suite = "branch"
    a32_branches(context, suite, results)
    thumb_branches(context, suite, results)
    register_branches(context, suite, results)
    compare_branches(context, suite, results)
    table_branches(context, suite, results)
    pc_loads(context, suite, results)
    alu_pc_writes(context, suite, results)
    unmapped_targets(context, suite, results)
    conditional_table_branches(context, suite, results)
    conditional_pc_loads(context, suite, results)
    return results


SUITES = {"branch": suite_branch, "conditions": suite_conditions, "pc": suite_pc}
