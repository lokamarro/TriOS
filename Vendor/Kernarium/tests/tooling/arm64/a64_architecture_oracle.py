#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Arm A64 XML form/allocation access for exact instruction evidence.

This module deliberately reads Arm's machine-readable release rather than
deriving architectural forms from the local inventory or execution bindings.
Only information present in the XML is exposed here; semantic result contracts
remain a separate, explicitly incomplete review artifact.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass, replace
from pathlib import Path


RELEASE = "2026-06"
COMMIT_ID = "2026-06_rel"
ARCHIVE_NAME = f"ISA_A64_xml_A_profile-{RELEASE}.tar.gz"
ARCHIVE_SHA256 = "63a01a1696483bbe2edfef9e0f0cd053d6c1c619ec0587876cb7a60bb344f354"
SOURCE_ORACLE = f"arm-a-profile-{RELEASE}-xml-{COMMIT_ID}"
INDEXES = {
    "Base": ("index.xml", 516),
    "FP&SIMD": ("fpsimdindex.xml", 467),
    "SVE": ("sveindex.xml", 955),
    "SME": ("mortlachindex.xml", 353),
}
STABLE_ID_PREFIXES = {
    "Base": "BASE",
    "FP&SIMD": "FPSIMD",
    "SVE": "SVE",
    "SME": "SME",
}
# Index headings the pinned XML repeats within one index, as
# (section, file): (heading, label). The SVE index and the PDF title two
# alias pages just "MOV"; each label names the operand its index
# description gives, as the sibling MOV headings of each section do.
HEADING_ERRATA = {
    ("SVE", "mov_dupm_z_i.xml"): ("MOV", "MOV (bitmask immediate)"),
    ("SVE", "mov_orr_p_p_pp.xml"): ("MOV", "MOV (predicate, unpredicated)"),
}
SIMPLE_CONSTRAINT = re.compile(r"(!=|==) ([01x]+)\Z")
# A feature expression is FEAT_ names and field comparisons joined by &&
# and ||, with parentheses: "FEAT_SME2 && (sz == '0' || FEAT_SME_I16I64)".
FEATURE_TOKEN = re.compile(
    r"\s*(FEAT_[A-Za-z0-9_]+|[A-Za-z][A-Za-z0-9_]* (?:==|!=) '[01x]+'"
    r"|&&|\|\||\(|\))"
)
UNDEF_GUARD = re.compile(r"if (.+) then EndOfDecode\(Decode_UNDEF\); end;\Z")
GUARD_FEATURE = re.compile(r"!IsFeatureImplemented\((FEAT_[A-Za-z0-9_]+)\)\Z")
GUARD_FIELD = re.compile(r"([A-Za-z][A-Za-z0-9_]*) (==|!=) '([01x]+)'\Z")
# Decode statements the pinned XML transcribes wrongly, as (wrong, right).
# FCMLA (by element) guards FEAT_FP16 on size == '10', the single-precision
# arrangement by the file's own <Ts> table. DDI 0487G.a C7.2.62 decodes it
# as "esize = 8 << UInt(size); if !HaveFP16Ext() && esize == 16 then
# UNDEFINED;", which is size == '01', the half-precision arrangement, as in
# FCMLA (vector) and FCADD.
DECODE_ERRATA = {
    "fcmla_advsimd_elt.xml": (
        "if !IsFeatureImplemented(FEAT_FP16) && size == '10' then "
        "EndOfDecode(Decode_UNDEF); end;",
        "if !IsFeatureImplemented(FEAT_FP16) && size == '01' then "
        "EndOfDecode(Decode_UNDEF); end;",
    ),
}


@dataclass(frozen=True)
class FieldConstraint:
    name: str
    low: int
    width: int
    expression: str

    def matches(self, word: int) -> bool:
        match = SIMPLE_CONSTRAINT.fullmatch(self.expression)
        if match is None or len(match.group(2)) != self.width:
            raise ValueError(
                f"unsupported Arm XML constraint {self.name} "
                f"{self.expression!r}"
            )
        actual = f"{(word >> self.low) & ((1 << self.width) - 1):0{self.width}b}"
        equal = all(
            expected == "x" or expected == observed
            for expected, observed in zip(match.group(2), actual)
        )
        return equal if match.group(1) == "==" else not equal

    def text(self) -> str:
        return (
            f"{self.name}@{self.low}:{self.width}"
            f"{self.expression.replace(' ', '')}"
        )


