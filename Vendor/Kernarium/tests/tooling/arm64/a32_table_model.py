#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The AArch32 encoding table as data: forms, rows and operand programs.

The XML gives each instruction page's encodings and, in its encoding index,
the exact partition of the A32 and T32 encoding spaces into those encodings.
This module combines them with the profile's choices
(a32_profile_policy.py) into the model the generator renders as C++ and the
tests check against: one Form per XML encoding, one Row per allocated index
partition, and one operand program per distinct translated Decode pseudocode.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
from dataclasses import dataclass, field

import a32_architecture_oracle as oracle
import a32_asl as asl
import a32_operand_codegen as codegen
import a32_profile_policy as policy


class ModelError(ValueError):
    pass


PSEUDO_FORMS = ("Undefined", "ReservedHint")
CONSTRAINT_TEXT = re.compile(r"(!=|==) ([01x]+)\Z")
MAX_SEGMENTS = 3

# Static traits of a form, scanned from its Execute pseudocode.
TRAIT_NAMES = (
    "loads", "stores", "exclusive", "branch", "interwork", "alu_pc",
    "load_pc", "exception", "barrier", "hint", "prefetch", "fp_state",
)
# A form that reads or writes the S, D, Q or V registers or FPSCR (VFP,
# Advanced SIMD and the crypto instructions, which share the vector file), or
# checks that those units are enabled. An engine that loads the vectors lazily
# must load them for any block that holds one.
FP_STATE = re.compile(
    r"\b(?:S|D|Q|V|Sin|Din|Qin|Sout|Dout|Qout)\s*\[|\bFPSCR\b|\bFPEXC\b"
    r"|\bCheck(?:AdvSIMD|VFP|AdvSIMDOrVFP|Crypto)\w*Enabled\w*"
)
PC_WRITERS = {
    "BranchWritePC": "branch",
    "CBWritePC": "branch",
    "SelectInstrSet": "branch",
    "BXWritePC": "interwork",
    "ALUWritePC": "alu_pc",
    "LoadWritePC": "load_pc",
}
EXCEPTIONS = (
    "AArch32_CallSupervisor", "AArch32_SoftwareBreakpoint",
    "AArch32_CallHypervisor", "AArch32_TakeSMCException",
)
BARRIERS = (
    "DataMemoryBarrier", "DataSynchronizationBarrier",
    "InstructionSynchronizationBarrier", "SpeculationBarrier",
    "SpeculativeStoreBypassBarrierToPA", "SpeculativeStoreBypassBarrierToVA",
    "ConsumptionOfSpeculativeDataBarrier", "ClearExclusiveLocal",
    "TraceSynchronizationBarrier",
)
HINTS = (
    "Hint_Yield", "WaitForEvent", "WaitForInterrupt", "SendEvent",
    "SendEventLocal", "Hint_CLRBHB",
)
PREFETCHES = ("Hint_PreloadData", "Hint_PreloadDataForWrite", "Hint_PreloadInstr")
EXCLUSIVES = ("AArch32_ExclusiveMonitorsPass", "AArch32_SetExclusiveMonitors")


@dataclass(frozen=True)
class ConstraintModel:
    """A field-pattern predicate over up to three joined field segments."""

    segments: tuple[tuple[int, int], ...]  # (low, width), most significant first
    negate: bool
    keep: int
    want: int

    @property
    def width(self) -> int:
        return sum(width for _low, width in self.segments)

    def value(self, word: int) -> int:
        result = 0
        for low, width in self.segments:
            result = result << width | (word >> low) & ((1 << width) - 1)
        return result

    def holds(self, word: int) -> bool:
        equal = self.value(word) & self.keep == self.want
        return not equal if self.negate else equal


def normalize_constraint(constraint) -> ConstraintModel:
    if isinstance(constraint, oracle.JoinedConstraint):
        segments = tuple((low, width) for _n, low, width in constraint.segments)
    else:
        segments = ((constraint.low, constraint.width),)
    match = CONSTRAINT_TEXT.fullmatch(constraint.expression)
    if match is None:
        raise ModelError(f"unsupported constraint {constraint.expression!r}")
    pattern = match.group(2)
    width = sum(width for _low, width in segments)
    if len(pattern) != width or width > 16 or len(segments) > MAX_SEGMENTS:
        raise ModelError(f"constraint {constraint.expression!r} does not fit")
    keep = want = 0
    for position, setting in enumerate(pattern):
        bit = width - 1 - position
        if setting != "x":
            keep |= 1 << bit
        if setting == "1":
            want |= 1 << bit
    return ConstraintModel(segments, match.group(1) == "!=", keep, want)


@dataclass
class FormModel:
    index: int
    name: str
    encoding: oracle.Encoding | None  # None for the pseudo-forms
    level: str = "base"
    feature: str | None = None
    availability: str = "implemented"  # or "undefined_at_el0"
    reason: str = ""
    check: str = ""
    nop: bool = False
    conditional: bool = False
    cond_low: int | None = None
    traits: frozenset[str] = frozenset()
    alu_exception_return: bool = False
    program: int | None = None
    page: str = ""

    @property
    def isa(self) -> str:
        return self.encoding.isa if self.encoding is not None else "A32"


