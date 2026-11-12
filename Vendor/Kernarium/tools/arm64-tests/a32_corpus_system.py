#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The system and profile suites of the AArch32 instruction corpus.

- system: barriers, hints, CPS (which has no effect at EL0), the APSR
  instructions, the coprocessor-15 registers a Linux process may use
  (TPIDRURO, TPIDRURW and the CP15 barriers), BKPT and the encodings Linux
  turns into SIGTRAP;
- absent: what the profile does not implement or decides against, with the
  event the profile defines for it. No oracle runs these: the record is
  labelled "profile", and an instruction that is UNDEFINED, UNPREDICTABLE,
  unallocated or of an absent feature raises SIGILL, and SVC raises the
  system-call event. Decisions D4 (floating-point trap enables are
  read-as-zero) and D6 (SWP, SWPB and SETEND raise the undefined-instruction
  event whatever the condition; the guest kernel emulates SWP and SWPB behind
  abi.swp) are here.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import pathlib
import random

from a32_corpus_plan import (  # noqa: E402
    FEATURES, FLAGS, ISA_LETTER, Case, Context, SAMPLES, candidate_words, corpus, fields_of, pattern,
    set_field, usable, word_of,
)

NOP_A = 0xE320F000
NOP_T = 0xBF00
EVENT_SIGILL, EVENT_SIGTRAP, EVENT_SVC = 4, 5, 255


# Generic samples of a set of forms --------------------------------------------------------------------


def generic(context: Context, suite: str, forms, words_per_form: int = 4, states: int = 2):
    cases = []
    for form in forms:
        isa = form.encoding.isa
        chosen = []
        for word in candidate_words(context, form, words_per_form + 4):
            found = usable(context, form, word)
            if found is not None and found[0] not in [w for w, _ in chosen]:
                chosen.append(found)
            if len(chosen) == words_per_form:
                break
        for index, (word, _operands) in enumerate(chosen):
            for variant in range(states):
                number = len(cases)
                recipe = f"{('edges', 'pattern', 'edges5', 'pattern1')[(index + variant) % 4]}-" \
                    f"{'a' if isa == 'A32' else 't'}"
                flags = FLAGS[(pattern(number) >> 8) % len(FLAGS)]
                cases.append(Case(suite, f"{form.name}/{index}.{variant}", form.name, ISA_LETTER[isa],
                                  word_of(isa, word), 0, recipe, {"apsr": flags}))
    return cases


def forms_where(context: Context, predicate):
    return [form for form in context.model.forms
            if form.encoding is not None and form.level == "base" and predicate(form)]


# Coprocessor 15 -------------------------------------------------------------------------------------------------

# (name, A32 word with Rt = 0, the decoder's name for the register)
CP15 = (
    ("MRC_A1", 0xEE1D0F70, "tpidruro"), ("MRC_A1", 0xEE1D0F50, "tpidrurw_read"),
    ("MCR_A1", 0xEE0D0F50, "tpidrurw_write"), ("MCR_A1", 0xEE070FBA, "cp15_dmb"),
    ("MCR_A1", 0xEE070F9A, "cp15_dsb"), ("MCR_A1", 0xEE070F95, "cp15_isb"),
)