@dataclass(frozen=True)
class ArchitectureVariant:
    section: str
    form: str
    file: str
    encoding: str
    mask: int
    value: int
    constraints: tuple[FieldConstraint, ...]
    feature_expressions: tuple[str, ...]
    fields: tuple[tuple[str, int, int], ...]
    feature_guards: tuple[tuple[str, tuple[FieldConstraint, ...]], ...]

    def matches(self, word: int) -> bool:
        return word & self.mask == self.value and all(
            constraint.matches(word) for constraint in self.constraints
        )

    def guarded_features(self, word: int) -> list[str]:
        """Return the decode-guard features this word's field values select."""
        return [
            feature
            for feature, conditions in self.feature_guards
            if all(condition.matches(word) for condition in conditions)
        ]


@dataclass(frozen=True)
class DecodePartition:
    """One exact predicate from the official encoding-index decode tables."""

    layer: str
    kind: str
    decode_class: str
    name: str
    file: str
    mask: int
    value: int
    constraints: tuple[FieldConstraint, ...]
    feature_expressions: tuple[str, ...]


def _box_bits(box: ET.Element) -> list[tuple[int, str]]:
    width = int(box.get("width", "1"))
    cells: list[str] = []
    for cell in box.findall("c"):
        cells.extend(
            [(cell.text or "").strip()] * int(cell.get("colspan", "1"))
        )
    if len(cells) != width:
        raise ValueError(
            f"Arm XML box at bit {box.get('hibit')} declares width {width} "
            f"but contains {len(cells)} bits"
        )
    high = int(box.get("hibit", "-1"))
    if high < 0 or high - width + 1 < 0 or high > 31:
        raise ValueError("Arm XML contains an out-of-range encoding box")
    return [(high - index, setting) for index, setting in enumerate(cells)]


def _add_boxes(
    mask: int, value: int, boxes: list[ET.Element]
) -> tuple[int, int, dict[str, tuple[int, int]], list[FieldConstraint]]:
    fields: dict[str, tuple[int, int]] = {}
    constraints: list[FieldConstraint] = []
    for box in boxes:
        high = int(box.get("hibit", "-1"))
        width = int(box.get("width", "1"))
        name = box.get("name", "")
        if name:
            fields[name] = (high - width + 1, width)
        expression = box.get("constraint")
        if expression is not None:
            if not name:
                raise ValueError("Arm XML constraint has no named bit field")
            constraints.append(
                FieldConstraint(name, high - width + 1, width, expression)
            )
        for bit, setting in _box_bits(box):
            if setting not in {"0", "1"}:
                continue
            bit_mask = 1 << bit
            wanted = bit_mask if setting == "1" else 0
            if mask & bit_mask and value & bit_mask != wanted:
                raise ValueError("Arm XML contains conflicting fixed bits")
            mask |= bit_mask
            value = (value & ~bit_mask) | wanted
    return mask, value, fields, constraints


def parse_feature_expression(text: str) -> tuple:
    """Parse an Arm XML feature expression into a tree.

    A node is ("feature", name), ("field", name, operator, pattern),
    ("and", children) or ("or", children); && binds tighter than ||.
    """
    tokens: list[str] = []
    position = 0
    while position < len(text):
        match = FEATURE_TOKEN.match(text, position)
        if match is None:
            raise ValueError(
                f"unsupported Arm XML feature expression {text!r}"
            )
        tokens.append(match.group(1))
        position = match.end()
    index = 0

    def peek() -> str | None:
        return tokens[index] if index < len(tokens) else None

    def take() -> str:
        nonlocal index
        if index >= len(tokens):
            raise ValueError(
                f"unsupported Arm XML feature expression {text!r}"
            )
        index += 1
        return tokens[index - 1]

    def primary() -> tuple:
        token = take()
        if token == "(":
            node = disjunction()
            if take() != ")":
                raise ValueError(
                    f"unsupported Arm XML feature expression {text!r}"
                )
            return node
        if token.startswith("FEAT_"):
            return ("feature", token)
        field = GUARD_FIELD.fullmatch(token)
        if field is None:
            raise ValueError(
                f"unsupported Arm XML feature expression {text!r}"
            )
        return ("field", *field.groups())

    def conjunction() -> tuple:
        children = [primary()]
        while peek() == "&&":
            take()
            children.append(primary())
        return children[0] if len(children) == 1 else ("and", tuple(children))

    def disjunction() -> tuple:
        children = [conjunction()]
        while peek() == "||":
            take()
            children.append(conjunction())
        return children[0] if len(children) == 1 else ("or", tuple(children))

    node = disjunction()
    if index != len(tokens):
        raise ValueError(f"unsupported Arm XML feature expression {text!r}")
    return node