@dataclass
class RowModel:
    isa: str  # "A32", "T16" or "T32"
    kind: str  # "instruction", "unpredictable" or "reserved_hint"
    mask: int
    value: int
    constraints: tuple[ConstraintModel, ...]
    form: int
    partition: str

    def matches(self, word: int) -> bool:
        return word & self.mask == self.value and all(
            constraint.holds(word) for constraint in self.constraints
        )


@dataclass
class ProgramModel:
    index: int
    fields: tuple[oracle.Field, ...]
    translation: codegen.Translation
    first_form: str


@dataclass
class Model:
    forms: list[FormModel]
    rows: dict[str, list[RowModel]]  # per ISA tree
    programs: list[ProgramModel]
    members: dict  # (ASL name, kind) -> codegen.Member
    member_list: list  # unique members, sorted by name
    by_name: dict[str, FormModel]
    partitions: dict[str, list]  # every partition per index ISA, for checks


# Execute-pseudocode scan ---------------------------------------------------


def _matching(text: str, start: int, opening: str, closing: str) -> int:
    depth = 0
    for position in range(start, len(text)):
        if text[position] == opening:
            depth += 1
        elif text[position] == closing:
            depth -= 1
            if depth == 0:
                return position
    raise ModelError("unbalanced brackets in Execute pseudocode")


def scan_traits(text: str) -> set[str]:
    traits: set[str] = set()
    for match in re.finditer(r"\bMem[A-Za-z_]*\{", text):
        brace = match.end() - 1
        close = _matching(text, brace, "{", "}")
        position = close + 1
        while text[position].isspace():
            position += 1
        if text[position] != "(":
            raise ModelError("memory access without an address")
        end = _matching(text, position, "(", ")")
        tail = text[end + 1:].lstrip()
        if tail.startswith("=") and not tail.startswith("=="):
            traits.add("stores")
        else:
            traits.add("loads")
    names = set(re.findall(r"\b([A-Za-z_][A-Za-z_0-9]*)\s*\(", text))
    for name, trait in PC_WRITERS.items():
        if name in names:
            traits.add(trait)
    if names & set(EXCEPTIONS):
        traits.add("exception")
    if names & set(BARRIERS):
        traits.add("barrier")
    if names & set(HINTS):
        traits.add("hint")
    if names & set(PREFETCHES):
        traits.add("prefetch")
    if names & set(EXCLUSIVES):
        traits.add("exclusive")
    if FP_STATE.search(text):
        traits.add("fp_state")
    return traits


# Building the model ---------------------------------------------------------


def _level_and_feature(
    encoding: oracle.Encoding,
) -> tuple[str, str | None, str]:
    """(level, required Feature, availability note) of one encoding."""
    features = [f for f in encoding.features]
    level = "simd" if encoding.section == "FP&SIMD" else "base"
    feature = None
    for xml_feature in features:
        if xml_feature in policy.FEATURES:
            if feature is not None and feature != policy.FEATURES[xml_feature]:
                raise ModelError(f"{encoding.name} names two profile features")
            feature = policy.FEATURES[xml_feature]
            level = "complete"
        elif xml_feature not in policy.ABSENT_FEATURES:
            raise ModelError(f"{encoding.name}: unknown feature {xml_feature}")
    if encoding.form_id in policy.ARMV8_PAGES or encoding.name in policy.ARMV8_ENCODINGS:
        level = "complete"
    return level, feature, ""


def _check_policy_names(encodings: list[oracle.Encoding]) -> None:
    pages = {encoding.form_id for encoding in encodings}
    names = {encoding.name for encoding in encodings}
    for page in (
        set(policy.UNDEFINED_AT_EL0) | set(policy.CHECKED_PAGES)
        | policy.NOP_AT_EL0 | policy.ARMV8_PAGES
    ):
        if page not in pages:
            raise ModelError(f"profile policy names unknown page {page}")
    for name in policy.ARMV8_ENCODINGS:
        if name not in names:
            raise ModelError(f"profile policy names unknown encoding {name}")


def build_forms(
    catalog: oracle.ArchitectureCatalog,
) -> tuple[list[FormModel], dict[str, FormModel]]:
    encodings = [e for e in catalog.all_encodings() if not e.alias]
    _check_policy_names(encodings)
    forms = [FormModel(i, name, None) for i, name in enumerate(PSEUDO_FORMS)]
    for encoding in encodings:
        level, feature, _note = _level_and_feature(encoding)
        form = FormModel(len(forms), encoding.name, encoding, level, feature)
        form.page = encoding.form_id
        if encoding.name != re.sub(r"[^A-Za-z0-9_]", "", encoding.name):
            raise ModelError(f"{encoding.name} is not an identifier")
        execute = catalog.execute_text(encoding.file)
        cond = next((f for f in encoding.fields if f.name == "cond"), None)
        form.cond_low = cond.low if cond is not None else None
        # An A32 encoding in the unconditional space has no cond field; a
        # Thumb one is conditional through its own field or through IT.
        form.conditional = "ConditionPassed" in execute and (
            encoding.isa != "A32" or cond is not None
        )
        traits = scan_traits(execute)
        form.alu_exception_return = "ALUExceptionReturn" in execute
        form.traits = frozenset(traits)
        if encoding.form_id in policy.UNDEFINED_AT_EL0:
            form.availability = "undefined_at_el0"
            form.reason = policy.UNDEFINED_AT_EL0[encoding.form_id]
            form.level = "base"
            form.feature = None
        form.check = policy.CHECKED_PAGES.get(encoding.form_id, "")
        form.nop = encoding.form_id in policy.NOP_AT_EL0
        forms.append(form)
    return forms, {form.name: form for form in forms}


