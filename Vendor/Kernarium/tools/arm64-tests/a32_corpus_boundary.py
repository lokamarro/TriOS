#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The suites that put the program or its accesses on a boundary.

- fetch: instruction fetch near a line. Every Thumb-2 form of the other
  suites runs once with its two halfwords on either side of the 4 KiB line
  at 0x4f001000 (the program's origin is 0xffe), where both pages are mapped;
  and the fetch faults at the end of the 16 KiB code region: a 32-bit
  instruction whose second halfword is unmapped (SIGSEGV at the unmapped
  halfword, pc at the instruction, nothing retired), and a program that falls
  off the end of the region (SIGSEGV at 0x4f004000, the instructions before
  it retired);
- straddle: stores that cross from the data region into the read-only region
  after some of their words were stored. STM, PUSH and VSTM store word by
  word (or register by register) in order, so the words ahead of the faulting
  one are in memory and the base register is as before.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import dataclasses  # noqa: E402

import a32_corpus_armv8 as armv8  # noqa: E402
import a32_corpus_memory as memory  # noqa: E402
import a32_corpus_plan as plan  # noqa: E402
import a32_corpus_simd as simd  # noqa: E402
from a32_corpus_plan import FLAGS, ISA_LETTER, Case, Context, pattern, word_of  # noqa: E402

CODE_END = 0x4000
LINE_4K = 0x1000
ORIGIN_STRADDLE = LINE_4K - 2


def thumb32(case: Case) -> bool:
    return case.isa == "T" and len(case.program) == 2 and case.program[0] >> 11 in (0x1D, 0x1E, 0x1F)


def suite_fetch(context: Context) -> list[Case]:
    sources: list[Case] = []
    for builder in (plan.suite_alu, memory.suite_memory, memory.suite_prefetch, simd.suite_simd,
                    simd.suite_simd_memory, armv8.suite_armv8, armv8.suite_armv8_memory):
        sources += builder(context)
    cases: list[Case] = []
    seen = set()
    for case in sources:
        if thumb32(case) and case.form not in seen and case.straight and case.target == 0:
            seen.add(case.form)
            cases.append(dataclasses.replace(case, suite="fetch", name=f"{case.form}/straddle",
                                             origin=ORIGIN_STRADDLE))
    # A 32-bit instruction whose second halfword is past the end of the region,
    # for each top byte of the first halfword (each major group of encodings).
    groups: dict[int, Case] = {}
    for case in sources:
        if thumb32(case) and case.straight:
            groups.setdefault(case.program[0] >> 8, case)
    for top, case in sorted(groups.items()):
        cases.append(Case("fetch", f"{case.form}/unmapped-second.{top:02x}", case.form, "T",
                          (case.program[0],), 0, case.recipe, dict(case.initial), straight=False,
                          fault_retired=0, origin=CODE_END - 2))
    # A program that runs to the end of the region and falls off it.
    for suffix, form, isa, program, origin, retired in (
            ("a32", "ADD_i_A1", "A", (0xE2821001,), CODE_END - 4, 1),
            ("a32x2", "ADD_i_A1", "A", (0xE2821001, 0xE2832002), CODE_END - 8, 2),
            ("t16", "MOV_i_T1", "T", (0x2105,), CODE_END - 2, 1),
            ("t16x2", "MOV_i_T1", "T", (0x2105, 0x2206), CODE_END - 4, 2),
            ("t32", "ADD_i_T3", "T", (0xF102, 0x0101), CODE_END - 4, 1)):
        context.model.by_name[form]
        cases.append(Case("fetch", f"{form}/falls-off.{suffix}", form, isa, program, 0,
                          f"edges-{'a' if isa == 'A' else 't'}", {}, straight=False, fault_retired=retired,
                          origin=origin))
    return cases


def suite_straddle(context: Context) -> list[Case]:
    cases: list[Case] = []
    read_only = memory.READ_ONLY
    for form in memory.memory_forms(context, "base"):
        if "stores" not in form.traits or memory.MULTIPLE.fullmatch(memory.mnemonic(form.name)) is None:
            continue
        isa = form.encoding.isa
        for index, (word, operands) in enumerate(memory.pick_words(context, form, 4)):
            count = bin(operands["registers"]).count("1")
            if count < 2:
                continue
            start = read_only - 4 * (count // 2)
            aim = memory.solve_multiple(operands, memory.multiple_mode(form.name), start)
            number = len(cases)
            cases.append(Case("straddle", f"{form.name}/{index}.sro", form.name, ISA_LETTER[isa],
                              word_of(isa, word), 0, f"{memory.RECIPES[number % 6]}-{'a' if isa == 'A32' else 't'}",
                              {"apsr": FLAGS[pattern(number) >> 8 & 3], **aim}))
    for form_index, form in enumerate(simd.simd_forms(context)):
        if "stores" not in form.traits or form.name[0] not in "VF":
            continue
        isa = form.encoding.isa
        for index, (word, operands) in enumerate(simd.pick_words(context, form, 4)):
            if "regs" not in operands or "imm32" not in operands or operands["imm32"] < 16:
                continue
            start = read_only - (operands["imm32"] // 2 & ~7)
            apsr = FLAGS[pattern(len(cases)) >> 8 & 3]
            aim = simd.aim_memory(operands, start, apsr, len(cases))
            if aim is None or operands.get("n") == 15:
                continue
            cases.append(Case("straddle", f"{form.name}/{index}.sro", form.name, ISA_LETTER[isa],
                              word_of(isa, word), 0,
                              f"{simd.RECIPES[(index + form_index) % len(simd.RECIPES)]}-"
                              f"{'a' if isa == 'A32' else 't'}", {"apsr": apsr, **aim}))
    return cases


SUITES = {"fetch": suite_fetch, "straddle": suite_straddle}