def _expression_has_fields(node: tuple) -> bool:
    if node[0] == "field":
        return True
    if node[0] == "feature":
        return False
    return any(_expression_has_fields(child) for child in node[1])


def resolve_feature_fields(
    node: tuple, fields: dict[str, tuple[int, int]], word: int
) -> tuple | bool:
    """Replace each field comparison with its value for word and simplify.

    Returns True or False when the expression no longer names a feature.
    """
    kind = node[0]
    if kind == "feature":
        return node
    if kind == "field":
        _kind, name, operator, pattern = node
        if name not in fields:
            raise ValueError(
                f"feature expression names unknown field {name!r}"
            )
        low, width = fields[name]
        constraint = FieldConstraint(name, low, width, f"{operator} {pattern}")
        return constraint.matches(word)
    children: list[tuple] = []
    for child in node[1]:
        value = resolve_feature_fields(child, fields, word)
        if value is True or value is False:
            if (value is True) == (kind == "or"):
                return value
            continue
        children.append(value)
    if not children:
        return kind == "and"
    return children[0] if len(children) == 1 else (kind, tuple(children))


def format_feature_expression(node: tuple, parent: str = "or") -> str:
    kind = node[0]
    if kind == "feature":
        return node[1]
    if kind == "field":
        return f"{node[1]} {node[2]} '{node[3]}'"
    text = (" && " if kind == "and" else " || ").join(
        format_feature_expression(child, kind) for child in node[1]
    )
    return f"({text})" if kind == "or" and parent == "and" else text


def feature_expression_terms(node: tuple) -> list[frozenset[str]]:
    """Return the expression's disjunctive normal form as FEAT_ name sets."""
    kind = node[0]
    if kind == "feature":
        return [frozenset((node[1],))]
    if kind == "field":
        raise ValueError(
            "feature expression still depends on field "
            f"{format_feature_expression(node)!r}"
        )
    if kind == "or":
        return [
            term
            for child in node[1]
            for term in feature_expression_terms(child)
        ]
    terms = [frozenset()]
    for child in node[1]:
        terms = [
            left | right
            for left in terms
            for right in feature_expression_terms(child)
        ]
    return terms


def _declared_features(element: ET.Element) -> list[str]:
    result: list[str] = []
    for variant in element.findall("./arch_variants/arch_variant"):
        feature = variant.get("feature")
        if feature:
            parse_feature_expression(feature)
            result.append(feature)
    return result


def _decode_box_patterns(box: ET.Element) -> tuple[str, str | None]:
    """Return a normalized decode pattern and optional != expression."""
    width = int(box.get("width", "1"))
    pieces: list[str] = []
    inequality: str | None = None
    for cell in box.findall("c"):
        span = int(cell.get("colspan", "1"))
        text = (cell.text or "").strip()
        if text.startswith("!= "):
            if len(box.findall("c")) != 1 or span != width:
                raise ValueError("Arm XML != decode cell is not a whole box")
            inequality = text
            pieces.append("x" * span)
        elif not text:
            pieces.append("x" * span)
        elif re.fullmatch(r"[01x]+", text) is not None and len(text) == span:
            pieces.append(text)
        else:
            raise ValueError(f"unsupported Arm XML decode cell {text!r}")
    pattern = "".join(pieces)
    if len(pattern) != width:
        raise ValueError("Arm XML decode box width does not match its cells")
    return pattern, inequality