def classify_thumb(partition) -> str:
    """Whether a T32-index partition is a 16- or a 32-bit instruction.

    A halfword starting 0b11101, 0b11110 or 0b11111 begins a 32-bit
    instruction; the partition must lie wholly on one side of that line.
    """
    mask, value = partition.mask >> 27, partition.value >> 27
    long_starts = {0b11101, 0b11110, 0b11111}
    sides = {
        top in long_starts
        for top in range(32)
        if top & mask == value & mask
    }
    if len(sides) != 1:
        raise ModelError(f"{partition.name} straddles the Thumb length rule")
    if sides == {True}:
        return "T32"
    if partition.mask & 0xFFFF:
        raise ModelError(f"{partition.name}: 16-bit partition with low bits")
    return "T16"


def build_rows(
    index: oracle.DecodeIndex, forms: dict[str, FormModel]
) -> tuple[dict[str, list[RowModel]], dict[str, list]]:
    rows: dict[str, list[RowModel]] = {"A32": [], "T16": [], "T32": []}
    partitions: dict[str, list] = {}
    for index_isa in ("A32", "T32"):
        partitions[index_isa] = index.partitions(index_isa)
        for partition in partitions[index_isa]:
            if partition.kind == "unallocated":
                continue
            isa = "A32" if index_isa == "A32" else classify_thumb(partition)
            constraints = tuple(
                normalize_constraint(c) for c in partition.constraints
            )
            if partition.kind == "nop-hint":
                kind, form = "reserved_hint", forms["ReservedHint"]
            else:
                form = forms.get(partition.name)
                if form is None:
                    if partition.kind != "unpredictable":
                        raise ModelError(
                            f"partition {partition.name} has no encoding"
                        )
                    form = forms["Undefined"]
                kind = "unpredictable" if partition.kind == "unpredictable" else "instruction"
                if form.encoding is not None and form.encoding.isa != isa:
                    raise ModelError(
                        f"partition {partition.name} is {isa}, encoding is "
                        f"{form.encoding.isa}"
                    )
            rows[isa].append(
                RowModel(
                    isa, kind, partition.mask, partition.value, constraints,
                    form.index, partition.name,
                )
            )
    return rows, partitions


def build_programs(
    catalog: oracle.ArchitectureCatalog, forms: list[FormModel]
) -> tuple[list[ProgramModel], dict, list]:
    """Translate every distinct Decode pseudocode of the implemented forms.

    A program is the C++ the translation renders: pseudocode that differs in
    text but translates to the same field extraction and body shares one
    program.
    """
    parsed: dict[str, list] = {}
    users: dict[tuple[str, tuple], list[FormModel]] = {}
    members = codegen.Members()
    for form in forms:
        encoding = form.encoding
        if encoding is None or form.availability != "implemented":
            continue
        text = catalog.decode_text(encoding.file, encoding.psname)
        if text not in parsed:
            parsed[text] = asl.parse(text, strict=True)
            codegen.collect_declarations(parsed[text], members, encoding.name)
        users.setdefault((text, encoding.fields), []).append(form)
    resolved = members.resolve()
    programs: list[ProgramModel] = []
    rendered: dict[tuple, int] = {}
    for (text, fields), forms_using in users.items():
        extents = {f.name: (f.low, f.width) for f in fields}
        translator = codegen.Translator(extents, resolved, forms_using[0].name)
        translator.statements(parsed[text])
        translation = translator.out
        key = (
            tuple((name, *extents[name]) for name in sorted(translation.fields_used)),
            tuple(translation.body),
        )
        index = rendered.get(key)
        if index is None:
            index = rendered[key] = len(programs)
            programs.append(
                ProgramModel(index, fields, translation, forms_using[0].name)
            )
        for form in forms_using:
            form.program = index
    unique = {id(m): m for m in resolved.values()}
    member_list = sorted(unique.values(), key=lambda m: m.name)
    return programs, resolved, member_list


def build_model(
    catalog: oracle.ArchitectureCatalog, index: oracle.DecodeIndex
) -> Model:
    forms, by_name = build_forms(catalog)
    rows, partitions = build_rows(index, by_name)
    programs, members, member_list = build_programs(catalog, forms)
    return Model(forms, rows, programs, members, member_list, by_name, partitions)