def cp15_cases(context: Context, suite: str) -> list[Case]:
    cases = []
    for state, suffix, letter in (("A", "A1", "a"), ("T", "T1", "t")):
        for name, word, expected_register in CP15:
            name = name.split("_")[0] + "_" + suffix
            for register in (0, 3, 12, 14):
                encoded = word | register << 12
                if state == "T":
                    program = (encoded >> 16, encoded & 0xFFFF)
                else:
                    program = (encoded,)
                result, _ = context.reference.operands("A32" if state == "A" else "T32", encoded, 0,
                                                       FEATURES)
                if result.status == "undefined" or result.sysreg != expected_register:
                    raise ValueError(f"{name} {encoded:08x}: the decoder reads {result.sysreg}")
                initial = {f"r{register}": 0x12345678, "tpidrurw": 0x70001000 + register}
                cases.append(Case(suite, f"{name}/{expected_register}.r{register}", name, state, program,
                                  0, f"edges-{letter}", initial))
        # Read back what a write stored, and write TPIDRURO (read-only at EL0: undefined).
        write = 0xEE0D3F50
        read = 0xEE1D4F50
        if state == "A":
            program = (write, read)
        else:
            program = (write >> 16, write & 0xFFFF, read >> 16, read & 0xFFFF)
        cases.append(Case(suite, f"MCR_{suffix}/tpidrurw.roundtrip", f"MCR_{suffix}", state, program, 0,
                          f"pattern-{letter}", {"r3": 0xCAFE0000}))
        denied = 0xEE0D3F70
        program = (denied,) if state == "A" else (denied >> 16, denied & 0xFFFF)
        cases.append(Case(suite, f"MCR_{suffix}/tpidruro.write", f"MCR_{suffix}", state, program, 0,
                          f"pattern-{letter}", {"r3": 0xCAFE0000}))
    return cases


# BKPT and the encodings Linux takes for breakpoints ------------------------------------------------------------------


def breakpoints(context: Context, suite: str) -> list[Case]:
    cases = []
    for imm in (0, 0x1234, 0xFFFF):
        word = 0xE1200070 | (imm >> 4) << 8 | imm & 15
        cases.append(Case(suite, f"BKPT_A1/{imm:x}", "BKPT_A1", "A", (word,), 0, "pattern-a", {}))
    for imm in (0, 0xAB, 0xFF):
        cases.append(Case(suite, f"BKPT_T1/{imm:x}", "BKPT_T1", "T", (0xBE00 | imm,), 0, "pattern-t", {}))
    # Linux turns these into SIGTRAP (arm: ptrace breakpoints; arm64: the compat hooks).
    cases.append(Case(suite, "UDF_A1/linux-breakpoint", "UDF_A1", "A", (0xE7F001F0,), 0, "pattern-a", {}))
    cases.append(Case(suite, "UDF_T1/linux-breakpoint", "UDF_T1", "T", (0xDE01,), 0, "pattern-t", {}))
    cases.append(Case(suite, "UDF_T2/linux-breakpoint", "UDF_T2", "T", (0xF7F0, 0xA000), 0, "pattern-t", {}))
    return cases


def suite_system(context: Context) -> list[Case]:
    suite = "system"
    forms = forms_where(context, lambda form: form.availability == "implemented" and not form.check
                        and (form.traits & {"barrier", "hint"}) and "exclusive" not in form.traits
                        or (form.availability == "implemented" and form.nop))
    forms = [form for form in forms if form.name not in ("CLREX_A1", "CLREX_T1")] + [
        form for form in forms if form.name in ("CLREX_A1", "CLREX_T1")]
    cases = generic(context, suite, forms, 6, 2)
    apsr = forms_where(context, lambda form: form.check == "apsr")
    cases += generic(context, suite, apsr, 8, 2)
    cases += cp15_cases(context, suite)
    cases += breakpoints(context, suite)
    # Reserved hints execute as NOPs.
    for index, (isa, word) in enumerate(context.samples.get("ReservedHint", [])[:60]):
        if isa == "A32" and word >> 28 not in (14, 15):
            word = set_field(word, 28, 4, 14)
        cases.append(Case(suite, f"ReservedHint/{index}", "ReservedHint", ISA_LETTER[isa], word_of(isa, word),
                          0, "pattern-a" if isa == "A32" else "pattern-t", {}))
    return cases


# What the profile leaves out ---------------------------------------------------------------------------------------