def _add_decode_boxes(
    mask: int,
    value: int,
    constraints: list[FieldConstraint],
    boxes: list[ET.Element],
) -> tuple[int, int, list[FieldConstraint]]:
    """Conjoin hierarchy decode boxes with an accumulated predicate."""
    result = list(constraints)
    for box in boxes:
        high = int(box.get("hibit", "-1"))
        width = int(box.get("width", "1"))
        low = high - width + 1
        pattern, inequality = _decode_box_patterns(box)
        name = box.get("name", f"bits_{high}_{low}")
        if inequality is not None:
            result.append(FieldConstraint(name, low, width, inequality))
        for offset, setting in enumerate(reversed(pattern)):
            if setting == "x":
                continue
            bit_mask = 1 << (low + offset)
            wanted = bit_mask if setting == "1" else 0
            if mask & bit_mask and value & bit_mask != wanted:
                raise ValueError("Arm XML contains conflicting decode bits")
            mask |= bit_mask
            value = (value & ~bit_mask) | wanted
    return mask, value, result


def _add_table_cells(
    mask: int,
    value: int,
    constraints: list[FieldConstraint],
    fields: dict[str, tuple[int, int]],
    names: list[str],
    cells: list[ET.Element],
) -> tuple[int, int, list[FieldConstraint]]:
    result = list(constraints)
    for name, cell in zip(names, cells):
        if name not in fields:
            raise ValueError(f"encoding-index table uses unknown field {name!r}")
        low, width = fields[name]
        text = "".join(cell.itertext()).strip()
        if not text:
            continue
        if text.startswith("!= "):
            pattern = text[3:]
            if len(pattern) != width or re.fullmatch(r"[01x]+", pattern) is None:
                raise ValueError(f"unsupported table constraint {text!r}")
            result.append(FieldConstraint(name, low, width, text))
            continue
        if len(text) != width or re.fullmatch(r"[01x]+", text) is None:
            raise ValueError(f"unsupported encoding-index pattern {text!r}")
        for offset, setting in enumerate(reversed(text)):
            if setting == "x":
                continue
            bit_mask = 1 << (low + offset)
            wanted = bit_mask if setting == "1" else 0
            if mask & bit_mask and value & bit_mask != wanted:
                raise ValueError("encoding-index row conflicts with its diagram")
            mask |= bit_mask
            value = (value & ~bit_mask) | wanted
    return mask, value, result


def _matching_table_features(
    root: ET.Element, fields: dict[str, tuple[int, int]], word: int
) -> list[str]:
    """Return feature expressions selected by word-indexed value tables."""
    result: list[str] = []
    for table in root.findall(".//table[@class='valuetable']"):
        header = table.find("./tgroup/thead/row")
        body = table.find("./tgroup/tbody")
        if header is None or body is None:
            continue
        headers = header.findall("entry")
        names = ["".join(entry.itertext()).strip() for entry in headers]
        bit_columns = [
            (index, names[index])
            for index, entry in enumerate(headers)
            if entry.get("class") == "bitfield" and names[index] in fields
        ]
        if not bit_columns:
            continue
        for table_row in body.findall("row"):
            entries = table_row.findall("entry")
            if len(entries) != len(names):
                continue
            matched = True
            for index, name in bit_columns:
                low, width = fields[name]
                pattern = "".join(entries[index].itertext()).strip()
                if len(pattern) != width or re.fullmatch(r"[01x]+", pattern) is None:
                    matched = False
                    break
                actual = (
                    f"{(word >> low) & ((1 << width) - 1):0{width}b}"
                )
                if any(
                    expected != "x" and expected != observed
                    for expected, observed in zip(pattern, actual)
                ):
                    matched = False
                    break
            if not matched:
                continue
            for entry in entries:
                for variant in entry.findall(".//arch_variant"):
                    feature = variant.get("feature")
                    if feature:
                        if _expression_has_fields(
                            parse_feature_expression(feature)
                        ):
                            raise ValueError(
                                "unsupported Arm XML feature expression "
                                f"{feature!r} in a value table"
                            )
                        result.append(feature)
    return result


