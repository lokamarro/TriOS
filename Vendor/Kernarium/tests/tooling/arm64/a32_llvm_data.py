#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""LLVM MC evidence for the AArch32 encoding table.

The XML is the authority for the table; LLVM MC is an independent decoder the
generator consults for two things. Every form gets a witness word LLVM
decodes to an instruction of the form's family, and every index row gets
deterministic sample words (edge fills and random fills, some with the
register fields kept clear of SP and PC) that the unit tests decode and
compare: the decoder's form against the index partition's, and its accept or
reject against LLVM's. The exhaustive 16-bit Thumb space is recorded the same
way. A disagreement the generator cannot explain fails the generation.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import random
import re
from dataclasses import dataclass
from pathlib import Path

import a32_llvm_oracle as llvm
import a32_table_model as tm
import a32_table_render as render


SAMPLE_ROWS_RANDOM = 2
THUMB_UNIVERSE = {"A32": 0xFFFFFFFF, "T16": 0xFFFF0000, "T32": 0xFFFFFFFF}
REGISTER_FIELDS = re.compile(r"R(d|n|m|t|t2|a|s|dn|dHi|dLo|x|)\Z")
SAFE_REGISTERS = tuple(range(0, 13)) + (14,)
REGISTER_LIST_FIELDS = {"register_list", "registers"}


@dataclass
class Sample:
    isa: str
    word: int
    kind: str  # row kind of the index partition, or "none"
    form: str
    clean: bool
    violation: bool  # a should-be bit is flipped
    llvm: str  # MC opcode, or "-"
    soft: bool
    note: str = ""
    expected: object = None  # a32_expected.Expected


def _fill(rng: random.Random, row: tm.RowModel, form: tm.FormModel, mode: str,
          clean: bool) -> int | None:
    universe = THUMB_UNIVERSE[row.isa]
    free = universe & ~row.mask
    encoding = form.encoding
    for _attempt in range(200):
        if mode == "zeros":
            word = row.value
        elif mode == "ones":
            word = row.value | free
        else:
            word = row.value | (rng.getrandbits(32) & free)
        if encoding is not None:
            word = word & ~encoding.should_mask | encoding.should_value
            if clean:
                word = _clean(rng, word, row, encoding)
                word = word & ~encoding.should_mask | encoding.should_value
        word &= universe
        if mode != "random" and _attempt == 0 and not row.matches(word):
            mode = "random"
        if row.matches(word) and (
            encoding is None
            or word & encoding.should_mask == encoding.should_value
        ):
            return word
    return None


def _clean(rng: random.Random, word: int, row: tm.RowModel, encoding) -> int:
    """Keep the register fields off SP and PC where the row leaves them free."""
    for field in encoding.fields:
        field_mask = ((1 << field.width) - 1) << field.low
        if field_mask & row.mask == field_mask:
            continue
        if REGISTER_FIELDS.match(field.name) and field.width == 4:
            value = rng.choice(SAFE_REGISTERS)
            word = word & ~field_mask | value << field.low
        elif field.name in REGISTER_LIST_FIELDS and field.width == 16:
            word &= ~(0xA000 << field.low)
        elif field.name == "cond" and rng.getrandbits(1):
            word = word & ~field_mask | 14 << field.low
        elif field.name in ("Vd", "Vn", "Vm") and rng.getrandbits(1):
            # Quadword and crypto forms need even register numbers.
            word &= ~(1 << field.low)
    return word


