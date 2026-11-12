#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the SVE/SME encoding table both arm64 engines decode against.

The virtual CPU's SVE and SME profiles decide every SVE and SME word before
an engine executes anything. The SME profile is FEAT_SME, FEAT_SME2,
FEAT_SME_F64F64 and FEAT_SME_I16I64; the SVE profile is FEAT_SVE, FEAT_SVE2,
FEAT_SVE_AES, FEAT_SVE_PMULL128 and FEAT_SVE_SHA3; both include the SVE
forms of FEAT_BF16 and FEAT_I8MM. FEAT_SME_FA64 and every other SVE or SME
extension are outside them, FEAT_SVE_BitPerm and FEAT_SVE_SM4 among them
until a workload needs them. This generator reads the pinned Arm A64 XML
release and writes darwin/cpu/arm64/detail/sme_encodings.hpp with, for every
non-alias encoding of the SVE and SME indexes:

- the encoding's mask and value, split where a field value changes the
  features it needs, reserves the encoding, or changes its access check;
- the features under which the virtual CPU implements it in streaming mode
  (PSTATE.SM = 1) and outside it (PSTATE.SM = 0), over the profile's
  feature tokens, or none in a mode where it never executes;
- the minimum streaming vector length a MaxImplementedSVL() guard demands;
- the access checks its execute pseudocode names and the streaming-mode
  state they demand (streaming, ZA, both, or either mode);
- its register-operand layout and its memory shape, with the vector length
  that sizes it; and
- a witness word that LLVM MC decodes to the encoding.

A companion table classifies every FP&SIMD encoding as AdvSIMD (it traps
while PSTATE.SM is 1, since the profile lacks FEAT_SME_FA64) or FP (legal in
streaming mode), from AArch64_CheckFPAdvSIMDEnabled() versus
AArch64_CheckFPEnabled(), evaluated per field value.

Everything comes from the XML and LLVM MC; a shape the generator does not
recognise fails the generation rather than taking a default.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import itertools
import re
import sys
import tarfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