def _decode_feature_guards(
    filename: str,
    instruction_class: ET.Element,
    fields: dict[str, tuple[int, int]],
) -> tuple[tuple[str, tuple[FieldConstraint, ...]], ...]:
    """Return field-conditional feature requirements from the decode ASL.

    Some encodings need an extra feature only for certain field values, and
    the XML records that only in a top-level decode statement such as
    ``if size == '01' && !IsFeatureImplemented(FEAT_FP16) then
    EndOfDecode(Decode_UNDEF); end;`` (the half-precision FCADD and FCMLA
    arrangements).  Each such statement whose condition is a conjunction of
    field comparisons and exactly one negated feature test yields that
    feature and its field conditions.  Top-level unconditional guards only
    repeat the declared features.  Statements nested in a case arm, which
    also depend on their selector, and other shapes (IN sets, disjunctions)
    are not read, nor are statements without a feature test that compare a
    decode variable such as SVE's tsize (tszh:tszl), which reserve values
    rather than require features.  DECODE_ERRATA corrects statements the XML
    gets wrong.
    """
    text = "\n".join(
        "".join(element.itertext())
        for element in instruction_class.findall(
            ".//pstext[@section='Decode']"
        )
    )
    if filename in DECODE_ERRATA:
        wrong, right = DECODE_ERRATA[filename]
        if text.count(wrong) != 1:
            raise ValueError(f"{filename}: decode erratum no longer applies")
        text = text.replace(wrong, right)
    result: list[tuple[str, tuple[FieldConstraint, ...]]] = []
    for line in text.splitlines():
        statement = UNDEF_GUARD.fullmatch(line)
        if statement is None:
            continue
        features: list[str] = []
        conditions: list[FieldConstraint] = []
        unknown: list[str] = []
        for atom in statement.group(1).split(" && "):
            feature = GUARD_FEATURE.fullmatch(atom)
            field = GUARD_FIELD.fullmatch(atom)
            if feature is not None:
                features.append(feature.group(1))
            elif field is not None:
                name, operator, pattern = field.groups()
                if name not in fields:
                    unknown.append(name)
                    continue
                low, width = fields[name]
                conditions.append(
                    FieldConstraint(name, low, width, f"{operator} {pattern}")
                )
            else:
                break
        else:
            if unknown and features:
                raise ValueError(
                    f"decode guard {line!r} names unknown field {unknown[0]!r}"
                )
            if len(features) == 1 and conditions and not unknown:
                result.append((features[0], tuple(conditions)))
    return tuple(result)