# LLVM names some instructions by a different mnemonic than the XML page: the
# opcode families, by XML page ID, that also count as the page's family.
FAMILY_EXTRAS = {
    "ADD_SP_i": ("ADD",), "ADD_i": ("ADD",), "SUB_SP_i": ("SUB",),
    "SUB_i": ("SUB",), "ADR_a32": ("ADD", "SUB"),
    "CLRBHB_a32": ("HINT",), "CSDB_a32": ("HINT",), "ESB_a32": ("HINT",),
    "NOP_a32": ("HINT",), "SEV_a32": ("HINT",), "SEVL_a32": ("HINT",),
    "TSB_a32": ("HINT",), "WFE_a32": ("HINT",), "WFI_a32": ("HINT",),
    "YIELD_a32": ("HINT",), "DBG": ("HINT",),
    "CPS": ("CPS",), "ERET_a32": ("SUBS_PC_LR",),
    "FLDMX": ("FLDMX",), "FSTMX": ("FSTMX",),
    "LDM_e": ("SYSLDM",), "LDM_u": ("SYSLDM",), "STM_u": ("SYSSTM",),
    "MOV_r": ("ASR", "LSL", "LSR", "ROR", "RRX"),
    "MOV_rr": ("ASR", "LSL", "LSR", "ROR", "RRX"),
    "PSSBB": ("DSB",), "SSBB": ("DSB",), "SRS": ("SRS",),
    "VCVT_bfs": ("BF16_VCVT",), "VCVTB_bfs": ("BF16_VCVT",),
    "VCVTT_bfs": ("BF16_VCVT",),
    "VCVTR_iv": ("VTO",), "VCVT_iv": ("VTO",),
    "VCVT_vi": ("VSITO", "VUITO"),
    "VCVT_xv": ("VSHTO", "VSLTO", "VUHTO", "VULTO", "VTOSH", "VTOSL", "VTOUH", "VTOUL"),
    "VDOT": ("BF16VDOT",), "VDOT_s": ("BF16VDOT",),
    "VFMA_bf": ("VBF16MAL",), "VFMA_bfs": ("VBF16MAL",),
    "VLDM": ("VLDM",), "VSTM": ("VSTM",),
    "VMAXNM": ("NEON_VMAXNM", "VFP_VMAXNM"),
    "VMINNM": ("NEON_VMINNM", "VFP_VMINNM"),
    "VMOVX": ("VMOVH",), "VMOV_i": ("FCONST",),
    "VMOV_rs": ("VSETLN",), "VMOV_sr": ("VGETLN",),
    "VQMOVN": ("VQMOVN",), "VQSHL_i": ("VQSHL",),
}


def opcode_family(opcode: str) -> str:
    if opcode.startswith("t2"):
        opcode = opcode[2:]
    elif len(opcode) > 1 and opcode[0] == "t" and opcode[1].isupper():
        opcode = opcode[1:]
    return opcode.upper()


def family_matches(page: str, mnemonic: str, opcode: str) -> bool:
    family = opcode_family(opcode)
    candidates = {mnemonic.upper(), *FAMILY_EXTRAS.get(page, ())}
    for suffix in ("S", "W"):
        if mnemonic.endswith(suffix) and len(mnemonic) > 2:
            candidates.add(mnemonic[:-1].upper())
    return any(family.startswith(candidate) for candidate in candidates)


def check_families(model: tm.Model, samples: list[Sample]) -> None:
    """Every sample LLVM decodes must decode to its form's instruction family."""
    bad: dict[str, set[str]] = {}
    for sample in samples:
        if sample.kind != "instruction" or sample.llvm in ("-", "?"):
            continue
        form = model.by_name[sample.form]
        encoding = form.encoding
        if not family_matches(form.page, encoding.mnemonic, sample.llvm):
            bad.setdefault(f"{form.page}/{encoding.mnemonic}", set()).add(sample.llvm)
    if bad:
        detail = "; ".join(f"{k}: {sorted(v)}" for k, v in sorted(bad.items())[:20])
        raise ValueError(
            f"LLVM decodes {len(bad)} pages to unexpected instruction families: {detail}"
        )


# The coprocessor accesses an EL0 program may make (the profile's whole set),
# which random fills almost never reach. A32 and T32 share the words.
POLICY_WORDS = tuple(
    (isa, word, sysreg)
    for word, sysreg in (
        (0xEE1D0F70, "tpidruro"),
        (0xEE1D0F50, "tpidrurw_read"),
        (0xEE1E0F10, "cntfrq"),
        (0xEE0D0F50, "tpidrurw_write"),
        (0xEE070FBA, "cp15_dmb"),
        (0xEE070F9A, "cp15_dsb"),
        (0xEE070F95, "cp15_isb"),
        (0xEC510F1E, "cntvct"),
    )
    for isa in ("A32", "T32")
)

OK_PER_ROW = 3
REJECTED_PER_ROW = 1
FIRST_BATCH = 10
EXTRA_BATCHES = 60
BATCH = 8


def _batch(rng, row, form, plan):
    result = []
    for mode, clean in plan:
        word = _fill(rng, row, form, mode, clean)
        if word is not None:
            result.append((word, clean))
    return result