def undefined_samples():
    """Stored words the decoder reports undefined, by form: (isa, word, reason)."""
    result: dict[str, list[tuple[str, int, str]]] = {}
    with SAMPLES.open(encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            if row["status"] == "undefined" and row["clean"] == "1":
                result.setdefault(row["form"], []).append((row["isa"], int(row["word"], 16), row["reason"]))
    return result


def predicted(suite: str, name: str, form: str, isa: str, word: int, event: int, lead: int) -> Case:
    state = "A" if isa == "A32" else "T"
    prog = ([NOP_A] * lead if isa == "A32" else [NOP_T] * lead) + list(word_of(isa, word))
    return Case(suite, name, form, state, tuple(prog), lead, f"pattern-{state.lower()}", {},
                predicted=event)


def suite_absent(context: Context) -> list[Case]:
    suite = "absent"
    cases = []
    samples = undefined_samples()
    reference = context.reference

    def decodes_undefined(isa, word) -> bool:
        result, _ = reference.operands(isa, word, 0, FEATURES)
        return result.status == "undefined"

    def force_always(form, isa, word):
        if isa == "A32" and form is not None and form.conditional and form.cond_low == 28:
            forced = set_field(word, 28, 4, 14)
            if decodes_undefined(isa, forced):
                return forced
        return word

    rng = random.Random("a32-absent")
    for form in context.model.forms:
        if form.encoding is None:
            continue
        entries = [(isa, word, reason) for isa, word, reason in samples.get(form.name, [])
                   if reason in ("profile", "feature", "unpredictable")]
        # One stored word per reason, then more of the permanent ones.
        chosen = []
        for reason in ("profile", "feature", "unpredictable"):
            matching = [entry for entry in entries if entry[2] == reason]
            chosen += matching[:3 if reason != "unpredictable" else 1]
        for index, (isa, word, reason) in enumerate(chosen):
            word = force_always(form, isa, word)
            if not decodes_undefined(isa, word):
                continue
            cases.append(predicted(suite, f"{form.name}/{reason}.{index}", form.name, isa, word,
                                   EVENT_SIGILL, index % 2))
    # The encodings nothing allocates.
    for isa in ("A32", "T16", "T32"):
        unallocated = [entry for entry in samples.get("Undefined", []) if entry[0] == isa]
        rng.shuffle(unallocated)
        for index, (_isa, word, reason) in enumerate(unallocated[:160 if isa != "T16" else 100]):
            if decodes_undefined(isa, word):
                cases.append(predicted(suite, f"Undefined/{isa}.{reason}.{index}", "Undefined", isa, word,
                                       EVENT_SIGILL, index % 3))
    # SWP and SWPB are not in the Armv8 AArch32 profile: the engine reports the event whatever
    # the condition and the flags, and the guest kernel decides (abi.swp). Every condition field,
    # and the register shapes: Rt == Rt2 (the form CTS runs), Rn == Rt, the stack pointer as Rn
    # and the program counter as each operand.
    for byte in (0, 1):
        name = "SWPB" if byte else "SWP"
        for condition in range(16):
            word = 0x01031092 | byte << 22 | condition << 28
            if decodes_undefined("A32", word):
                cases.append(predicted(suite, f"Undefined/{name}.cond{condition:x}", "Undefined", "A32",
                                       word, EVENT_SIGILL, condition % 3))
        for shape, (rn, rt, rt2) in enumerate(((1, 0, 0), (0, 0, 1), (13, 0, 1), (2, 15, 1), (2, 1, 15),
                                               (15, 1, 2))):
            word = 0xE1000090 | byte << 22 | rn << 16 | rt << 12 | rt2
            if decodes_undefined("A32", word):
                cases.append(predicted(suite, f"Undefined/{name}.r{rn}.r{rt}.r{rt2}", "Undefined", "A32",
                                       word, EVENT_SIGILL, shape % 2))
    # System calls leave the program: the engine reports the event.
    for imm in (0, 0x123456, 0xFFFFFF):
        word = 0xEF000000 | imm
        cases.append(predicted(suite, f"SVC_A1/{imm:x}", "SVC_A1", "A32", word, EVENT_SVC, imm % 3))
    for imm in (0, 0xAB, 0xFF):
        cases.append(predicted(suite, f"SVC_T1/{imm:x}", "SVC_T1", "T16", (0xDF00 | imm) << 16, EVENT_SVC,
                               imm % 3))
    return cases


SUITES = {"system": suite_system, "absent": suite_absent}
PREDICTED = {"absent"}