class ArchitectureCatalog:
    """Validated index and instruction forms from the pinned Arm XML release."""

    def __init__(self, root: Path):
        self.root = root
        self._files: dict[tuple[str, str], str] = {}
        self._stable_forms: dict[str, tuple[str, str, str]] = {}
        self._trees: dict[str, ET.Element] = {}
        unused_errata = set(HEADING_ERRATA)
        for section, (index_name, expected_count) in INDEXES.items():
            index_path = root / index_name
            if not index_path.is_file():
                raise ValueError(f"Arm XML release lacks {index_name}")
            entries = ET.parse(index_path).getroot().findall(".//iform")
            if len(entries) != expected_count:
                raise ValueError(
                    f"{index_path}: contains {len(entries)} forms, "
                    f"expected {expected_count} for {RELEASE}"
                )
            for ordinal, entry in enumerate(entries, start=1):
                heading = entry.get("heading")
                filename = entry.get("iformfile")
                if not heading or not filename:
                    raise ValueError(f"{index_path}: malformed form index entry")
                erratum = HEADING_ERRATA.get((section, filename))
                if erratum is not None:
                    if heading != erratum[0]:
                        raise ValueError(
                            f"{index_path}: heading erratum for {filename} "
                            "no longer applies"
                        )
                    heading = erratum[1]
                    unused_errata.discard((section, filename))
                key = (section, heading)
                if key in self._files:
                    raise ValueError(f"{index_path}: duplicate form {heading!r}")
                self._files[key] = filename
                stable_id = (
                    f"{STABLE_ID_PREFIXES[section]}-{ordinal:04d}"
                )
                self._stable_forms[stable_id] = (
                    section,
                    heading,
                    filename,
                )
        if unused_errata:
            raise ValueError(
                "heading errata name no indexed form: "
                + ", ".join(sorted(f"{s} {f}" for s, f in unused_errata))
            )
        self._forms_by_file = {
            filename: key for key, filename in self._files.items()
        }
        if len(self._forms_by_file) != len(self._files):
            raise ValueError("Arm XML indexes name a form file twice")

    @property
    def stable_forms(self) -> dict[str, tuple[str, str, str]]:
        """Return stable IDs derived solely from official index order."""
        return dict(self._stable_forms)

    def stable_form(self, stable_id: str) -> tuple[str, str, str]:
        try:
            return self._stable_forms[stable_id]
        except KeyError as error:
            raise ValueError(
                f"Arm {RELEASE} XML has no indexed stable ID {stable_id!r}"
            ) from error

    def _tree(self, filename: str) -> ET.Element:
        cached = self._trees.get(filename)
        if cached is not None:
            return cached
        path = self.root / filename
        if not path.is_file() or path.parent != self.root:
            raise ValueError(f"Arm XML release lacks indexed file {filename}")
        root = ET.parse(path).getroot()
        commit_ids = {item.text for item in root.findall(".//commit_id")}
        if commit_ids != {COMMIT_ID}:
            raise ValueError(
                f"{path}: commit IDs are {sorted(str(x) for x in commit_ids)!r}, "
                f"expected {COMMIT_ID!r}"
            )
        self._trees[filename] = root
        return root

    def encoding_count(self, section: str, form: str) -> int:
        filename = self._files.get((section, form))
        if filename is None:
            raise ValueError(
                f"Arm {RELEASE} XML has no {section} form {form!r}"
            )
        count = len(self._tree(filename).findall("./classes/iclass/encoding"))
        if count == 0:
            raise ValueError(f"{filename}: form contains no encodings")
        return count

    def all_variants(self, section: str, form: str) -> list[ArchitectureVariant]:
        filename = self._files.get((section, form))
        if filename is None:
            raise ValueError(
                f"Arm {RELEASE} XML has no {section} form {form!r}"
            )
        root = self._tree(filename)
        result: list[ArchitectureVariant] = []
        for instruction_class in root.findall("./classes/iclass"):
            diagram = instruction_class.find("regdiagram")
            if diagram is None:
                continue
            mask, value, fields, constraints = _add_boxes(
                0, 0, diagram.findall("box")
            )
            class_features = _declared_features(instruction_class)
            for encoding in instruction_class.findall("encoding"):
                (
                    encoding_mask,
                    encoding_value,
                    encoding_fields,
                    encoding_constraints,
                ) = _add_boxes(mask, value, encoding.findall("box"))
                all_fields = dict(fields)
                all_fields.update(encoding_fields)
                all_constraints = tuple(constraints + encoding_constraints)
                features = tuple(
                    dict.fromkeys(class_features + _declared_features(encoding))
                )
                variant = ArchitectureVariant(
                    section=section,
                    form=form,
                    file=filename,
                    encoding=encoding.get("name", ""),
                    mask=encoding_mask,
                    value=encoding_value,
                    constraints=all_constraints,
                    feature_expressions=features,
                    fields=tuple(
                        sorted(
                            (name, low, width)
                            for name, (low, width) in all_fields.items()
                        )
                    ),
                    feature_guards=_decode_feature_guards(
                        filename, instruction_class, all_fields
                    ),
                )
                if not variant.encoding:
                    raise ValueError(f"{filename}: encoding has no name")
                result.append(variant)
        return result

    def _alias_operation_features(
        self, root: ET.Element, word: int
    ) -> list[str]:
        """Return the features an alias's operation table gives this word.

        SYS and SYSP pass their operation to an implementation-defined
        dispatcher, and the XML records which operations exist, and under
        which features, only in the operation tables of their aliases (AT,
        BRB, DC, TLBI and TLBIP).  A word that names such an operation needs
        those features: SYSP with TLBIP RVAE1OSNXS's operands needs
        FEAT_D128 || FEAT_TLBID as well as FEAT_SYSINSTR128.  An alias's own
        declared features are not inherited, because a base form can give
        them an architected NOP fallback, as HINT does for BTI.
        """
        result: list[str] = []
        for reference in root.findall("./alias_list/aliasref"):
            alias_file = reference.get("aliasfile", "")
            alias_form = self._forms_by_file.get(alias_file)
            if alias_form is None:
                raise ValueError(f"alias file {alias_file!r} is not indexed")
            alias_root = self._tree(alias_file)
            for alias in self.all_variants(*alias_form):
                if alias.matches(word):
                    result += _matching_table_features(
                        alias_root,
                        {name: (low, width) for name, low, width in alias.fields},
                        word,
                    )
        return result

    def variants(self, section: str, form: str, word: int) -> list[ArchitectureVariant]:
        result: list[ArchitectureVariant] = []
        root = self._tree(self._files[(section, form)])
        operation_features = self._alias_operation_features(root, word)
        for variant in self.all_variants(section, form):
            if not variant.matches(word):
                continue
            fields = {
                name: (low, width) for name, low, width in variant.fields
            }
            declared: list[str] = []
            for expression in variant.feature_expressions:
                tree = parse_feature_expression(expression)
                if not _expression_has_fields(tree):
                    declared.append(expression)
                    continue
                resolved = resolve_feature_fields(tree, fields, word)
                if resolved is False:
                    raise ValueError(
                        f"{section} {form}: {word:08x} fails its feature "
                        f"expression {expression!r}"
                    )
                if resolved is not True:
                    declared.append(format_feature_expression(resolved))
            variant = replace(variant, feature_expressions=tuple(declared))
            dynamic = (
                _matching_table_features(
                    root,
                    {name: (low, width) for name, low, width in variant.fields},
                    word,
                )
                + variant.guarded_features(word)
                + operation_features
            )
            result.append(
                replace(
                    variant,
                    feature_expressions=tuple(
                        dict.fromkeys(
                            list(variant.feature_expressions) + dynamic
                        )
                    ),
                )
            )
        return result

    def resolve(self, section: str, form: str, word: int) -> ArchitectureVariant:
        matches = self.variants(section, form, word)
        if not matches:
            raise ValueError(
                f"{section} {form}: {word:08x} matches no Arm {RELEASE} "
                "XML encoding"
            )
        specificity = max(variant.mask.bit_count() for variant in matches)
        matches = [
            variant for variant in matches if variant.mask.bit_count() == specificity
        ]
        identities = {
            (
                variant.mask,
                variant.value,
                variant.constraints,
                variant.feature_expressions,
            )
            for variant in matches
        }
        if len(identities) != 1:
            names = ", ".join(sorted(variant.encoding for variant in matches))
            raise ValueError(
                f"{section} {form}: {word:08x} ambiguously matches Arm XML "
                f"encodings {names}"
            )
        # Encodings that only an alias condition outside the encoding boxes
        # tells apart, such as the two of SVE MOV (SIMD&FP scalar,
        # unpredicated), decode the same words under the same features; the
        # first in XML order stands for them.
        return matches[0]