def build_samples(model: tm.Model, reference, features: set[str], seed: str,
                  tool: Path, runner) -> list[Sample]:
    """Sample words per row, chosen by what the decoder reference accepts.

    Every instruction row gets up to OK_PER_ROW words the reference accepts
    and REJECTED_PER_ROW it rejects, drawn from edge fills and random fills
    (more of them for a row whose accepted words are rare); every other row
    gets a few words. LLVM's decode of all of them is recorded."""
    samples: list[Sample] = []
    seen: set[tuple[str, int]] = set()
    for isa in ("A32", "T16", "T32"):
        for position, row in enumerate(model.rows[isa]):
            form = model.forms[row.form]
            rng = random.Random(f"{seed}:{isa}:{position}:{row.partition}")
            plan = [("zeros", True), ("ones", True)]
            plan += [("random", True)] * 6 + [("random", False)] * 2
            ok: list[Sample] = []
            rejected: list[Sample] = []
            for attempt in range(1 + EXTRA_BATCHES):
                if attempt:
                    plan = [("random", True)] * BATCH
                if attempt and (row.kind != "instruction" or len(ok) >= OK_PER_ROW):
                    break
                for word, clean in _batch(rng, row, form, plan):
                    if (isa, word) in seen:
                        continue
                    seen.add((isa, word))
                    expected = reference.decode(isa, word, 0, features)
                    sample = Sample(isa, word, row.kind, form.name, clean, False, "-", False)
                    sample.expected = expected
                    (ok if expected.status != "undefined" else rejected).append(sample)
            limit = OK_PER_ROW if row.kind == "instruction" else 2
            chosen = ok[:limit] + _distinct_reasons(rejected, REJECTED_PER_ROW)
            samples.extend(chosen)
    for isa, word, sysreg in POLICY_WORDS:
        expected = reference.decode(isa, word, 0, features)
        if expected.status != "ok" or expected.sysreg != sysreg:
            raise ValueError(
                f"policy word {isa} {word:#010x} decodes to {expected.form} "
                f"{expected.status}/{expected.reason}/{expected.sysreg}, not {sysreg}"
            )
        if (isa, word) in seen:
            continue
        seen.add((isa, word))
        sample = Sample(isa, word, "instruction", expected.form, True, False, "-", False)
        sample.expected = expected
        samples.append(sample)
    for sample in violation_samples(model, seed):
        sample.expected = reference.decode(sample.isa, sample.word, 0, features)
        samples.append(sample)
    decode_samples(tool, runner, [s for s in samples if not s.violation])
    return samples


WITNESS_TRIES = 160


def find_witnesses(model: tm.Model, names: set[str], seed: str, tool: Path, runner) -> dict[str, int]:
    """A word LLVM MC decodes in each named form, among clean fills with the
    condition field AL (LLVM decodes some forms only unconditionally)."""
    candidates: list[Sample] = []
    for isa in ("A32", "T16", "T32"):
        for position, row in enumerate(model.rows[isa]):
            form = model.forms[row.form]
            if row.kind != "instruction" or form.name not in names:
                continue
            rng = random.Random(f"{seed}:witness:{isa}:{position}")
            for _attempt in range(WITNESS_TRIES):
                word = _fill(rng, row, form, "random", True)
                if word is None:
                    break
                for field in form.encoding.fields:
                    mask = ((1 << field.width) - 1) << field.low
                    if field.name == "cond" and field.width == 4 and not mask & row.mask:
                        word = word & ~mask | 14 << field.low
                if row.matches(word):
                    candidates.append(Sample(isa, word, row.kind, form.name, True, False, "-", False))
    decode_samples(tool, runner, candidates)
    result: dict[str, int] = {}
    for sample in candidates:
        form = model.by_name[sample.form]
        if (
            sample.llvm != "-"
            and not sample.soft
            and sample.form not in result
            and family_matches(form.page, form.encoding.mnemonic, sample.llvm)
        ):
            result[sample.form] = sample.word
    return result


def _distinct_reasons(items: list[Sample], count: int) -> list[Sample]:
    """Rejected samples, preferring each reason once."""
    chosen: list[Sample] = []
    reasons: set[str] = set()
    for sample in items:
        if sample.expected.reason not in reasons:
            reasons.add(sample.expected.reason)
            chosen.append(sample)
    for sample in items:
        if len(chosen) >= count:
            break
        if sample not in chosen:
            chosen.append(sample)
    return chosen[:count]


def violation_samples(model: tm.Model, seed: str) -> list[Sample]:
    """Words with one should-be bit flipped; the decoder rejects them."""
    result: list[Sample] = []
    for isa in ("A32", "T16", "T32"):
        for position, row in enumerate(model.rows[isa]):
            form = model.forms[row.form]
            encoding = form.encoding
            if encoding is None or not encoding.should_mask or row.kind != "instruction":
                continue
            rng = random.Random(f"{seed}:violation:{isa}:{position}:{row.partition}")
            word = _fill(rng, row, form, "random", True)
            if word is None:
                continue
            bits = [b for b in range(32) if encoding.should_mask >> b & 1]
            flipped = word ^ (1 << rng.choice(bits))
            if row.matches(flipped):
                result.append(Sample(isa, flipped, row.kind, form.name, True, True, "?", False))
    return result