import a64_asl as asl
from a64_architecture_oracle import (
    SOURCE_ORACLE,
    ArchitectureCatalog,
    ArchitectureVariant,
    parse_feature_expression,
    resolve_feature_fields,
)
from a64_bounded_generation import (
    BoundedCommandRunner,
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_llvm_oracle import decoder_instructions, find_llvm_mc, require_pinned_llvm
from a64_one_shot_publication import OneShotPublication
from generate_a64_instruction_allocations import archive_root


# The profile's features that a row's requirement names, as the runtime's
# ArmFeature tokens. FEAT_BF16 and FEAT_I8MM follow the host; the rest are the
# SME and SVE profiles. Every other FEAT_ name is unimplemented, except the
# base features below, which every host the virtual CPU runs on implements.
PROFILE_FEATURES = {
    "FEAT_SME": "sme",
    "FEAT_SME2": "sme2",
    "FEAT_SME_F64F64": "sme_f64f64",
    "FEAT_SME_I16I64": "sme_i16i64",
    "FEAT_BF16": "bf16",
    "FEAT_I8MM": "i8mm",
    "FEAT_SVE": "sve",
    "FEAT_SVE2": "sve2",
    "FEAT_SVE_AES": "sve_aes",
    "FEAT_SVE_PMULL128": "sve_pmull128",
    "FEAT_SVE_SHA3": "sve_sha3",
}
# SVE extensions the virtual CPU withholds until a workload needs them (the
# user's decision of 2026-10-07). Each has its requirement bit already, so
# moving its line into PROFILE_FEATURES is the whole change here.
WITHHELD_FEATURES = {
    "FEAT_SVE_BitPerm": "sve_bitperm",
    "FEAT_SVE_SM4": "sve_sm4",
}
BASE_FEATURES = frozenset({"FEAT_FP", "FEAT_AdvSIMD", "FEAT_FP16"})
# The SVE features, which decide only what executes outside streaming mode:
# without FEAT_SME_FA64 or a FEAT_SSVE_ extension, streaming mode executes
# what FEAT_SME gives it (encoding_outcomes() checks this for every word).
SVE_FEATURES = frozenset(
    f for f in {**PROFILE_FEATURES, **WITHHELD_FEATURES} if f.startswith("FEAT_SVE")
)
# What each feature implies: FEAT_SVE2 needs FEAT_SVE, its crypto
# extensions need it, FEAT_SVE_PMULL128 needs FEAT_SVE_AES, and SME's
# extensions need FEAT_SME. One more is the profile's own: the virtual CPU
# implements FEAT_SVE2 wherever it implements FEAT_SVE.
IMPLIES = {
    "FEAT_SVE": {"FEAT_SVE2"},
    "FEAT_SVE2": {"FEAT_SVE"},
    "FEAT_SVE_AES": {"FEAT_SVE2"},
    "FEAT_SVE_PMULL128": {"FEAT_SVE_AES"},
    "FEAT_SVE_SHA3": {"FEAT_SVE2"},
    "FEAT_SVE_BitPerm": {"FEAT_SVE2"},
    "FEAT_SVE_SM4": {"FEAT_SVE2"},
    "FEAT_SME2": {"FEAT_SME"},
    "FEAT_SME_F64F64": {"FEAT_SME"},
    "FEAT_SME_I16I64": {"FEAT_SME"},
}
# Maximum vector lengths, in bits, that MaxImplementedSVL() guards compare.
VECTOR_LENGTHS = (128, 256, 512, 1024, 2048)

ACCESS_CHECKS = (
    "CheckSVEEnabled",
    "CheckStreamingSVEEnabled",
    "CheckNonStreamingSVEEnabled",
    "CheckStreamingSVEAndZAEnabled",
    "CheckSMEAndZAEnabled",
    "CheckSMEZT0Enabled",
    "CheckSMEEnabled",
    "CheckFPMREnabled",
    "AArch64_CheckFPEnabled",
    "AArch64_CheckFPAdvSIMDEnabled",
)
ACCESS_CHECK = re.compile(r"\b(" + "|".join(ACCESS_CHECKS) + r")\(")
# What each check demands of PSTATE on a CPU with FEAT_SME and without
# FEAT_SVE or FEAT_SME_FA64, from their shared pseudocode: CheckSVEEnabled
# becomes CheckStreamingSVEEnabled, and CheckNonStreamingSVEEnabled traps
# outside streaming mode through it and inside streaming mode itself. These
# decide a row's streaming-mode requirement, and its mode records them, so
# the streaming bit keeps its meaning; NON_STREAMING_CHECKS add FEAT_SVE.
CHECK_DEMANDS = {
    "CheckSVEEnabled": {"sm"},
    "CheckStreamingSVEEnabled": {"sm"},
    "CheckNonStreamingSVEEnabled": {"never"},
    "CheckStreamingSVEAndZAEnabled": {"sm", "za"},
    "CheckSMEAndZAEnabled": {"za"},
    "CheckSMEZT0Enabled": {"za"},
    "CheckSMEEnabled": set(),
}
# The feature each check needs to pass outside streaming mode (PSTATE.SM =
# 0), or None where it traps there: CheckSVEEnabled and
# CheckNonStreamingSVEEnabled reach CheckOriginalSVEEnabled only with
# FEAT_SVE (with FEAT_SME and without FEAT_SVE they trap), the streaming
# checks trap, and the SME checks need FEAT_SME.
NON_STREAMING_CHECKS = {
    "CheckSVEEnabled": "FEAT_SVE",
    "CheckStreamingSVEEnabled": None,
    "CheckNonStreamingSVEEnabled": "FEAT_SVE",
    "CheckStreamingSVEAndZAEnabled": None,
    "CheckSMEAndZAEnabled": "FEAT_SME",
    "CheckSMEZT0Enabled": "FEAT_SME",
    "CheckSMEEnabled": "FEAT_SME",
}


class GenerationError(ValueError):
    pass


@dataclass
class EncodingSource:
    section: str
    form: str
    stable_id: str
    file: str
    variant: ArchitectureVariant
    decode: list
    checks_prefix: list
    check_names: tuple[str, ...]
    execute_text: str
    decode_text: str
    # Field values an encoding box excludes with "Z" (0) and "N" (1) cells,
    # as (low, width, value): LD1's post-index Rm != 11111 and LDR's
    # extended-register option != 011. The catalog's variants omit them.
    excluded: tuple[tuple[int, int, int], ...] = ()

    def matches(self, word: int) -> bool:
        return self.variant.matches(word) and not any(
            (word >> low) & ((1 << width) - 1) == value
            for low, width, value in self.excluded
        )


def excluded_fields(
    instruction_class: ET.Element, encoding: str
) -> tuple[tuple[int, int, int], ...]:
    result = []
    for element in instruction_class.findall("encoding"):
        if element.get("name") != encoding:
            continue
        for box in element.findall("box"):
            cells = []
            for cell in box.findall("c"):
                cells += [(cell.text or "").strip()] * int(cell.get("colspan", "1"))
            if not set(cells) & {"Z", "N"}:
                continue
            width = int(box.get("width", "1"))
            if len(cells) != width or not set(cells) <= {"Z", "N"}:
                raise GenerationError(f"{encoding}: box mixes Z or N with other cells")
            value = int("".join("1" if c == "N" else "0" for c in cells), 2)
            result.append((int(box.get("hibit")) - width + 1, width, value))
    return tuple(result)


def iclass_of(root: ET.Element, encoding: str) -> ET.Element:
    for instruction_class in root.findall("./classes/iclass"):
        if any(e.get("name") == encoding for e in instruction_class.findall("encoding")):
            return instruction_class
    raise GenerationError(f"encoding {encoding} has no instruction class")


def section_text(element: ET.Element, section: str) -> str:
    texts = [
        "".join(item.itertext())
        for item in element.findall("./ps_section/ps/pstext")
        if item.get("section") == section
    ]
    if len(texts) != 1:
        raise GenerationError(f"expected one {section} section, found {len(texts)}")
    return texts[0]


def check_prefix(execute: str, where: str) -> tuple[list, tuple[str, ...]]:
    """Parse the leading statements of an execute section that select its
    access checks, and require that no access check follows them."""
    tokens = asl.tokenize(execute)
    parser = asl.Parser(tokens)
    statements = []
    names: list[str] = []
    end = 0
    kept = 0
    while parser.peek() is not None:
        start = parser.index
        token = parser.peek()
        if token not in ACCESS_CHECKS and token not in ("if", "let"):
            break
        try:
            statement = parser.statement()
        except asl.AslError:
            parser.index = start
            break
        if statement[0] == "let":
            statements.append(statement)
            continue
        leaves = _check_leaves(statement)
        if leaves is None:
            parser.index = start
            break
        statements.append(statement)
        names.extend(leaves)
        end = parser.index
        kept = len(statements)
    statements = statements[:kept]
    parser.index = end
    rest = " ".join(tokens[parser.index:])
    late = [m.group(1) for m in ACCESS_CHECK.finditer(rest.replace(" (", "("))]
    if late:
        raise GenerationError(f"{where}: access check {late[0]} after the prologue")
    return statements, tuple(dict.fromkeys(names))


def _check_leaves(statement) -> list[str] | None:
    """The access checks a statement can call, or None if it does more."""
    kind = statement[0]
    if kind == "call-statement":
        node = statement[1]
        if node[0] == "call" and node[1] in ACCESS_CHECKS:
            return [node[1]]
        return None
    if kind == "if":
        result: list[str] = []
        for _test, body in statement[1]:
            for item in body:
                leaves = _check_leaves(item)
                if leaves is None:
                    return None
                result += leaves
        for item in statement[2]:
            leaves = _check_leaves(item)
            if leaves is None:
                return None
            result += leaves
        return result
    return None


class CheckRecorder(asl.Evaluator):
    def __init__(self, environment: asl.Environment):
        super().__init__(environment)
        self.checks: list[str] = []

    def execute(self, statement) -> None:
        if statement[0] == "call-statement":
            node = statement[1]
            if node[0] == "call" and node[1] in ACCESS_CHECKS:
                self.checks.append(node[1])
                return
        super().execute(statement)


def load_sources(
    catalog: ArchitectureCatalog, sections: tuple[str, ...]
) -> list[EncodingSource]:
    result: list[EncodingSource] = []
    for stable_id, (section, form, filename) in catalog.stable_forms.items():
        if section not in sections:
            continue
        root = catalog._tree(filename)
        if root.get("type") == "alias":
            continue
        if root.get("type") != "instruction":
            raise GenerationError(f"{filename}: unknown page type {root.get('type')!r}")
        execute = section_text(root, "Execute")
        prefix, names = check_prefix(execute, filename)
        for variant in catalog.all_variants(section, form):
            instruction_class = iclass_of(root, variant.encoding)
            decode_text = section_text(instruction_class, "Decode")
            try:
                decode = asl.parse_statements(decode_text)
            except asl.AslError as error:
                decode = [("opaque", f"unparsed decode: {error}", decode_text)]
            result.append(
                EncodingSource(
                    section=section,
                    form=form,
                    stable_id=stable_id,
                    file=filename,
                    variant=variant,
                    decode=decode,
                    checks_prefix=prefix,
                    check_names=names,
                    execute_text=execute,
                    decode_text=decode_text,
                    excluded=excluded_fields(instruction_class, variant.encoding),
                )
            )
    return result


def _statement_bindings(statements: list, result: dict[str, list]) -> None:
    """Map each name the decode binds to every expression bound to it."""
    for statement in statements:
        kind = statement[0]
        if kind in ("let", "assign"):
            result.setdefault(statement[1], []).append(statement[2])
        elif kind == "if":
            for _test, body in statement[1]:
                _statement_bindings(body, result)
            _statement_bindings(statement[2], result)
        elif kind == "case":
            for _patterns, body in statement[2]:
                _statement_bindings(body, result)
            if statement[3] is not None:
                _statement_bindings(statement[3], result)
        elif kind == "loop":
            _statement_bindings(statement[2], result)


def _decision_names(statements: list) -> set[str]:
    """Names read by every decode condition that can end in UNDEFINED or
    change a binding."""
    result: set[str] = set()
    for statement in statements:
        kind = statement[0]
        if kind == "if":
            relevant = any(
                asl._ends_in_undefined(body) or asl._assigns(body)
                for _test, body in statement[1]
            ) or asl._ends_in_undefined(statement[2]) or asl._assigns(statement[2])
            for test, body in statement[1]:
                if relevant:
                    result |= asl.names_in(test)
                result |= _decision_names(body)
            result |= _decision_names(statement[2])
        elif kind == "case":
            result |= asl.names_in(statement[1])
            for patterns, body in statement[2]:
                result |= _decision_names(body)
            if statement[3] is not None:
                result |= _decision_names(statement[3])
        elif kind == "loop":
            result |= _decision_names(statement[2])
    return result


def _closure(names: set[str], bindings: dict[str, list], fields: set[str]) -> set[str]:
    """The fields that names depend on through the decode's bindings."""
    seen: set[str] = set()
    pending = list(names)
    result: set[str] = set()
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        if name in bindings:
            for expression in bindings[name]:
                pending.extend(asl.names_in(expression))
        elif name in fields:
            result.add(name)
    return result


def _feature_names(text: str) -> set[str]:
    return set(re.findall(r"\bFEAT_[A-Za-z0-9_]+\b", text))


@dataclass(frozen=True)
class Outcome:
    """What one field assignment of an encoding means on this profile: the
    features it needs in streaming mode (requirement) and outside it
    (non_streaming), as alternatives, or none in a mode it never executes
    in."""

    kind: str  # "legal", "never", "absent", "reserved", "other"
    requirement: tuple[tuple[str, ...], ...] = ()
    min_svl: int = 0
    checks: tuple[str, ...] = ()
    non_streaming: tuple[tuple[str, ...], ...] = ()


def _implied(feature: str) -> frozenset[str]:
    """Every feature a feature implies, through IMPLIES."""
    seen: set[str] = set()
    pending = [feature]
    while pending:
        for implied in IMPLIES.get(pending.pop(), ()):
            if implied not in seen:
                seen.add(implied)
                pending.append(implied)
    return frozenset(seen)


def _closed(subset: frozenset[str], universe: list[str]) -> bool:
    """Whether a combination of the universe's features has every feature
    of the universe that its features imply."""
    return all(_implied(f) & set(universe) <= subset for f in subset)


def _evaluate_once(
    source: EncodingSource, word: int, fields: dict, features: frozenset[str],
    svl: int | None,
):
    """None when the word is UNDEFINED under these inputs, else the checks."""
    for expression in source.variant.feature_expressions:
        tree = parse_feature_expression(expression)
        resolved = resolve_feature_fields(
            tree, {n: (lo, w) for n, lo, w in source.variant.fields}, word
        )
        if resolved is False:
            return None
        if resolved is True:
            continue
        if not _holds(resolved, features):
            return None
    environment = asl.Environment(fields=dict(fields), features=features, max_svl=svl)
    evaluator = CheckRecorder(environment)
    try:
        evaluator.run(source.decode)
    except asl.Undefined:
        return None
    evaluator.run(source.checks_prefix)
    return tuple(evaluator.checks)


def _holds(node, features: frozenset[str]) -> bool:
    kind = node[0]
    if kind == "feature":
        return node[1] in features
    if kind == "and":
        return all(_holds(child, features) for child in node[1])
    if kind == "or":
        return any(_holds(child, features) for child in node[1])
    raise GenerationError(f"feature expression still names a field: {node!r}")


def _minimal(sets: list[frozenset[str]]) -> list[frozenset[str]]:
    return sorted(
        {s for s in sets if not any(other < s for other in sets)},
        key=lambda s: (len(s), sorted(s)),
    )


def field_values(variant: ArchitectureVariant, word: int, variable: set[str]) -> dict:
    """Concrete values for the fields fixed in the word, opaque for the rest."""
    result = {}
    for name, low, width in variant.fields:
        bits = ((1 << width) - 1) << low
        if name in variable or (variant.mask & bits) == bits:
            result[name] = asl.Bits(width, (word >> low) & ((1 << width) - 1))
        else:
            result[name] = asl.Opaque(f"field {name}")
    return result


def encoding_outcomes(source: EncodingSource) -> dict[int, Outcome]:
    """Map each assignment of the encoding's deciding bits to its outcome."""
    variant = source.variant
    field_names = {name for name, _low, _width in variant.fields}
    bindings: dict[str, list] = {}
    _statement_bindings(source.decode, bindings)
    names = _decision_names(source.decode)
    for statement in source.checks_prefix:
        names |= _decision_names([statement]) | _prefix_names(statement)
    for expression in variant.feature_expressions:
        names |= _expression_fields(parse_feature_expression(expression))
    names |= {constraint.name for constraint in variant.constraints}
    relevant = _closure(names, bindings, field_names)
    bit_positions = _free_bits(source, relevant)
    if len(bit_positions) > 14:
        raise GenerationError(
            f"{source.file} {variant.encoding}: {len(bit_positions)} deciding bits"
        )
    text = source.decode_text + " " + " ".join(variant.feature_expressions) + " " + \
        source.execute_text.split("\n")[0]
    for statement in source.checks_prefix:
        text += " " + repr(statement)
    symbolic = sorted(
        name for name in _feature_names(text) if name in PROFILE_FEATURES
    )
    lengths = VECTOR_LENGTHS if "MaxImplemented" in source.decode_text else (None,)
    # In streaming mode the CPU has FEAT_SME, and the SVE features change
    # nothing; outside it, the combinations of the named features and the
    # two the checks consult that IMPLIES allows.
    streaming_names = [f for f in symbolic if f not in SVE_FEATURES]
    subsets = [
        frozenset(c)
        for size in range(len(streaming_names) + 1)
        for c in itertools.combinations(streaming_names, size)
    ]
    universe = sorted(set(symbolic) | {"FEAT_SVE", "FEAT_SME"})
    outside_subsets = [
        frozenset(c)
        for size in range(len(universe) + 1)
        for c in itertools.combinations(universe, size)
        if _closed(frozenset(c), universe)
    ]
    every_sve = frozenset(f for f in PROFILE_FEATURES if f in SVE_FEATURES)
    where = f"{source.file} {variant.encoding}"
    result: dict[int, Outcome] = {}
    for assignment in range(1 << len(bit_positions)):
        word = variant.value
        for index, bit in enumerate(bit_positions):
            if assignment >> index & 1:
                word |= 1 << bit
        if not source.matches(word):
            result[word] = Outcome("other")
            continue
        fields = field_values(variant, word, relevant)

        def evaluate(features: frozenset[str], length):
            try:
                return _evaluate_once(source, word, fields, features, length)
            except asl.AslError as error:
                raise GenerationError(f"{where} {word:08x}: {error}") from error

        table = {}
        outside = {}
        for subset in subsets:
            for length in lengths:
                table[(subset, length)] = evaluate(BASE_FEATURES | subset, length)
                with_sme = BASE_FEATURES | subset | {"FEAT_SME"}
                if _streaming_legal(evaluate(with_sme, length), where) != \
                        _streaming_legal(evaluate(with_sme | every_sve, length), where):
                    raise GenerationError(
                        f"{where} {word:08x}: the SVE profile changes what "
                        "streaming mode executes"
                    )
        for subset in outside_subsets:
            for length in lengths:
                outside[(subset, length)] = evaluate(BASE_FEATURES | subset, length)
        result[word] = _summarize(
            source, word, table, subsets, lengths, outside, outside_subsets
        )
    return result


def _free_bits(source: EncodingSource, relevant: set[str]) -> list[int]:
    """The bits of the relevant fields and of the encoding's constraints
    that its mask leaves free."""
    variant = source.variant
    spans = [(low, width) for name, low, width in variant.fields if name in relevant]
    spans += [(c.low, c.width) for c in variant.constraints]
    spans += [(low, width) for low, width, _value in source.excluded]
    return sorted(
        {
            bit
            for low, width in spans
            for bit in range(low, low + width)
            if not variant.mask >> bit & 1
        }
    )


def _prefix_names(statement) -> set[str]:
    if statement[0] == "if":
        result: set[str] = set()
        for test, body in statement[1]:
            result |= asl.names_in(test)
            for item in body:
                result |= _prefix_names(item)
        for item in statement[2]:
            result |= _prefix_names(item)
        return result
    return set()


def _expression_fields(node) -> set[str]:
    if node[0] == "field":
        return {node[1]}
    if node[0] == "feature":
        return set()
    result: set[str] = set()
    for child in node[1]:
        result |= _expression_fields(child)
    return result


def _streaming_demands(checks: tuple[str, ...], where: str) -> set[str]:
    """What a decodable word's checks demand in streaming mode."""
    if not checks:
        raise GenerationError(f"{where}: execute names no access check")
    demands: set[str] = set()
    for check in checks:
        if check == "CheckFPMREnabled":
            raise GenerationError(f"{where}: FPMR check in a decodable word")
        if check not in CHECK_DEMANDS:
            raise GenerationError(f"{where}: unexpected check {check}")
        demands |= CHECK_DEMANDS[check]
    return demands


def _streaming_legal(checks: tuple[str, ...] | None, where: str) -> bool:
    return checks is not None and "never" not in _streaming_demands(checks, where)


def _non_streaming_legal(checks: tuple[str, ...], features: frozenset[str]) -> bool:
    return all(NON_STREAMING_CHECKS[check] in features for check in checks)


def _requirement(where, legal: dict, subsets, lengths) -> tuple[list, int]:
    """The minimal feature sets of one mode's legal combinations, which must
    imply legality exactly, and the minimum SVL they share."""
    minimal = _minimal(list(legal))
    for subset in subsets:
        implied = any(m <= subset for m in minimal)
        if implied != (subset in legal):
            raise GenerationError(f"{where}: legality is not monotonic in features")
    min_lengths = set()
    for subset in legal:
        lengths_ok = sorted(l for l in legal[subset] if l is not None)
        if lengths != (None,):
            first = lengths_ok[0]
            if lengths_ok != [l for l in VECTOR_LENGTHS if l >= first]:
                raise GenerationError(f"{where}: legality is not monotonic in SVL")
            min_lengths.add(first)
        else:
            min_lengths.add(0)
    if len(min_lengths) != 1:
        raise GenerationError(f"{where}: minimum SVL depends on features")
    min_svl = min_lengths.pop()
    if min_svl == VECTOR_LENGTHS[0]:
        min_svl = 0
    return minimal, min_svl


def _summarize(source, word, table, subsets, lengths, outside, outside_subsets) -> Outcome:
    where = f"{source.file} {source.variant.encoding} {word:08x}"
    legal: dict[frozenset[str], set] = {}
    decodable: list[frozenset[str]] = []
    checks_by_subset: dict[frozenset[str], tuple[str, ...]] = {}
    for (subset, length), checks in table.items():
        if checks is None:
            continue
        demands = _streaming_demands(checks, where)
        previous = checks_by_subset.setdefault(subset, checks)
        if previous != checks:
            raise GenerationError(f"{where}: checks depend on the vector length")
        if "never" in demands:
            decodable.append(subset)
        else:
            legal.setdefault(subset, set()).add(length)
    outside_legal: dict[frozenset[str], set] = {}
    outside_checks: dict[frozenset[str], tuple[str, ...]] = {}
    for (subset, length), checks in outside.items():
        if checks is None:
            continue
        _streaming_demands(checks, where)
        previous = outside_checks.setdefault(subset, checks)
        if previous != checks:
            raise GenerationError(f"{where}: checks depend on the vector length")
        if _non_streaming_legal(checks, subset):
            outside_legal.setdefault(subset, set()).add(length)
        else:
            decodable.append(subset)
    if not legal and not outside_legal and not decodable:
        # Reserved: the profile has the encoding's features, but the decode
        # pseudocode rejects this field value.
        profile = frozenset(PROFILE_FEATURES) | BASE_FEATURES
        variant = source.variant
        fields = {n: (lo, w) for n, lo, w in variant.fields}
        for expression in variant.feature_expressions:
            resolved = resolve_feature_fields(
                parse_feature_expression(expression), fields, word
            )
            if resolved is False or (
                resolved is not True and not _holds(resolved, profile)
            ):
                return Outcome("absent")
        return Outcome("reserved")
    if not legal and not outside_legal:
        minimal = _minimal(decodable)
        return Outcome(
            "never",
            tuple(tuple(sorted(PROFILE_FEATURES[f] for f in m)) for m in minimal),
            0,
            sorted(set(checks_by_subset.values()) | set(outside_checks.values()))[0],
        )
    checks = {checks_by_subset[s] for s in legal} | {outside_checks[s] for s in outside_legal}
    if len(checks) != 1:
        raise GenerationError(f"{where}: legal checks depend on features")
    checks_called = checks.pop()
    requirement: list[tuple[str, ...]] = []
    min_svl = 0
    if legal:
        minimal, min_svl = _requirement(where, legal, subsets, lengths)
        # Without the SVE profile, the virtual CPU implements an SVE or SME
        # encoding in streaming mode only through FEAT_SME, which FEAT_SME2
        # and the others imply; the requirement names it whatever the XML's
        # expression is.
        requirement = sorted(
            {tuple(sorted({PROFILE_FEATURES[f] for f in m} | {"sme"})) for m in minimal},
            key=lambda item: (len(item), item),
        )
    non_streaming: list[tuple[str, ...]] = []
    if outside_legal:
        minimal, outside_svl = _requirement(where, outside_legal, outside_subsets, lengths)
        non_streaming = sorted(
            {tuple(sorted(PROFILE_FEATURES[f] for f in m)) for m in minimal},
            key=lambda item: (len(item), item),
        )
        if legal and outside_svl != min_svl:
            raise GenerationError(f"{where}: minimum SVL depends on the mode")
        if not legal and outside_svl != 0:
            raise GenerationError(f"{where}: a non-streaming row needs an SVL")
        min_svl = outside_svl
    # The mode's streaming bit keeps its meaning where the SVE profile is
    # absent: a row without it executes outside streaming mode exactly where
    # it does inside, and any other row only through FEAT_SVE.
    streaming = "sm" in _streaming_demands(checks_called, where)
    if legal and not streaming and non_streaming != requirement:
        raise GenerationError(f"{where}: an either-mode row's requirements differ")
    if (streaming or not legal) and any("sve" not in alt for alt in non_streaming):
        raise GenerationError(f"{where}: executes outside streaming mode without FEAT_SVE")
    return Outcome(
        "legal", tuple(requirement), min_svl, checks_called, tuple(non_streaming)
    )


@dataclass(frozen=True)
class Row:
    source: EncodingSource
    mask: int
    value: int
    outcome: Outcome


def cubes(words: set[int], positions: list[int], base: int) -> list[tuple[int, int]]:
    """A disjoint (mask, value) cover of words, which differ from base only
    in positions, by recursive halving."""
    result: list[tuple[int, int]] = []

    def visit(index: int, mask: int, value: int, members: set[int]) -> None:
        if not members:
            return
        remaining = len(positions) - index
        if len(members) == 1 << remaining:
            result.append((mask, value))
            return
        bit = positions[index]
        low = {w for w in members if not w >> bit & 1}
        visit(index + 1, mask | 1 << bit, value, low)
        visit(index + 1, mask | 1 << bit, value | 1 << bit, members - low)

    visit(0, 0, 0, words)
    return result


def encoding_rows(source: EncodingSource) -> list[Row]:
    outcomes = encoding_outcomes(source)
    variant = source.variant
    positions = sorted(
        {bit for word in outcomes for bit in range(32)
         if (word ^ variant.value) >> bit & 1}
        | set(_deciding_positions(outcomes, variant))
    )
    groups: dict[Outcome, set[int]] = {}
    for word, outcome in outcomes.items():
        if outcome.kind in ("other", "reserved"):
            continue
        groups.setdefault(outcome, set()).add(word)
    rows = []
    for outcome, words in sorted(groups.items(), key=lambda item: min(item[1])):
        for mask, value in cubes(words, positions, variant.value):
            rows.append(
                Row(source, variant.mask | mask, variant.value | value, outcome)
            )
    return rows


def _deciding_positions(outcomes, variant) -> list[int]:
    # Every enumerated word differs from the encoding value only in the
    # deciding bits, so the bits that vary across them are those bits.
    words = list(outcomes)
    varying = 0
    for word in words:
        varying |= word ^ words[0]
    return [bit for bit in range(32) if varying >> bit & 1]


def check_disjoint(rows: list[Row]) -> None:
    by_top: dict[int, list[Row]] = {}
    for row in rows:
        by_top.setdefault(row.value >> 24 if row.mask >> 24 == 0xFF else -1, []).append(row)
    ordered = sorted(rows, key=lambda r: (r.value & 0xFF000000, r.mask))
    for index, first in enumerate(ordered):
        for second in ordered[index + 1:]:
            common = first.mask & second.mask
            if (first.value ^ second.value) & common == 0:
                raise GenerationError(
                    "rows overlap: "
                    f"{first.source.variant.encoding} {first.mask:08x}/{first.value:08x}"
                    f" and {second.source.variant.encoding} "
                    f"{second.mask:08x}/{second.value:08x}"
                )


# Register operands.

# Execute accessors whose first argument is a register number, and the
# operand kind each gives the register variable it names.
REGISTER_ACCESSORS = {
    "Z": "z",
    "P": "p",
    "V": "v",
    "X": "gpr",
    "ZAtile": "za_tile",
    "ZAslice": "za_tile",
}
ACCESSOR_CALL = re.compile(
    r"\b(" + "|".join(REGISTER_ACCESSORS) + r")\{[^}]*\}\("
)


@dataclass(frozen=True)
class Segment:
    """Part of a register number, most significant first: a field's bits or
    a constant."""

    field: str | None
    low: int
    width: int
    constant: int = 0


@dataclass(frozen=True)
class Operand:
    name: str
    kind: str  # z, p, v, gpr, gpr_or_sp, za_tile
    segments: tuple[Segment, ...]
    count: int = 1
    stride: int = 1

    def number(self, word: int) -> int:
        result = 0
        for segment in self.segments:
            part = segment.constant if segment.field is None else \
                (word >> segment.low) & ((1 << segment.width) - 1)
            result = result << segment.width | part
        return result

    def bits(self) -> int:
        result = 0
        for segment in self.segments:
            if segment.field is not None:
                result |= ((1 << segment.width) - 1) << segment.low
        return result


def register_segments(
    node, fields: dict[str, tuple[int, int]]
) -> tuple[Segment, ...] | None:
    """Segments of UInt(concatenation of fields, field slices and constants)."""
    if node[0] != "call" or node[1] != "UInt" or len(node[2]) != 1:
        return None
    parts: list = []

    def flatten(item) -> bool:
        if item[0] == "binary" and item[1] == "::":
            return flatten(item[2]) and flatten(item[3])
        parts.append(item)
        return True

    flatten(node[2][0])
    result: list[Segment] = []
    for part in parts:
        if part[0] == "bits":
            if part[1].wild:
                return None
            result.append(Segment(None, 0, part[1].width, part[1].value))
        elif part[0] == "name" and part[1] in fields:
            low, width = fields[part[1]]
            result.append(Segment(part[1], low, width))
        elif part[0] == "slice" and part[1][0] == "name" and part[1][1] in fields \
                and len(part[2]) == 1:
            low, width = fields[part[1][1]]
            high_node, low_node = part[2][0]
            if high_node[0] != "num" or low_node[0] != "num":
                return None
            high, bottom = high_node[1], low_node[1]
            if high >= width or bottom > high:
                return None
            result.append(Segment(part[1][1], low + bottom, high - bottom + 1))
        else:
            return None
    if not any(segment.field for segment in result):
        return None
    return tuple(result)


def _first_argument(text: str, start: int) -> str:
    depth = 0
    for index in range(start, len(text)):
        character = text[index]
        if character in "([{":
            depth += 1
        elif character in ")]}":
            if depth == 0:
                return text[start:index]
            depth -= 1
        elif character == "," and depth == 0:
            return text[start:index]
    raise GenerationError("unterminated accessor call")


def _aliases(name: str, execute: str) -> set[str]:
    result = {name}
    changed = True
    while changed:
        changed = False
        for match in re.finditer(
            r"\b(?:let|var|constant)\s+([A-Za-z_]\w*)\s*:\s*integer\s*=\s*"
            r"([A-Za-z_]\w*)\s*;",
            execute,
        ):
            if match.group(2) in result and match.group(1) not in result:
                result.add(match.group(1))
                changed = True
    return result


def register_operands(source: EncodingSource) -> list[Operand]:
    variant = source.variant
    fields = {name: (low, width) for name, low, width in variant.fields}
    execute = source.execute_text
    result: list[Operand] = []
    for statement in source.decode:
        if statement[0] != "let":
            continue
        name, expression = statement[1], statement[2]
        segments = register_segments(expression, fields)
        if segments is None:
            continue
        names = _aliases(name, execute)
        uses: set[str] = set()
        for match in ACCESSOR_CALL.finditer(execute):
            argument = _first_argument(execute, match.end())
            if any(re.search(rf"\b{re.escape(n)}\b", argument) for n in names):
                uses.add(REGISTER_ACCESSORS[match.group(1)])
        if not uses:
            continue
        if len(uses) != 1:
            raise GenerationError(
                f"{source.file} {variant.encoding}: {name} is both "
                + " and ".join(sorted(uses))
            )
        kind = uses.pop()
        if kind == "gpr" and re.search(rf"\b{re.escape(name)}\s*==\s*31\b", execute) \
                and "SP{" in execute:
            kind = "gpr_or_sp"
        result.append(Operand(name, kind, segments))
    return result


# LLVM MC witnesses and the operand cross-check.

LLVM_REGISTER = re.compile(
    r"(?P<z>Z(?P<zn>\d+)(?P<zt>(?:_Z\d+)*))"
    r"|(?P<p>PN?(?P<pn>\d+)(?P<pt>(?:_PN?\d+)*))"
    r"|(?P<gpr>[XW](?P<gn>\d+))|(?P<zr>[XW]ZR)|(?P<sp>W?SP)"
    r"|(?P<v>[BHSDQ](?P<vn>\d+))"
    r"|(?P<tile>ZA[BHSDQ](?P<tn>\d+))"
)
FIXED_REGISTERS = {"ZA", "ZT0", "FFR", "NZCV"}


def llvm_register(value: str) -> tuple[str, int, int, int] | None:
    """(kind, first number, count, stride) of an LLVM register name."""
    if value in FIXED_REGISTERS:
        return None
    value = {"FP": "X29", "LR": "X30"}.get(value, value)
    match = LLVM_REGISTER.fullmatch(value)
    if match is None:
        raise GenerationError(f"unknown LLVM register {value!r}")
    if match.group("z"):
        numbers = [int(match.group("zn"))] + [
            int(n) for n in re.findall(r"_Z(\d+)", match.group("zt"))
        ]
        stride = (numbers[1] - numbers[0]) % 32 if len(numbers) > 1 else 1
        if any((b - a) % 32 != stride for a, b in zip(numbers, numbers[1:])):
            raise GenerationError(f"irregular LLVM register tuple {value!r}")
        return ("z", numbers[0], len(numbers), stride)
    if match.group("p"):
        numbers = [int(match.group("pn"))] + [
            int(n) for n in re.findall(r"_PN?(\d+)", match.group("pt"))
        ]
        stride = (numbers[1] - numbers[0]) % 16 if len(numbers) > 1 else 1
        return ("p", numbers[0], len(numbers), stride)
    if match.group("gpr"):
        return ("gpr", int(match.group("gn")), 1, 1)
    if match.group("zr"):
        return ("zr", 31, 1, 1)
    if match.group("sp"):
        return ("sp", 31, 1, 1)
    if match.group("v"):
        return ("v", int(match.group("vn")), 1, 1)
    return ("za_tile", int(match.group("tn")), 1, 1)


def _position(
    operand: Operand, register: tuple[str, int, int, int], word: int
) -> int | None:
    """The operand's position in the LLVM register (0 for the register or the
    first of a tuple), or None if the register is not the operand's."""
    kind, first, count, stride = register
    expected = operand.number(word)
    if operand.kind in ("z", "v", "p"):
        if operand.kind == "z" and kind != "z":
            return None
        if operand.kind == "v" and kind not in ("v", "z"):
            return None
        if operand.kind == "p" and kind != "p":
            return None
        modulus = 16 if kind == "p" else 32
        for position in range(count):
            if (first + position * stride) % modulus == expected:
                return position
        return None
    if operand.kind == "za_tile":
        return 0 if kind == "za_tile" and first == expected else None
    if operand.kind == "gpr":
        if (kind == "gpr" and first == expected) or (kind == "zr" and expected == 31):
            return 0
        return None
    if operand.kind == "gpr_or_sp":
        if (kind == "gpr" and first == expected) or (kind == "sp" and expected == 31):
            return 0
        return None
    raise GenerationError(f"unknown operand kind {operand.kind}")


def _compatible(operand: Operand, register, word: int) -> bool:
    return _position(operand, register, word) is not None


def _registers(decoded) -> list[tuple[str, int, int, int] | None]:
    return [
        llvm_register(value) if kind == "Reg" else None
        for kind, value in decoded[1]
    ]


def witness_candidates(row_mask: int, row_value: int) -> list[int]:
    free = ~row_mask & 0xFFFF_FFFF
    fills = [0, 0xFFFF_FFFF, 0x5555_5555, 0xAAAA_AAAA, 0x3333_3333, 0xCCCC_CCCC]
    state = 0x9E3779B9 ^ row_value
    for _ in range(10):
        state = (state * 6364136223846793005 + 1442695040888963407) % (1 << 64)
        fills.append(state >> 32)
    result = []
    for fill in fills:
        word = row_value | (fill & free)
        if word not in result:
            result.append(word)
    return result


def cross_check(row_name: str, operands: list[Operand], witness: int, base,
                flips: dict[int, object]) -> list[Operand]:
    """Check the ASL operands against LLVM's decoding of the witness and its
    one-bit neighbours, and return them with LLVM's group sizes. An operand
    that names a later member of another operand's register group is
    dropped: the group's first register implies it."""
    base_registers = _registers(base)
    usable = {}
    for bit, decoded in flips.items():
        if decoded is None or decoded[0] != base[0]:
            continue
        registers = _registers(decoded)
        if len(registers) != len(base_registers):
            raise GenerationError(f"{row_name}: operand count changes with bit {bit}")
        usable[bit] = registers
    owned: dict[str, set[int]] = {}
    for operand in operands:
        indices: set[int] = set()
        for bit, registers in usable.items():
            if not operand.bits() >> bit & 1:
                continue
            word = witness ^ (1 << bit)
            for index, (old, new) in enumerate(zip(base_registers, registers)):
                if old != new and new is not None and _compatible(operand, new, word):
                    indices.add(index)
        if not indices:
            indices = {
                index for index, register in enumerate(base_registers)
                if register is not None and _compatible(operand, register, witness)
            }
        if not indices:
            raise GenerationError(
                f"{row_name}: LLVM has no {operand.kind} operand "
                f"{operand.number(witness)} for {operand.name}"
            )
        owned[operand.name] = indices
    claimed = set().union(*owned.values()) if owned else set()
    for index, register in enumerate(base_registers):
        if register is None or index in claimed:
            continue
        if any(base_registers[other] == register for other in claimed):
            continue
        # A ZA tile the form fixes, such as LD1B's only byte tile ZAB0.
        if register[0] == "za_tile" and all(
                registers[index] == register for registers in usable.values()):
            continue
        raise GenerationError(
            f"{row_name}: LLVM register {base[1][index][1]} is no ASL operand"
        )
    for bit, registers in usable.items():
        word = witness ^ (1 << bit)
        owners = [o for o in operands if o.bits() >> bit & 1]
        changed = [
            index for index, (old, new) in enumerate(zip(base_registers, registers))
            if old != new
        ]
        for index in changed:
            new = registers[index]
            if new is not None and not any(
                    _compatible(owner, new, word) for owner in owners):
                raise GenerationError(
                    f"{row_name}: bit {bit} changes an LLVM register "
                    "that no ASL operand predicts"
                )
        for owner in owners:
            if not any(
                registers[index] is not None
                and _compatible(owner, registers[index], word)
                for index in changed
            ):
                raise GenerationError(
                    f"{row_name}: bit {bit} of {owner.name} changes no LLVM "
                    "register as predicted"
                )
    result = []
    for operand in operands:
        shapes = {
            (base_registers[i][2], base_registers[i][3],
             _position(operand, base_registers[i], witness))
            for i in owned[operand.name]
        }
        if len(shapes) != 1:
            raise GenerationError(
                f"{row_name}: {operand.name} has group shapes {sorted(shapes)}"
            )
        count, stride, position = shapes.pop()
        if position:
            continue
        result.append(Operand(operand.name, operand.kind, operand.segments, count, stride))
    return result


# Memory shapes. Streaming mode has no gather, scatter, first-fault or
# non-fault access, so every memory form of the profile is one of these. The
# page names the family; the pseudocode supplies the parameters, and the two
# must agree.
MEMORY_FAMILIES = (
    (re.compile(r"(ld|st)(nt)?1s?[bhwd]_z_p_(bi|br)"), "contiguous"),
    (re.compile(r"(ld|st)(nt)?1[bhwd]_mz_p_(bi|br)"), "contiguous"),
    (re.compile(r"(ld|st)(nt)?1[bhwd]_mzx_p_(bi|br)"), "strided"),
    (re.compile(r"(ld|st)[234][bhwd]_z_p_(bi|br)"), "structure"),
    (re.compile(r"ld1rs?[bhwd]_z_p_bi"), "replicate"),
    (re.compile(r"ld1rq[bhwd]_z_p_(bi|br)"), "replicate_quad"),
    (re.compile(r"(ld|st)r_z_bi"), "fill_z"),
    (re.compile(r"(ld|st)r_p_bi"), "fill_p"),
    (re.compile(r"(ld|st)1[bhwdq]_za_p_rrr"), "za_slice"),
    (re.compile(r"(ld|st)r_za_ri"), "za_vector"),
    (re.compile(r"(ld|st)r_zt_br"), "zt0"),
    (re.compile(r"prf[bhwd]_i_p_(bi|br)"), "prefetch"),
    (re.compile(r"(ld|st)1s?[bhwd]_z_p_(ai|bz)"), "gather"),
    (re.compile(r"(ld|st)nt1s?[bhwd]_z_p_ar"), "gather"),
    (re.compile(r"ldff1s?[bhwd]_z_p_(ai|bz)"), "gather_first_fault"),
    (re.compile(r"ldff1s?[bhwd]_z_p_br"), "first_fault"),
    (re.compile(r"ldnf1s?[bhwd]_z_p_bi"), "non_fault"),
    (re.compile(r"prf[bhwd]_i_p_(ai|bz)"), "gather_prefetch"),
)
# The families that address each element on its own (gathers, scatters and
# their prefetches), the first-fault and non-fault ones and the access
# descriptor each of those creates, and the families based on a vector of
# addresses (Zn) where their pages name one (_ai, _ar).
GATHER_FAMILIES = frozenset({"gather", "gather_first_fault", "gather_prefetch"})
FAULT_DESCRIPTORS = {
    "first_fault": "CreateAccDescSVEFF(",
    "gather_first_fault": "CreateAccDescSVEFF(",
    "non_fault": "CreateAccDescSVENF(",
}
PREFETCH_FAMILIES = frozenset({"prefetch", "gather_prefetch"})
# The byte offset each AddressAdd expression adds to its base, a scalar
# (base) or a vector element (baddr): an immediate times the access's
# footprint (MUL VL), a register or an immediate times the memory element
# size, an immediate times 16, a vector element extended and shifted, or a
# register unscaled.
ADDRESS_OFFSETS = {
    ("base", "offset * elements * mbytes"): "imm_footprint",
    ("base", "offset * elements * nreg * mbytes"): "imm_footprint",
    ("base", "offset * nreg * elements * mbytes"): "imm_footprint",
    ("base", "offset"): "imm_footprint",
    ("base", "moffs"): "imm_footprint",
    ("base", "UInt(offset) * mbytes"): "register_element",
    ("base", "UInt(moffs) * mbytes"): "register_element",
    ("base", "offset * mbytes"): "imm_element",
    ("base", "offset * 16"): "imm_16",
    ("base", "off << scale"): "vector_index",
    ("baddr", "offset * mbytes"): "imm_element",
    ("baddr", "offset"): "register_byte",
}
# The gather prefetches' addresses, which no AddressAdd forms.
PREFETCH_ADDRESSES = {
    "base + (off << scale)": "vector_index",
    "ZeroExtend{64}(base[e*:esize]) + (offset << scale)": "imm_element",
}
MEMORY_MARKERS = re.compile(r"\bMem\w*\{|AArch64_MemSingle|Hint_Prefetch|CreateAccDesc")


@dataclass(frozen=True)
class Memory:
    shape: str
    direction: str  # load, store, prefetch
    esize: int  # bits; with esize_field, the size at field value 0
    msize: int
    registers: int
    stride: int
    signed: bool
    nontemporal: bool
    # none, imm_footprint, register_element, imm_element, imm_16,
    # vector_index or register_byte
    offset: str
    immediate: tuple[Segment, ...] = ()
    immediate_signed: bool = False
    base: str = ""
    index: str = ""
    # The field whose value shifts esize left: ST1B's 8 << UInt(size).
    esize_field: Segment | None = None
    # The vector length that sizes the access: CurrentVL(), which is VL
    # outside streaming mode and SVL in it; CurrentSVL(); or none.
    vector_length: str = "current"
    # vector_index: the width of each offset element, its left shift, and
    # the field that sign-extends it (SXTW where 1), or None to zero-extend.
    index_bits: int = 0
    index_shift: int = 0
    index_sign: Segment | None = None


def _constant(statements: list, name: str) -> int | bool | None:
    for statement in statements:
        if statement[0] == "let" and statement[1] == name:
            node = statement[2]
            if node[0] in ("num", "bool"):
                return node[1]
            return None
    return None


def memory_shape(source: EncodingSource, operands: list[Operand]) -> Memory | None:
    execute = source.execute_text
    stem = source.file[:-4]
    family = next((f for pattern, f in MEMORY_FAMILIES if pattern.fullmatch(stem)), None)
    where = f"{source.file} {source.variant.encoding}"
    if family is None:
        if MEMORY_MARKERS.search(execute):
            raise GenerationError(f"{where}: memory form of no known family")
        return None
    if family in PREFETCH_FAMILIES:
        if "Hint_Prefetch" not in execute or "CreateAccDesc" in execute:
            raise GenerationError(f"{where}: prefetch without Hint_Prefetch")
        direction = "prefetch"
    elif family in FAULT_DESCRIPTORS:
        descriptors = set(re.findall(r"CreateAccDesc\w+\(", execute))
        if descriptors != {FAULT_DESCRIPTORS[family]} or not stem.startswith("ld"):
            raise GenerationError(f"{where}: access descriptors {sorted(descriptors)}")
        direction = "load"
    else:
        operations = set(re.findall(r"CreateAccDesc\w+\((MemOp_\w+)", execute))
        if operations == {"MemOp_LOAD"}:
            direction = "load"
        elif operations == {"MemOp_STORE"}:
            direction = "store"
        else:
            raise GenerationError(f"{where}: memory operations {sorted(operations)}")
        if stem.startswith("ld") != (direction == "load"):
            raise GenerationError(f"{where}: name and operation disagree")
    lengths = [name for name in ("CurrentVL()", "CurrentSVL()") if name in execute]
    if len(lengths) > 1:
        raise GenerationError(f"{where}: sized by both VL and SVL")
    vector_length = {"CurrentVL()": "current", "CurrentSVL()": "streaming"}[
        lengths[0]
    ] if lengths else "none"
    decode = source.decode
    esize = _constant(decode, "esize")
    esize_field = None
    for statement in decode:
        if statement[0] == "let" and statement[1] == "esize":
            node = statement[2]
            if node[0] == "binary" and node[1] == "<<" and node[2][0] == "num":
                segments = register_segments(
                    node[3], {n: (lo, w) for n, lo, w in source.variant.fields}
                )
                if segments is None or len(segments) != 1:
                    raise GenerationError(f"{where}: element size {node!r}")
                esize, esize_field = node[2][1], segments[0]
    msize = _constant(decode, "msize")
    nreg = _constant(decode, "nreg")
    tstride = _constant(decode, "tstride")
    unsigned = _constant(decode, "unsigned")
    scale = _constant(decode, "scale")
    if family in ("fill_z", "fill_p", "za_vector", "zt0"):
        esize = msize = 8
    if family == "gather_prefetch":
        # A prefetch moves no data; its "element" is what its offsets scale
        # by, PRFH's halfword.
        if msize is not None or scale is None:
            raise GenerationError(f"{where}: gather prefetch scale {scale}")
        msize = 8 << scale
    if esize is None:
        raise GenerationError(f"{where}: no constant element size")
    if msize is None:
        msize = esize
    registers = nreg if nreg is not None else 1
    if family == "strided":
        if tstride is None:
            raise GenerationError(f"{where}: strided form without tstride")
        stride = tstride
    else:
        stride = 1
    contiguous = re.findall(r"\bcontiguous : boolean = (TRUE|FALSE)", execute)
    if family not in PREFETCH_FAMILIES and contiguous != (
        ["FALSE"] if family in GATHER_FAMILIES else ["TRUE"]
    ):
        raise GenerationError(f"{where}: contiguous {contiguous}")
    nontemporal = re.findall(r"\bnontemporal : boolean = (TRUE|FALSE)", execute)
    if family in FAULT_DESCRIPTORS:
        if nontemporal:
            raise GenerationError(f"{where}: a non-temporal fault-tolerant load")
    elif family not in PREFETCH_FAMILIES and len(nontemporal) != 1:
        raise GenerationError(f"{where}: no single nontemporal setting")
    offsets = re.findall(r"AddressAdd\((base|baddr),\s*([^,]+),", execute)
    if family == "prefetch":
        offset = "imm_footprint" if stem.endswith("_bi") else "register_element"
        if stem.endswith("_bi") and "(offset * elements) + e" not in execute:
            raise GenerationError(f"{where}: unexpected prefetch address")
        if stem.endswith("_br") and "UInt(offset) + e" not in execute:
            raise GenerationError(f"{where}: unexpected prefetch address")
    elif family == "gather_prefetch":
        kinds = {kind for text, kind in PREFETCH_ADDRESSES.items() if text in execute}
        if offsets or len(kinds) != 1:
            raise GenerationError(f"{where}: unexpected prefetch address")
        offset = kinds.pop()
    elif family == "zt0":
        if offsets:
            raise GenerationError(f"{where}: ZT0 access with an offset")
        offset = "none"
    else:
        kinds = {ADDRESS_OFFSETS.get((b, " ".join(o.split()))) for b, o in offsets}
        if len(kinds) != 1 or None in kinds:
            raise GenerationError(f"{where}: address offsets {offsets}")
        offset = kinds.pop()
    by_name = {operand.name: operand for operand in operands}
    base = by_name.get("n")
    vector_base = family in GATHER_FAMILIES and stem.endswith(("_ai", "_ar"))
    if base is None or base.kind != ("z" if vector_base else "gpr_or_sp"):
        raise GenerationError(f"{where}: base register is not "
                              f"{'Zn' if vector_base else 'Xn|SP'}")
    index = ""
    index_kind = {"register_element": "gpr", "register_byte": "gpr",
                  "vector_index": "z"}.get(offset)
    if index_kind is not None:
        if "m" not in by_name or by_name["m"].kind != index_kind:
            raise GenerationError(f"{where}: index register is not {index_kind}")
        index = "m"
    index_bits = index_shift = 0
    index_sign = None
    if offset == "vector_index":
        index_bits = _constant(decode, "offs_size")
        if index_bits not in (32, 64) or scale is None:
            raise GenerationError(f"{where}: vector offsets {index_bits} << {scale}")
        index_shift = scale
        fields = {name: (low, width) for name, low, width in source.variant.fields}
        signedness = next(
            (st[2] for st in decode if st[0] == "let" and st[1] == "offs_unsigned"), None
        )
        if signedness == ("bool", True):
            index_sign = None
        elif (
            signedness is not None and signedness[0] == "binary"
            and signedness[1] == "==" and signedness[2][0] == "name"
            and signedness[3][0] == "bits" and signedness[3][1].width == 1
            and signedness[3][1].value == 0 and signedness[2][1] in fields
        ):
            low, width = fields[signedness[2][1]]
            if width != 1:
                raise GenerationError(f"{where}: offset extension field width")
            index_sign = Segment(signedness[2][1], low, width)
        else:
            raise GenerationError(f"{where}: offset extension {signedness!r}")
        if index_bits == 64 and index_sign is not None:
            raise GenerationError(f"{where}: a 64-bit offset with an extension")
    immediate: tuple[Segment, ...] = ()
    immediate_signed = False
    if offset.startswith("imm"):
        fields = {name: (low, width) for name, low, width in source.variant.fields}
        source_name = {"fill_z": "imm", "fill_p": "imm"}.get(family, "offset")
        for statement in decode:
            if statement[0] == "let" and statement[1] == source_name:
                node = statement[2]
                if node[0] == "call" and node[1] in ("SInt", "UInt"):
                    segments = register_segments(("call", "UInt", node[2], []), fields)
                    if segments is not None:
                        immediate = segments
                        immediate_signed = node[1] == "SInt"
        if not immediate:
            raise GenerationError(f"{where}: no immediate offset field")
    return Memory(
        shape=family,
        direction=direction,
        esize=esize,
        msize=msize,
        registers=registers,
        stride=stride,
        signed=unsigned is False,
        nontemporal=nontemporal == ["TRUE"],
        offset=offset,
        immediate=immediate,
        immediate_signed=immediate_signed,
        base="n",
        index=index,
        esize_field=esize_field,
        vector_length=vector_length,
        index_bits=index_bits,
        index_shift=index_shift,
        index_sign=index_sign,
    )


# The FP&SIMD companion: which encodings trap in streaming mode.

# Features a check selection may name, all of which the virtual CPU omits:
# FEAT_FPRCVT moves the scalar FCVT*, SCVTF and UCVTF forms to FP.
CLASS_ABSENT_FEATURES = frozenset({"FEAT_FPRCVT"})
FP_CHECKS = {"AArch64_CheckFPEnabled", "AArch64_CheckFPAdvSIMDEnabled", "CheckFPMREnabled"}


@dataclass(frozen=True)
class ClassRow:
    source: EncodingSource
    mask: int
    value: int
    advsimd: bool


def _streaming_class(source: EncodingSource, checks: tuple[str, ...], where: str) -> bool:
    if not checks or not set(checks) <= FP_CHECKS:
        raise GenerationError(f"{where}: checks {checks}")
    if "AArch64_CheckFPAdvSIMDEnabled" in checks:
        return True
    if "AArch64_CheckFPEnabled" in checks:
        return False
    raise GenerationError(f"{where}: no FP or AdvSIMD check")


def class_rows(source: EncodingSource) -> list[ClassRow]:
    """Rows classifying the words of an FP&SIMD encoding that its decode
    accepts as AdvSIMD or FP."""
    variant = source.variant
    where = f"{source.file} {variant.encoding}"
    prefix_text = repr(source.checks_prefix)
    named = _feature_names(prefix_text)
    if not named <= CLASS_ABSENT_FEATURES:
        raise GenerationError(f"{where}: check selection names {sorted(named)}")
    field_names = {name for name, _low, _width in variant.fields}
    bindings: dict[str, list] = {}
    _statement_bindings(source.decode, bindings)
    names = _decision_names(source.decode)
    for statement in source.checks_prefix:
        names |= _decision_names([statement]) | _prefix_names(statement)
    relevant = _closure(names, bindings, field_names)
    positions = _free_bits(source, relevant)
    if len(positions) > 14:
        raise GenerationError(f"{where}: {len(positions)} deciding bits")
    features = frozenset(
        _feature_names(source.decode_text + prefix_text)
    ) - CLASS_ABSENT_FEATURES | BASE_FEATURES
    groups: dict[bool, set[int]] = {}
    for assignment in range(1 << len(positions)):
        word = variant.value
        for index, bit in enumerate(positions):
            if assignment >> index & 1:
                word |= 1 << bit
        if not source.matches(word):
            continue
        # A CONSTRAINED UNPREDICTABLE choice (LDTNP with Rt == Rt2) leaves
        # the word defined: it is still classified.
        environment = asl.Environment(
            fields=field_values(variant, word, relevant),
            features=features,
            unpredictable="Constraint_UNKNOWN",
        )
        evaluator = CheckRecorder(environment)
        try:
            evaluator.run(source.decode)
        except asl.Undefined:
            continue
        except asl.AslError as error:
            raise GenerationError(f"{where} {word:08x}: {error}") from error
        try:
            evaluator.run(source.checks_prefix)
        except asl.AslError as error:
            raise GenerationError(f"{where} {word:08x}: {error}") from error
        advsimd = _streaming_class(source, tuple(evaluator.checks), f"{where} {word:08x}")
        groups.setdefault(advsimd, set()).add(word)
    rows = []
    for advsimd, words in sorted(groups.items(), key=lambda item: min(item[1])):
        for mask, value in cubes(words, positions, variant.value):
            rows.append(
                ClassRow(source, variant.mask | mask, variant.value | value, advsimd)
            )
    return rows


# The decode partitions, which the allocation route derives from the XML's
# encoding index rather than from the instruction pages.


@dataclass(frozen=True)
class Partition:
    kind: str
    name: str
    related: tuple[str, ...]
    mask: int
    value: int
    constraints: tuple[tuple[int, int, bool, str], ...]

    def matches(self, word: int) -> bool:
        if word & self.mask != self.value:
            return False
        for low, width, equal, pattern in self.constraints:
            actual = f"{(word >> low) & ((1 << width) - 1):0{width}b}"
            same = all(a == "x" or a == b for a, b in zip(pattern, actual))
            if same != equal:
                return False
        return True


def read_partitions(path: Path) -> list[Partition]:
    result = []
    with path.open(encoding="utf-8", newline="") as source:
        for row in csv.DictReader(source, delimiter="\t"):
            if row["decode_layer"] != "instruction-table":
                continue
            constraints = []
            if row["architecture_constraints"] != "none":
                for item in row["architecture_constraints"].split(";"):
                    _name, _at, rest = item.partition("@")
                    operator = "!=" if "!=" in rest else "=="
                    location, _op, pattern = rest.partition(operator)
                    low, width = (int(part) for part in location.split(":"))
                    if len(pattern) != width:
                        raise GenerationError(f"{path}: constraint {item}")
                    constraints.append((low, width, operator == "==", pattern))
            result.append(
                Partition(
                    kind=row["partition_kind"],
                    name=row["partition_name"],
                    related=tuple(
                        () if row["related_stable_ids"] == "none"
                        else row["related_stable_ids"].split("+")
                    ),
                    mask=int(row["architecture_mask"], 16),
                    value=int(row["architecture_value"], 16),
                    constraints=tuple(constraints),
                )
            )
    if not result:
        raise GenerationError(f"{path}: no instruction-table partitions")
    return result


def check_partition(
    partitions: list[Partition], source: EncodingSource, witness: int
) -> None:
    """The witness must fall in the allocated partition of its own encoding
    and form, and in no unallocated partition."""
    where = f"{source.file} {source.variant.encoding} {witness:08x}"
    matching = [p for p in partitions if p.matches(witness)]
    if any(p.kind != "allocated" for p in matching):
        raise GenerationError(f"{where}: in an unallocated decode partition")
    if not any(
        p.name == source.variant.encoding and source.stable_id in p.related
        for p in matching
    ):
        raise GenerationError(f"{where}: not in its encoding's decode partition")


# The table.

REPOSITORY = Path(__file__).resolve().parents[3]
DEFAULT_OUTPUT = REPOSITORY / "darwin/cpu/arm64/detail/sme_encodings.hpp"
DEFAULT_PARTITIONS = REPOSITORY / "tests/unit/arm64/a64_instruction_decode_partitions.tsv"
FEATURE_BITS = {
    token: f"sme_feature_{token}"
    for token in {**PROFILE_FEATURES, **WITHHELD_FEATURES}.values()
}
CHECK_BITS = {
    "CheckSVEEnabled": "sme_check_sve_enabled",
    "CheckStreamingSVEEnabled": "sme_check_streaming_sve_enabled",
    "CheckNonStreamingSVEEnabled": "sme_check_non_streaming_sve_enabled",
    "CheckStreamingSVEAndZAEnabled": "sme_check_streaming_sve_and_za_enabled",
    "CheckSMEAndZAEnabled": "sme_check_sme_and_za_enabled",
    "CheckSMEZT0Enabled": "sme_check_sme_zt0_enabled",
    "CheckSMEEnabled": "sme_check_sme_enabled",
}
OFFSET_NAMES = {
    "none": "none",
    "imm_footprint": "immediate_footprint",
    "register_element": "register_element",
    "imm_element": "immediate_element",
    "imm_16": "immediate_16",
    "vector_index": "vector_index",
    "register_byte": "register_byte",
}
VECTOR_LENGTH_NAMES = {"current": "current", "streaming": "streaming", "none": "none"}
BUCKET_SHIFT = 21


@dataclass(frozen=True)
class TableRow:
    row: Row
    witness: int
    operands: tuple[Operand, ...]
    memory: Memory | None


def in_sve_or_sme_space(mask: int, value: int) -> bool:
    """Whether every word of a row is in the SVE encoding space (bits 28:25
    = 0010) or the SME space (bit 31 set, bits 28:25 = 0000)."""
    sve = mask & 0x1E00_0000 == 0x1E00_0000 and value & 0x1E00_0000 == 0x0400_0000
    sme = mask & 0x9E00_0000 == 0x9E00_0000 and value & 0x9E00_0000 == 0x8000_0000
    return sve or sme


def choose_witnesses(tool: Path, runner, masks_values: list[tuple[int, int]], what: str):
    candidates = [witness_candidates(mask, value) for mask, value in masks_values]
    decoded = decoder_instructions(tool, [w for c in candidates for w in c], runner=runner)
    position = 0
    result = []
    for (mask, value), words in zip(masks_values, candidates):
        found = None
        for word in words:
            if found is None and decoded[position] is not None:
                found = (word, decoded[position])
            position += 1
        if found is None:
            raise GenerationError(
                f"{what} {mask:08x}/{value:08x}: LLVM MC decodes no witness"
            )
        result.append(found)
    return result


def build_table(
    catalog: ArchitectureCatalog, tool: Path, runner, partitions: list[Partition]
):
    sources = load_sources(catalog, ("SVE", "SME"))
    rows: list[Row] = []
    for source in sources:
        rows += sorted(encoding_rows(source), key=lambda r: (r.value, r.mask))
    for row in rows:
        where = (
            f"{row.source.file} {row.source.variant.encoding} "
            f"{row.mask:08x}/{row.value:08x}"
        )
        if row.outcome.kind not in ("legal", "absent"):
            raise GenerationError(f"{where}: outcome {row.outcome.kind}")
        if row.outcome.kind == "legal" and (
            len(row.outcome.requirement) > 1 or len(row.outcome.non_streaming) > 1
        ):
            raise GenerationError(
                f"{where}: requirements {row.outcome.requirement} and "
                f"{row.outcome.non_streaming}"
            )
        if not in_sve_or_sme_space(row.mask, row.value):
            raise GenerationError(f"{where}: outside the SVE and SME spaces")
    check_disjoint(rows)
    witnesses = choose_witnesses(tool, runner, [(r.mask, r.value) for r in rows], "row")
    legal = [index for index, row in enumerate(rows) if row.outcome.kind == "legal"]
    flip_words = []
    flip_keys = []
    for index in legal:
        word = witnesses[index][0]
        for bit in range(32):
            if not rows[index].mask >> bit & 1:
                flip_words.append(word ^ (1 << bit))
                flip_keys.append((index, bit))
    flips: dict[int, dict[int, object]] = {index: {} for index in legal}
    for (index, bit), decoded in zip(
        flip_keys, decoder_instructions(tool, flip_words, runner=runner)
    ):
        flips[index][bit] = decoded
    result = []
    for index, row in enumerate(rows):
        word, decoded = witnesses[index]
        check_partition(partitions, row.source, word)
        operands: tuple[Operand, ...] = ()
        memory = None
        if row.outcome.kind == "legal":
            name = (
                f"{row.source.file} {row.source.variant.encoding} "
                f"{word:08x} {decoded[0]}"
            )
            operands = tuple(
                cross_check(
                    name, register_operands(row.source), word, decoded, flips[index]
                )
            )
            memory = memory_shape(row.source, list(operands))
        result.append(TableRow(row, word, operands, memory))
    class_sources = load_sources(catalog, ("FP&SIMD",))
    classes: list[ClassRow] = []
    for source in class_sources:
        classes += sorted(class_rows(source), key=lambda r: (r.value, r.mask))
    check_disjoint(classes)
    class_witnesses = choose_witnesses(
        tool, runner, [(r.mask, r.value) for r in classes], "FP&SIMD row"
    )
    for row, (word, _decoded) in zip(classes, class_witnesses):
        check_partition(partitions, row.source, word)
    return result, [(row, word) for row, (word, _decoded) in zip(classes, class_witnesses)]


def _hex(value: int) -> str:
    text = f"{value:08x}"
    return f"0x{text[:4]}'{text[4:]}U"


def _segment(segment: Segment | None) -> str:
    if segment is None:
        return "{}"
    if segment.field is None:
        return f"sme_constant({segment.width}, {segment.constant})"
    return f"sme_field({segment.low}, {segment.width})"


def _segments(segments: tuple[Segment, ...], size: int) -> str:
    if len(segments) > size:
        raise GenerationError(f"{len(segments)} segments exceed {size}")
    return "{{" + ", ".join(_segment(s) for s in segments) + "}}"


def _operand(operand: Operand) -> str:
    return (
        f"SmeOperand{{SmeOperandKind::{operand.kind}, {operand.count}, "
        f"{operand.stride}, {len(operand.segments)}, "
        f"{_segments(operand.segments, 4)}}}"
    )


def _memory(memory: Memory, operands: tuple[Operand, ...]) -> str:
    names = [operand.name for operand in operands]
    base = names.index(memory.base)
    index = names.index(memory.index) if memory.index else None
    flag = lambda value: "true" if value else "false"  # noqa: E731
    return (
        "SmeMemory{"
        f"SmeMemoryShape::{memory.shape}, SmeMemoryDirection::{memory.direction}, "
        f"SmeAddressOffset::{OFFSET_NAMES[memory.offset]}, {memory.esize}, "
        f"{_segment(memory.esize_field)}, {memory.msize}, {memory.registers}, "
        f"{memory.stride}, {flag(memory.signed)}, {flag(memory.nontemporal)}, "
        f"{flag(memory.immediate_signed)}, {len(memory.immediate)}, "
        f"{_segments(memory.immediate, 2)}, {base}, "
        f"{'sme_no_operand' if index is None else index}, "
        f"SmeVectorLength::{VECTOR_LENGTH_NAMES[memory.vector_length]}, "
        f"{memory.index_bits}, {memory.index_shift}, {_segment(memory.index_sign)}}}"
    )


def _form(stable_id: str) -> int:
    section, number = stable_id.split("-")
    ordinal = int(number)
    if section == "SVE":
        return ordinal
    if section == "SME":
        return 0x8000 | ordinal
    if section == "FPSIMD":
        return ordinal
    raise GenerationError(f"unexpected stable id {stable_id}")


def _bits(names: list[str]) -> str:
    return " | ".join(names) if names else "0"


def _buckets(masks_values: list[tuple[int, int]]) -> tuple[list[int], list[int]]:
    """Start offsets per word >> BUCKET_SHIFT, and the rows of each bucket."""
    width = 32 - BUCKET_SHIFT
    members: list[list[int]] = [[] for _ in range(1 << width)]
    for index, (mask, value) in enumerate(masks_values):
        fixed = mask >> BUCKET_SHIFT
        base = value >> BUCKET_SHIFT
        free = [bit for bit in range(width) if not fixed >> bit & 1]
        for assignment in range(1 << len(free)):
            key = base
            for position, bit in enumerate(free):
                if assignment >> position & 1:
                    key |= 1 << bit
            members[key].append(index)
    starts = [0]
    flat: list[int] = []
    for bucket in members:
        flat += bucket
        starts.append(len(flat))
    if len(flat) > 0xFFFF:
        raise GenerationError("bucket index exceeds 16 bits")
    return starts, flat


def _numbers(values: list[int], per_line: int, formatter=str) -> list[str]:
    return [
        "\t" + " ".join(f"{formatter(v)}," for v in values[start:start + per_line])
        for start in range(0, len(values), per_line)
    ]


def render(rows: list[TableRow], classes: list[tuple[ClassRow, int]], llvm: str) -> str:
    operand_list: list[Operand] = []
    sequences: dict[tuple[Operand, ...], int] = {}
    memory_list: list[str] = []
    memories: dict[str, int] = {}
    encoding_lines = []
    witnesses = []
    for table_row in rows:
        row = table_row.row
        source = row.source
        operands = table_row.operands
        if operands and operands not in sequences:
            sequences[operands] = len(operand_list)
            operand_list += operands
        begin = sequences.get(operands, 0)
        memory = 0
        if table_row.memory is not None:
            text = _memory(table_row.memory, operands)
            if text not in memories:
                memories[text] = len(memory_list)
                memory_list.append(text)
            memory = memories[text] + 1
        outcome = row.outcome
        requirement = []
        non_streaming = []
        checks = []
        mode = []
        if outcome.kind == "legal":
            if outcome.requirement:
                requirement = [FEATURE_BITS[f] for f in outcome.requirement[0]]
            if outcome.non_streaming:
                non_streaming = [FEATURE_BITS[f] for f in outcome.non_streaming[0]]
            checks = [CHECK_BITS[c] for c in CHECK_BITS if c in outcome.checks]
            demands = set().union(*(CHECK_DEMANDS[c] for c in outcome.checks))
            if "sm" in demands:
                mode.append("sme_mode_streaming")
            if "za" in demands:
                mode.append("sme_mode_za")
            if "CheckSMEZT0Enabled" in outcome.checks:
                mode.append("sme_mode_zt0")
        encoding_lines.append(
            f"\tSmeEncoding{{{_hex(row.mask)}, {_hex(row.value)}, "
            f"{_form(source.stable_id)}U, {_bits(requirement)}, "
            f"{_bits(non_streaming)}, {_bits(checks)}, "
            f"{_bits(mode)}, {len(operands)}, {begin}, {memory}, "
            f"{outcome.min_svl // 8}}}, // {source.stable_id} {source.variant.encoding}"
        )
        witnesses.append(table_row.witness)
    if len(operand_list) > 0xFFFF or len(memory_list) >= 0xFFFF:
        raise GenerationError("side tables exceed 16-bit indexes")
    starts, flat = _buckets([(r.row.mask, r.row.value) for r in rows])
    class_starts, class_flat = _buckets([(r.mask, r.value) for r, _w in classes])
    streaming = sum(bool(r.row.outcome.requirement) for r in rows)
    non_streaming = sum(bool(r.row.outcome.non_streaming) for r in rows)
    class_lines = [
        f"\tFpSimdStreamingClass{{{_hex(row.mask)}, {_hex(row.value)}, "
        f"{_form(row.source.stable_id)}U, {'true' if row.advsimd else 'false'}}}, "
        f"// {row.source.stable_id} {row.source.variant.encoding}"
        for row, _word in classes
    ]
    lines = [
        "// SPDX-License-Identifier: GPL-2.0-or-later",
        "// Generated by tests/tooling/arm64/generate_sme_encoding_table.py from",
        f"// {SOURCE_ORACLE} and {llvm}. Do not edit.",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        '#include "sme_encoding_types.hpp"',
        "",
        "namespace uml::cpu::arm64::detail {",
        "",
        "/*",
        f" * The {len(rows)} rows of the SVE and SME encodings, {streaming} of them in the",
        f" * virtual CPU's profile in streaming mode and {non_streaming} outside it, and",
        " * their side tables. See sme_encoding_types.hpp.",
        " */",
    ]
    lines += _array("SmeOperand", "sme_encoding_operands",
                    [f"\t{_operand(o)}," for o in operand_list])
    lines += _array("SmeMemory", "sme_encoding_memory", [f"\t{m}," for m in memory_list])
    lines += _array("SmeEncoding", "sme_encodings", encoding_lines)
    lines.append("/* For each row, a word LLVM MC decodes to the row's encoding. */")
    lines += _array("std::uint32_t", "sme_encoding_witnesses", _numbers(witnesses, 6, _hex),
                    len(witnesses))
    lines += [
        f"/* The rows whose words can have each value of bits 31:{BUCKET_SHIFT}. */",
        f"inline constexpr unsigned sme_encoding_bucket_shift = {BUCKET_SHIFT};",
    ]
    lines += _array("std::uint16_t", "sme_encoding_buckets", _numbers(starts, 12),
                    len(starts))
    lines += _array("std::uint16_t", "sme_encoding_bucket_rows", _numbers(flat, 12),
                    len(flat))
    lines += [
        "/*",
        f" * The {len(classes)} rows of the FP&SIMD encodings: AdvSIMD, which traps in",
        " * streaming mode, or FP. A word with no row is one the decode rejects; its",
        " * caller must already have rejected it. The buckets use the same shift.",
        " */",
    ]
    lines += _array("FpSimdStreamingClass", "fp_simd_streaming_classes", class_lines)
    lines += _array("std::uint32_t", "fp_simd_streaming_witnesses",
                    _numbers([w for _r, w in classes], 6, _hex), len(classes))
    lines += _array("std::uint16_t", "fp_simd_streaming_buckets",
                    _numbers(class_starts, 12), len(class_starts))
    lines += _array("std::uint16_t", "fp_simd_streaming_bucket_rows",
                    _numbers(class_flat, 12), len(class_flat))
    lines += ["} // namespace uml::cpu::arm64::detail", ""]
    return "\n".join(lines)


def _array(
    element: str, name: str, items: list[str], count: int | None = None
) -> list[str]:
    size = len(items) if count is None else count
    return [
        f"inline constexpr std::array<{element}, {size}> {name}{{{{",
        *items,
        "}};",
        "",
    ]


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--architecture-archive", type=Path, required=True)
    parser.add_argument("--partitions", type=Path, default=DEFAULT_PARTITIONS)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--llvm-mc", type=Path)
    parser.add_argument("--timeout-seconds", type=int, default=120)
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def main() -> int:
    options = arguments()
    tool = find_llvm_mc(options.llvm_mc).resolve(strict=True)
    publication = OneShotPublication.prepare(
        [options.output], [options.architecture_archive, options.partitions, tool]
    )
    with workspace_from_options(
        options,
        source_root=REPOSITORY,
        publication_targets=publication.targets,
        prefix="a64-sme-",
    ) as workspace:
        runner = BoundedCommandRunner(workspace, options.timeout_seconds)
        llvm = require_pinned_llvm(tool, runner.run)
        catalog = ArchitectureCatalog(
            archive_root(
                options.architecture_archive,
                workspace.require_path() / "archive",
                workspace,
            )
        )
        rows, classes = build_table(
            catalog, tool, runner.run, read_partitions(options.partitions)
        )
    payload = render(rows, classes, llvm).encode("utf-8")

    def validate(observed: bytes) -> None:
        if observed != payload:
            raise ValueError("published table differs from the rendered table")

    publication.publish_payloads([(publication.targets[0], payload, validate)])
    streaming = sum(bool(row.row.outcome.requirement) for row in rows)
    non_streaming = sum(bool(row.row.outcome.non_streaming) for row in rows)
    print(
        f"SVE/SME encoding table generated: {len(rows)} rows ({streaming} in the "
        f"profile in streaming mode, {non_streaming} outside it), {len(classes)} "
        f"FP&SIMD rows, {SOURCE_ORACLE}, {llvm}; wrote {options.output}"
    )
    print(sve_index_modes(rows))
    return 0


def sve_index_modes(rows: list[TableRow]) -> str:
    """The SVE index's encodings by the modes in which the profile executes
    them."""
    modes: dict[tuple[str, str], set[tuple[bool, bool]]] = {}
    sources: dict[tuple[str, str], EncodingSource] = {}
    for table_row in rows:
        row = table_row.row
        if row.source.section != "SVE" or row.outcome.kind != "legal":
            continue
        key = (row.source.file, row.source.variant.encoding)
        sources[key] = row.source
        modes.setdefault(key, set()).add(
            (bool(row.outcome.requirement), bool(row.outcome.non_streaming))
        )
    both = [key for key, seen in modes.items() if seen == {(True, True)}]
    outside = [key for key, seen in modes.items() if seen == {(False, True)}]
    inside = [key for key, seen in modes.items() if seen == {(True, False)}]
    mixed = len(modes) - len(both) - len(outside) - len(inside)
    split = sum(
        "CheckNonStreamingSVEEnabled" in sources[key].check_names for key in both
    )
    with_sme = sum(
        any("sme" in r.row.outcome.non_streaming[0] for r in rows
            if (r.row.source.file, r.row.source.variant.encoding) == key
            and r.row.outcome.non_streaming)
        for key in both
    )
    return (
        f"SVE index encodings: {len(both)} in either mode ({split} with a "
        f"field-dependent check, {with_sme} only with FEAT_SME too), "
        f"{len(outside)} only outside streaming mode, {len(inside)} only in "
        f"it, {mixed} mixed; {len(both) + len(outside)} execute outside "
        "streaming mode"
    )


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, tarfile.TarError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