class DecodeCatalog:
    """Official A64 decode hierarchy and terminal instruction-table rows."""

    def __init__(self, root: Path):
        path = root / "encodingindex.xml"
        if not path.is_file():
            raise ValueError("Arm XML release lacks encodingindex.xml")
        self.root = ET.parse(path).getroot()
        if self.root.get("instructionset") != "A64":
            raise ValueError("encodingindex.xml is not the A64 index")

    def _hierarchy_partitions(self) -> list[DecodePartition]:
        hierarchy = self.root.find("./hierarchy")
        if hierarchy is None:
            raise ValueError("encodingindex.xml lacks its decode hierarchy")
        result: list[DecodePartition] = []

        def visit(
            node: ET.Element,
            mask: int,
            value: int,
            constraints: list[FieldConstraint],
        ) -> None:
            decode = node.find("decode")
            if decode is None:
                raise ValueError("decode hierarchy node lacks decode boxes")
            mask, value, constraints = _add_decode_boxes(
                mask, value, constraints, decode.findall("box")
            )
            children = node.findall("node")
            if children:
                for child in children:
                    visit(child, mask, value, constraints)
                return
            name = node.get("iclass", "")
            if not name:
                raise ValueError("decode hierarchy leaf has no identity")
            kind = (
                "unallocated"
                if node.get("unallocated") == "1"
                else "instruction-class"
            )
            result.append(
                DecodePartition(
                    layer="hierarchy",
                    kind=kind,
                    decode_class=name,
                    name=name,
                    file="none",
                    mask=mask,
                    value=value,
                    constraints=tuple(constraints),
                    feature_expressions=(),
                )
            )

        for node in hierarchy.findall("node"):
            visit(node, 0, 0, [])
        return result

    def _table_partitions(self) -> list[DecodePartition]:
        result: list[DecodePartition] = []
        for section in self.root.findall("./iclass_sect"):
            decode_class = section.get("id", "")
            diagram = section.find("regdiagram")
            table = section.find("instructiontable")
            if not decode_class or diagram is None or table is None:
                raise ValueError("malformed encoding-index instruction class")
            mask, value, fields, constraints = _add_boxes(
                0, 0, diagram.findall("box")
            )
            headings = table.findall(
                "./thead/tr[@id='heading2']/th[@class='bitfields']"
            )
            names = ["".join(item.itertext()).strip() for item in headings]
            for row in table.findall("./tbody/tr"):
                cells = row.findall("td[@class='bitfield']")
                if len(cells) != len(names):
                    raise ValueError(
                        f"{decode_class}: row has {len(cells)} fields, "
                        f"expected {len(names)}"
                    )
                row_mask, row_value, row_constraints = _add_table_cells(
                    mask, value, constraints, fields, names, cells
                )
                name = row.get("encname", "")
                if not name:
                    raise ValueError(f"{decode_class}: table row has no name")
                filename = row.get("iformfile", "")
                if filename:
                    kind = "allocated"
                elif row.get("undef") == "1":
                    kind = "unallocated"
                    filename = "none"
                else:
                    raise ValueError(
                        f"{decode_class} {name}: unclassified table row"
                    )
                feature = row.get("arch_version", "")
                if feature and any(character in feature for character in "\t\r\n"):
                    raise ValueError(
                        f"unsupported table feature expression {feature!r}"
                    )
                result.append(
                    DecodePartition(
                        layer="instruction-table",
                        kind=kind,
                        decode_class=decode_class,
                        name=name,
                        file=filename,
                        mask=row_mask,
                        value=row_value,
                        constraints=tuple(dict.fromkeys(row_constraints)),
                        feature_expressions=(feature,) if feature else (),
                    )
                )
        return result

    def partitions(self) -> list[DecodePartition]:
        result = self._hierarchy_partitions() + self._table_partitions()
        identities = {
            (
                item.layer,
                item.decode_class,
                item.name,
                item.file,
                item.mask,
                item.value,
                item.constraints,
            )
            for item in result
        }
        if len(identities) != len(result):
            raise ValueError("encoding-index contains duplicate decode partitions")
        return result