def decode_samples(tool: Path, runner, samples: list[Sample]) -> None:
    """Fill in LLVM's result for every sample."""
    for isa in ("A32", "T16", "T32"):
        chosen = [s for s in samples if s.isa == isa]
        if not chosen:
            continue
        if isa == "A32":
            items = [(s.word, 4) for s in chosen]
            results = llvm.decode_words(tool, "A32", items, runner)
        elif isa == "T16":
            items = [(s.word, 2) for s in chosen]
            results = llvm.decode_words(tool, "T32", items, runner)
        else:
            items = [(s.word, 4) for s in chosen]
            results = llvm.decode_words(tool, "T32", items, runner)
        for sample, result in zip(chosen, results):
            if result is not None:
                sample.llvm, sample.soft = result


def t16_reference(model: tm.Model, reference, features: set[str], tool: Path, runner):
    """Run-length (first, last, form, llvm opcode, soft, status, reason) over
    every T16 word."""
    halfwords = list(range(0, 0xE800))
    results = llvm.decode_words(tool, "T32", [(hw << 16, 2) for hw in halfwords], runner)
    runs = []
    for hw, result in zip(halfwords, results):
        outcome = reference.decode("T16", hw << 16, 0, features)
        opcode, soft = ("-", False) if result is None else result
        key = (outcome.form, opcode, soft, outcome.status, outcome.reason)
        if runs and runs[-1][2:] == key and runs[-1][1] == hw - 1:
            runs[-1] = (runs[-1][0], hw, *key)
        else:
            runs.append((hw, hw, *key))
    check_t16(model, runs)
    return runs


# Disagreements between the reference decoder and LLVM that are understood.
# An instruction the reference accepts and LLVM rejects must be listed here.
LLVM_REJECTS_ACCEPTED = {
    "BKPT": "LLVM decodes BKPT only with condition AL; the XML executes it under any",
    "ReservedHint": "the reserved hint space executes as a NOP; LLVM rejects it",
}
# A word in an allocated encoding that the reference calls unallocated (a
# Decode UNDEFINED) while LLVM decodes it.
LLVM_ACCEPTS_UNALLOCATED = {
    "VCVT_xs": "LLVM decodes half-precision fixed-point conversions with imm6 10xxxx",
}


def _llvm_disagreement(page: str, status: str, reason: str, llvm_ok: bool):
    """The explanation key of a disagreement, or None if they agree."""
    if status != "undefined" and not llvm_ok:
        return ("rejects", page)
    if status == "undefined" and llvm_ok and reason == "unallocated":
        return ("accepts-unallocated", page)
    return None


def check_agreement(model: tm.Model, samples: list[Sample]) -> None:
    """The reference accepts only what LLVM decodes, and rejects only what
    the profile, a feature, UNPREDICTABLE or a listed leniency explains."""
    problems = []
    for sample in samples:
        if sample.violation:
            continue
        outcome = sample.expected
        page = model.by_name[sample.form].page or sample.form
        key = _llvm_disagreement(page, outcome.status, outcome.reason, sample.llvm != "-")
        if key is None:
            continue
        table = LLVM_REJECTS_ACCEPTED if key[0] == "rejects" else LLVM_ACCEPTS_UNALLOCATED
        if page not in table:
            problems.append(f"{key[0]} {sample.isa} {sample.word:#010x} {sample.form} {sample.llvm}")
    if problems:
        raise ValueError(
            f"{len(problems)} unexplained reference/LLVM disagreements, "
            f"for example {problems[:12]}"
        )


def check_t16(model: tm.Model, runs) -> None:
    problems = []
    for first, _last, form, opcode, _soft, status, reason in runs:
        page = (model.by_name[form].page if form in model.by_name else "") or form
        key = _llvm_disagreement(page, status, reason, opcode != "-")
        if key is None:
            continue
        table = LLVM_REJECTS_ACCEPTED if key[0] == "rejects" else LLVM_ACCEPTS_UNALLOCATED
        if page not in table:
            problems.append(f"{key[0]} {first:#06x} {form} {opcode}")
    if problems:
        raise ValueError(
            f"{len(problems)} unexplained T16 reference/LLVM disagreements, "
            f"for example {problems[:6]}"
        )