def feature_alternatives(
    expressions: tuple[str, ...], architecture_file: str = ""
) -> set[frozenset[str]]:
    """Convert the conjunction of Arm feature expressions to host alternatives."""
    alternatives: set[frozenset[str]] = {frozenset()}
    for expression in expressions:
        terms = [
            frozenset(_host_feature(name) for name in term)
            for term in feature_expression_terms(
                parse_feature_expression(expression)
            )
        ]
        alternatives = {
            left | right for left in alternatives for right in terms
        }
    return {
        _runtime_feature_closure(features, architecture_file)
        for features in alternatives
    }


def _host_feature(feature: str) -> str:
    if not feature.startswith("FEAT_"):
        raise ValueError(f"invalid Arm feature name {feature!r}")
    name = feature[5:]
    exceptions = {
        "AdvSIMD": "advanced_simd",
        "DotProd": "dotprod",
        "FP": "floating_point",
        "FPMR": "fp8",
        "FlagM": "flag_m",
        "FlagM2": "flag_m2",
        "PAuth": "pauth",
        "PAuth_LR": "pauth_lr",
        "WFxT": "wfxt",
    }
    return exceptions.get(name, name.lower())


def _runtime_feature_closure(
    features: frozenset[str], architecture_file: str
) -> frozenset[str]:
    """Map architectural extension requirements to exact runtime feature gates.

    These additions are reviewed host-gating dependencies. They are fixed code,
    independent of the inventory, bindings, and emitted execution evidence.
    """
    result = set(features)
    if "aes" in result:
        result.add("advanced_simd")
    if "rdm" in result:
        result.add("advanced_simd")
    if "sha3" in result:
        result.add("advanced_simd")
    if "fcma" in result:
        result.add("advanced_simd")
    if "fp16" in result:
        if "_advsimd" in architecture_file:
            result.add("advanced_simd")
        elif "_float" in architecture_file:
            result.add("floating_point")
        else:
            raise ValueError(
                "FEAT_FP16 form has no reviewed runtime parent: "
                f"{architecture_file}"
            )
    if "frintts" in result:
        if architecture_file.endswith("_advsimd.xml"):
            result.add("advanced_simd")
        elif architecture_file.endswith("_float.xml"):
            result.add("floating_point")
        else:
            raise ValueError(
                "FEAT_FRINTTS form has no reviewed runtime parent: "
                f"{architecture_file}"
            )
    return frozenset(result)
