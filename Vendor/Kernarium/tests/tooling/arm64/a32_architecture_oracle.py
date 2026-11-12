#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Arm AArch32 XML access for the A32/T32 encoding table and inventory.

This module reads Arm's machine-readable AArch32 release, pinned beside the
A64 one by name and SHA-256, and exposes its instruction pages and its
encoding-index decode hierarchy. Only information present in the XML is
exposed here; the choices the virtual CPU's profile makes on top of it (what
is UNDEFINED at EL0, what a feature gates, how UNPREDICTABLE resolves) live in
generate_a32_encoding_table.py, where they are visible in one place.

Word layout. An A32 word is the 32-bit instruction. A T32 word is the pair of
halfwords as hw1 << 16 | hw2, so a 16-bit instruction occupies the upper
half and leaves the lower half as don't-care. Every mask, value and field
position below uses that layout, whatever box numbering the XML page uses.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import re
import tarfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

from a64_architecture_oracle import (
    FieldConstraint,
    _box_bits,
    parse_feature_expression,
)


RELEASE = "2026-06"
COMMIT_ID = "2026-06_rel"
ARCHIVE_NAME = f"ISA_AArch32_xml_A_profile-{RELEASE}.tar.gz"
ARCHIVE_SHA256 = (
    "b389a93f41504376895b723ed6e9861ce369e2253c138e6f4c46e27e95375887"
)
SOURCE_ORACLE = f"arm-aarch32-a-profile-{RELEASE}-xml-{COMMIT_ID}"
# The archive carries the previous release's change markup beside the
# release; its pages hold a different commit tag and are never read.
ARCHIVE_ROOT = f"ISA_AArch32_xml_A_profile-{RELEASE}"
ARCHIVE_MEMBERS = {
    ARCHIVE_ROOT,
    f"{ARCHIVE_ROOT}.pdf",
    f"{ARCHIVE_ROOT}-diff-from-ISA_AArch32_xml_A_profile-2026-03.pdf",
    f"{ARCHIVE_ROOT}_relnote.html",
    "ISA_AArch32_xml_A_profile-2026-03",
}
INDEXES = {
    "Base": ("index.xml", 302),
    "FP&SIMD": ("fpsimdindex.xml", 265),
}
STABLE_ID_PREFIXES = {"Base": "BASE", "FP&SIMD": "FPSIMD"}
ENCODING_INDEXES = {"A32": "a32_encindex.xml", "T32": "t32_encindex.xml"}
SIMPLE_CONSTRAINT = re.compile(r"(!=|==) ([01x]+)\Z")
SHOULD_BE = {"(0)": 0, "(1)": 1}


class CatalogError(ValueError):
    pass


def archive_root(archive: Path, temporary: Path, workspace=None) -> Path:
    """Verify the pinned archive and extract the release's XML pages."""
    if archive.name != ARCHIVE_NAME:
        raise ValueError(
            f"architecture archive is {archive.name!r}, expected {ARCHIVE_NAME!r}"
        )
    digest_hash = hashlib.sha256()
    with archive.open("rb") as archive_input:
        for block in iter(lambda: archive_input.read(1024 * 1024), b""):
            digest_hash.update(block)
    digest = digest_hash.hexdigest()
    if digest != ARCHIVE_SHA256:
        raise ValueError(
            f"{archive}: SHA-256 is {digest}, expected pinned {ARCHIVE_SHA256}"
        )
    with tarfile.open(archive, "r:gz") as source:
        members = source.getmembers()
        wanted = []
        for member in members:
            path = Path(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"{archive}: unsafe archive member {member.name!r}")
            if path.parts and path.parts[0] not in ARCHIVE_MEMBERS:
                raise ValueError(
                    f"{archive}: unexpected top-level member {member.name!r}"
                )
            if member.issym() or member.islnk():
                raise ValueError(f"{archive}: link member is not accepted")
            if (
                path.parts[0] == ARCHIVE_ROOT
                and len(path.parts) == 2
                and path.suffix == ".xml"
                and member.isfile()
            ):
                wanted.append(member)
        if workspace is not None:
            workspace.validate_archive_members(wanted)
        source.extractall(temporary, members=wanted, filter="data")
    if workspace is not None:
        workspace.check()
    return temporary / ARCHIVE_ROOT


@dataclass(frozen=True)
class JoinedConstraint:
    """A != constraint over fields that an index column joins (R:imm4)."""

    segments: tuple[tuple[str, int, int], ...]
    expression: str

    @property
    def name(self) -> str:
        return ":".join(name for name, _low, _width in self.segments)

    def matches(self, word: int) -> bool:
        pattern = self.expression[3:]
        actual = ""
        for _name, low, width in self.segments:
            actual += f"{(word >> low) & ((1 << width) - 1):0{width}b}"
        equal = all(
            expected == "x" or expected == observed
            for expected, observed in zip(pattern, actual)
        )
        return not equal

    def text(self) -> str:
        return (
            "+".join(f"{n}@{low}:{width}" for n, low, width in self.segments)
            + self.expression.replace(" ", "")
        )


@dataclass(frozen=True)
class Field:
    name: str
    low: int
    width: int


@dataclass(frozen=True)
class Encoding:
    """One XML encoding of an instruction page, in word layout."""

    name: str
    section: str
    form: str
    form_id: str
    file: str
    iclass: str
    psname: str
    isa: str  # "A32", "T16" or "T32"
    alias: bool
    mask: int
    value: int
    constraints: tuple[FieldConstraint, ...]
    fields: tuple[Field, ...]
    should_mask: int
    should_value: int
    features: tuple[str, ...]
    mnemonic: str

    def matches(self, word: int) -> bool:
        return word & self.mask == self.value and all(
            constraint.matches(word) for constraint in self.constraints
        )

    def field(self, name: str) -> Field:
        for field in self.fields:
            if field.name == name:
                return field
        raise KeyError(name)

    def field_value(self, name: str, word: int) -> int:
        field = self.field(name)
        return (word >> field.low) & ((1 << field.width) - 1)

    @property
    def length(self) -> int:
        return 2 if self.isa == "T16" else 4


@dataclass(frozen=True)
class Partition:
    """One leaf of the official encoding-index decode tables.

    kind is "allocated" (an instruction encoding), "unallocated" (UNDEFINED),
    "unpredictable" (a table row the index marks UNPREDICTABLE) or "nop-hint"
    (a reserved hint, which executes as a NOP).
    """

    isa: str
    kind: str
    iclass: str
    name: str
    file: str
    mask: int
    value: int
    constraints: tuple[FieldConstraint, ...]
    feature_expressions: tuple[str, ...]

    def matches(self, word: int) -> bool:
        return word & self.mask == self.value and all(
            constraint.matches(word) for constraint in self.constraints
        )


def _layout_shift(form: str) -> int:
    """How far a box position moves to reach word layout."""
    if form == "16":
        return 16
    if form in ("32", "16x2"):
        return 0
    raise CatalogError(f"unsupported Arm XML register diagram form {form!r}")


def _boxes(
    diagram: ET.Element,
    shift: int,
    mask: int = 0,
    value: int = 0,
    fields: dict | None = None,
    constraints: list | None = None,
    should: tuple[int, int] = (0, 0),
):
    """Fold a register diagram's boxes into word-layout predicates."""
    fields = {} if fields is None else dict(fields)
    constraints = [] if constraints is None else list(constraints)
    should_mask, should_value = should
    for box in diagram.findall("box"):
        high = int(box.get("hibit", "-1"))
        width = int(box.get("width", "1"))
        name = box.get("name", "")
        low = high - width + 1 + shift
        if high < 0 or low < 0 or high + shift > 31:
            raise CatalogError("Arm XML contains an out-of-range encoding box")
        if name:
            previous = fields.get(name)
            if previous is not None and previous != (low, width):
                raise CatalogError(f"box {name!r} redeclared with a new position")
            fields[name] = (low, width)
        expression = box.get("constraint")
        if expression is not None:
            if not name:
                raise CatalogError("Arm XML constraint has no named bit field")
            if re.fullmatch(r"[01x]+", expression):
                expression = f"== {expression}"
            constraint = FieldConstraint(name, low, width, expression)
            if constraint not in constraints:
                constraints.append(constraint)
        for bit, setting in _box_bits(box):
            bit += shift
            bit_mask = 1 << bit
            if setting in SHOULD_BE:
                wanted = SHOULD_BE[setting]
                should_mask |= bit_mask
                should_value = (should_value & ~bit_mask) | (wanted << bit)
                continue
            if setting not in {"0", "1"}:
                continue
            wanted = bit_mask if setting == "1" else 0
            if mask & bit_mask and value & bit_mask != wanted:
                raise CatalogError("Arm XML contains conflicting fixed bits")
            mask |= bit_mask
            value = (value & ~bit_mask) | wanted
    return mask, value, fields, constraints, (should_mask, should_value)


def _declared_features(element: ET.Element) -> list[str]:
    result: list[str] = []
    for variant in element.findall("./arch_variants/arch_variant"):
        feature = variant.get("feature")
        if feature:
            parse_feature_expression(feature)
            result.append(feature)
    return result


class ArchitectureCatalog:
    """Validated instruction pages of the pinned Arm AArch32 XML release."""

    def __init__(self, root: Path):
        self.root = root
        self._files: dict[tuple[str, str], str] = {}
        self._sections: dict[str, str] = {}
        self._trees: dict[str, ET.Element] = {}
        self._stable: dict[str, tuple[str, str, str]] = {}
        for section, (index_name, expected_count) in INDEXES.items():
            index_path = root / index_name
            if not index_path.is_file():
                raise CatalogError(f"Arm XML release lacks {index_name}")
            entries = ET.parse(index_path).getroot().findall(".//iform")
            if len(entries) != expected_count:
                raise CatalogError(
                    f"{index_path}: contains {len(entries)} forms, "
                    f"expected {expected_count} for {RELEASE}"
                )
            for ordinal, entry in enumerate(entries, start=1):
                heading = entry.get("heading")
                filename = entry.get("iformfile")
                if not heading or not filename:
                    raise CatalogError(f"{index_path}: malformed form index entry")
                key = (section, heading)
                if key in self._files:
                    raise CatalogError(f"{index_path}: duplicate form {heading!r}")
                if filename in self._sections:
                    raise CatalogError(f"Arm XML indexes {filename} twice")
                self._files[key] = filename
                self._sections[filename] = section
                self._stable[f"{STABLE_ID_PREFIXES[section]}-{ordinal:04d}"] = (
                    section,
                    heading,
                    filename,
                )

    @property
    def stable_forms(self) -> dict[str, tuple[str, str, str]]:
        """Stable IDs derived solely from official index order."""
        return dict(self._stable)

    def files(self) -> list[tuple[str, str, str]]:
        """Every indexed (section, heading, file) in index order."""
        return [
            (section, heading, self._files[(section, heading)])
            for _stable, (section, heading, _file) in self._stable.items()
        ]

    def tree(self, filename: str) -> ET.Element:
        cached = self._trees.get(filename)
        if cached is not None:
            return cached
        path = self.root / filename
        if not path.is_file() or path.parent != self.root:
            raise CatalogError(f"Arm XML release lacks indexed file {filename}")
        root = ET.parse(path).getroot()
        commit_ids = {item.text for item in root.findall(".//commit_id")}
        if commit_ids != {COMMIT_ID}:
            raise CatalogError(
                f"{path}: commit IDs are {sorted(str(x) for x in commit_ids)!r}, "
                f"expected {COMMIT_ID!r}"
            )
        self._trees[filename] = root
        return root

    def is_alias(self, filename: str) -> bool:
        return self.tree(filename).get("type") == "alias"

    def form_id(self, filename: str) -> str:
        return self.tree(filename).get("id", "")

    def mnemonic(self, filename: str) -> str:
        for docvar in self.tree(filename).findall("./docvars/docvar"):
            if docvar.get("key") == "mnemonic":
                return docvar.get("value", "")
        return ""

    def decode_text(self, filename: str, psname: str) -> str:
        """The Decode pseudocode of the iclass whose diagram names psname."""
        root = self.tree(filename)
        for instruction_class in root.findall("./classes/iclass"):
            diagram = instruction_class.find("regdiagram")
            if diagram is None or diagram.get("psname") != psname:
                continue
            return "\n".join(
                "".join(element.itertext())
                for element in instruction_class.findall(
                    ".//pstext[@section='Decode']"
                )
            )
        raise CatalogError(f"{filename}: no iclass with psname {psname!r}")

    def execute_text(self, filename: str) -> str:
        root = self.tree(filename)
        return "\n".join(
            "".join(element.itertext())
            for element in root.findall("./ps_section//pstext[@section='Execute']")
        )

    def encodings(self, filename: str) -> list[Encoding]:
        section = self._sections[filename]
        heading = next(
            heading
            for (found_section, heading), name in self._files.items()
            if found_section == section and name == filename
        )
        root = self.tree(filename)
        alias = self.is_alias(filename)
        mnemonic = self.mnemonic(filename)
        result: list[Encoding] = []
        for instruction_class in root.findall("./classes/iclass"):
            diagram = instruction_class.find("regdiagram")
            if diagram is None:
                raise CatalogError(f"{filename}: iclass has no register diagram")
            isa = instruction_class.get("isa", "")
            form = diagram.get("form", "")
            if isa not in ("A32", "T32"):
                raise CatalogError(f"{filename}: unsupported instruction set {isa!r}")
            shift = _layout_shift(form)
            if (isa == "A32") != (form == "32"):
                raise CatalogError(f"{filename}: {isa} iclass uses form {form!r}")
            mask, value, fields, constraints, should = _boxes(diagram, shift)
            class_features = _declared_features(instruction_class)
            for encoding in instruction_class.findall("encoding"):
                (
                    encoding_mask,
                    encoding_value,
                    encoding_fields,
                    encoding_constraints,
                    encoding_should,
                ) = _boxes(
                    encoding, shift, mask, value, fields, constraints, should
                )
                name = encoding.get("name", "")
                if not name:
                    raise CatalogError(f"{filename}: encoding has no name")
                if name in XML_NAME_ERRATA:
                    name = _corrected_name(
                        filename, name, encoding_mask, encoding_value,
                        encoding_fields,
                    )
                features = tuple(
                    dict.fromkeys(class_features + _declared_features(encoding))
                )
                result.append(
                    Encoding(
                        name=name,
                        section=section,
                        form=heading,
                        form_id=root.get("id", ""),
                        file=filename,
                        iclass=instruction_class.get("name", ""),
                        psname=diagram.get("psname", ""),
                        isa="A32" if isa == "A32" else (
                            "T16" if form == "16" else "T32"
                        ),
                        alias=alias,
                        mask=encoding_mask,
                        value=encoding_value,
                        constraints=tuple(encoding_constraints),
                        fields=tuple(
                            sorted(
                                (Field(n, low, width)
                                 for n, (low, width) in encoding_fields.items()),
                                key=lambda f: (-f.low, f.name),
                            )
                        ),
                        should_mask=encoding_should[0],
                        should_value=encoding_should[1],
                        features=features,
                        mnemonic=encoding_mnemonic(encoding, mnemonic),
                    )
                )
        return result

    def all_encodings(self) -> list[Encoding]:
        result: list[Encoding] = []
        for _stable, (_section, _heading, filename) in self._stable.items():
            result.extend(self.encodings(filename))
        names = [encoding.name for encoding in result]
        if len(set(names)) != len(names):
            raise CatalogError("Arm XML encoding names are not unique")
        return result


# Arm's 2026-06 XML names its A32 VRSRA encodings against their own Q field:
# "VRSRA_A1_Q" is the 64-bit encoding (Q == 0) and "VRSRA_A1_D" the 128-bit
# one, while the T32 pair and every other D/Q pair are right. Each entry maps
# the XML's name to the corrected name, the Q value the XML encoding actually
# carries and the label its own encoding index gives it; an encoding or row
# that no longer carries them means the XML was corrected, and the generation
# fails so that the entry is removed. The index names its rows the same way,
# so the corrected name applies there too.
XML_NAME_ERRATA = {
    "VRSRA_A1_Q": ("VRSRA_A1_D", 0, "64-bit SIMD vector"),
    "VRSRA_A1_D": ("VRSRA_A1_Q", 1, "128-bit SIMD vector"),
}


def _corrected_row_name(iclass: str, name: str, label: str) -> str:
    corrected, _q, expected_label = XML_NAME_ERRATA[name]
    if label != expected_label:
        raise CatalogError(
            f"{iclass}: index row {name} is no longer labelled {expected_label!r}; "
            "remove it from XML_NAME_ERRATA"
        )
    return corrected


def _corrected_name(
    filename: str,
    name: str,
    mask: int,
    value: int,
    fields: dict[str, tuple[int, int]],
) -> str:
    corrected, expected_q, _label = XML_NAME_ERRATA[name]
    low, width = fields.get("Q", (-1, 0))
    if width != 1 or not (mask >> low) & 1 or (value >> low) & 1 != expected_q:
        raise CatalogError(
            f"{filename}: {name} no longer carries Q == {expected_q}; "
            "remove it from XML_NAME_ERRATA"
        )
    return corrected


def encoding_mnemonic(encoding: ET.Element, page_mnemonic: str) -> str:
    for docvar in encoding.findall("./docvars/docvar"):
        if docvar.get("key") == "mnemonic":
            return docvar.get("value", page_mnemonic)
    return page_mnemonic


# Decode-index partitions ---------------------------------------------------


def _pattern_constraint(
    name: str, low: int, width: int, text: str
) -> tuple[int, int, FieldConstraint | None]:
    """Fold a decode cell into (mask, value, constraint) at word position."""
    text = text.strip()
    if not text:
        return 0, 0, None
    if text.startswith("!= "):
        pattern = text[3:]
        if len(pattern) != width or re.fullmatch(r"[01x]+", pattern) is None:
            raise CatalogError(f"unsupported decode constraint {text!r}")
        return 0, 0, FieldConstraint(name, low, width, text)
    if len(text) != width or re.fullmatch(r"[01x]+", text) is None:
        raise CatalogError(f"unsupported decode pattern {text!r}")
    mask = value = 0
    for offset, setting in enumerate(reversed(text)):
        if setting == "x":
            continue
        mask |= 1 << (low + offset)
        if setting == "1":
            value |= 1 << (low + offset)
    return mask, value, None


def _table_segments(
    iclass: str, name: str, fields: dict
) -> list[tuple[str, int, int]]:
    """Segments (name, low, width), high first, of a table column.

    A column may join fields that are not adjacent (R:imm4), its pattern
    running over their bits in order.
    """
    result = []
    for part in name.split(":"):
        if part not in fields:
            raise CatalogError(f"{iclass}: unknown table field {part!r}")
        result.append((part, *fields[part]))
    return result


def _segment_cell(
    segments: list[tuple[str, int, int]], text: str
) -> tuple[int, int, FieldConstraint | None]:
    text = text.strip()
    if not text:
        return 0, 0, None
    if text.startswith("!= ") and len(segments) > 1:
        pattern = text[3:]
        total = sum(width for _name, _low, width in segments)
        if len(pattern) != total or re.fullmatch(r"[01x]+", pattern) is None:
            raise CatalogError(f"unsupported decode constraint {text!r}")
        return 0, 0, JoinedConstraint(tuple(segments), text)
    if len(segments) == 1:
        name, low, width = segments[0]
        return _pattern_constraint(name, low, width, text)
    total = sum(width for _name, _low, width in segments)
    if len(text) != total or re.fullmatch(r"[01x]+", text) is None:
        raise CatalogError(f"unsupported decode pattern {text!r}")
    mask = value = 0
    position = 0
    for _name, low, width in segments:
        piece = text[position:position + width]
        position += width
        for offset, setting in enumerate(reversed(piece)):
            if setting == "x":
                continue
            mask |= 1 << (low + offset)
            if setting == "1":
                value |= 1 << (low + offset)
    return mask, value, None


def _merge(mask, value, add_mask, add_value) -> tuple[int, int]:
    if mask & add_mask & (value ^ add_value):
        raise CatalogError("decode partition has conflicting fixed bits")
    return mask | add_mask, value | add_value


class DecodeIndex:
    """The official A32 and T32 encoding-index decode hierarchy and tables."""

    def __init__(self, root: Path):
        self.roots: dict[str, ET.Element] = {}
        for isa, name in ENCODING_INDEXES.items():
            path = root / name
            if not path.is_file():
                raise CatalogError(f"Arm XML release lacks {name}")
            tree = ET.parse(path).getroot()
            if tree.get("instructionset") != isa:
                raise CatalogError(f"{name} is not the {isa} index")
            commit = {item.text for item in tree.findall(".//commit_id")}
            if commit and commit != {COMMIT_ID}:
                raise CatalogError(f"{name}: unexpected commit IDs {commit!r}")
            self.roots[isa] = tree

    @staticmethod
    def _decode_boxes(node: ET.Element, shift: int):
        mask = value = 0
        constraints: list[FieldConstraint] = []
        decode = node.find("decode")
        if decode is None:
            raise CatalogError("decode hierarchy node lacks decode boxes")
        for box in decode.findall("box"):
            high = int(box.get("hibit", "-1"))
            width = int(box.get("width", "1"))
            low = high - width + 1 + shift
            if low < 0 or high + shift > 31:
                raise CatalogError("decode box is out of range")
            cells = box.findall("c")
            text = "".join(
                [(cell.text or "").strip() or "x" * int(cell.get("colspan", "1"))
                 for cell in cells]
            )
            if len(cells) == 1 and (cells[0].text or "").strip().startswith("!="):
                text = cells[0].text.strip()
            name = box.get("name") or f"bits_{high}_{low}"
            cell_mask, cell_value, constraint = _pattern_constraint(
                name, low, width, text
            )
            mask, value = _merge(mask, value, cell_mask, cell_value)
            if constraint is not None:
                constraints.append(constraint)
        return mask, value, constraints

    def _class_predicates(self, isa: str):
        """Hierarchy leaves as {iclass: (mask, value, constraints, node)}."""
        hierarchy = self.roots[isa].find("hierarchy")
        if hierarchy is None:
            raise CatalogError("encoding index lacks its decode hierarchy")
        result: dict[str, tuple] = {}

        def visit(node, mask, value, constraints, shift):
            node_mask, node_value, node_constraints = self._decode_boxes(node, shift)
            mask, value = _merge(mask, value, node_mask, node_value)
            constraints = constraints + node_constraints
            # The 16-bit subtree numbers its boxes within the halfword.
            child_shift = 16 if (isa == "T32" and node.get("groupname") == "n") else shift
            children = node.findall("node")
            if children:
                for child in children:
                    visit(child, mask, value, constraints, child_shift)
                return
            name = node.get("iclass", "")
            if not name:
                raise CatalogError("decode hierarchy leaf has no identity")
            if node.get("unallocated") == "1":
                leaf = "unallocated"
            elif node.get("unpredictable") == "1":
                leaf = "unpredictable"
            else:
                leaf = "table"
            result.setdefault(name, []).append(
                (mask, value, tuple(constraints), leaf)
            )

        for node in hierarchy.findall("node"):
            visit(node, 0, 0, [], 0)
        return result

    def partitions(self, isa: str) -> list[Partition]:
        classes = self._class_predicates(isa)
        sections = {
            section.get("id", ""): section
            for section in self.roots[isa].findall("./iclass_sect")
        }
        result: list[Partition] = []
        # Hierarchy leaves that are plain UNALLOCATED or UNPREDICTABLE classes
        # have no table.
        for iclass, predicates in classes.items():
            for mask, value, constraints, leaf in predicates:
                if leaf != "table":
                    result.append(
                        Partition(isa, leaf, iclass, iclass, "none",
                                  mask, value, constraints, ())
                    )
                elif iclass not in sections:
                    raise CatalogError(f"decode class {iclass} has no table")
        for iclass, section in sections.items():
            if iclass not in classes:
                raise CatalogError(f"table {iclass} is not in the hierarchy")
            diagram = section.find("regdiagram")
            table = section.find("instructiontable")
            if diagram is None or table is None:
                raise CatalogError(f"malformed encoding-index table {iclass}")
            shift = _layout_shift(diagram.get("form", ""))
            sec_mask, sec_value, sec_fields, sec_constraints, _ = _boxes(
                diagram, shift
            )
            headings = table.findall(
                "./thead/tr[@id='heading2']/th[@class='bitfields']"
            )
            names = ["".join(item.itertext()).strip() for item in headings]
            for row in table.findall("./tbody/tr"):
                cells = row.findall("td[@class='bitfield']")
                if len(cells) != len(names):
                    raise CatalogError(
                        f"{iclass}: row has {len(cells)} fields, expected {len(names)}"
                    )
                row_mask, row_value = sec_mask, sec_value
                row_constraints = list(sec_constraints)
                for name, cell in zip(names, cells):
                    segments = _table_segments(iclass, name, sec_fields)
                    text = "".join(cell.itertext()).strip()
                    cell_mask, cell_value, constraint = _segment_cell(
                        segments, text
                    )
                    row_mask, row_value = _merge(
                        row_mask, row_value, cell_mask, cell_value
                    )
                    if constraint is not None and constraint not in row_constraints:
                        row_constraints.append(constraint)
                encname = row.get("encname", "")
                if not encname:
                    raise CatalogError(f"{iclass}: table row has no name")
                if encname in XML_NAME_ERRATA and isa == "A32":
                    encname = _corrected_row_name(
                        iclass, encname, row.get("label", "")
                    )
                filename = row.get("iformfile", "")
                if filename:
                    kind = "allocated"
                    if row.get("unpred") == "1":
                        kind = "unpredictable"
                    elif row.get("reserved_nop_hint") == "1":
                        kind = "nop-hint"
                elif row.get("undef") == "1":
                    kind = "unallocated"
                    filename = "none"
                elif row.get("unpred") == "1":
                    kind = "unpredictable"
                    filename = "none"
                elif row.get("reserved_nop_hint") == "1":
                    kind = "nop-hint"
                    filename = "none"
                else:
                    raise CatalogError(f"{iclass} {encname}: unclassified table row")
                feature = row.get("arch_version", "")
                for leaf_mask, leaf_value, leaf_constraints, _leaf in classes[iclass]:
                    mask, value = _merge(row_mask, row_value, leaf_mask, leaf_value)
                    result.append(
                        Partition(
                            isa,
                            kind,
                            iclass,
                            encname,
                            filename,
                            mask,
                            value,
                            tuple(dict.fromkeys(row_constraints + list(leaf_constraints))),
                            (feature,) if feature else (),
                        )
                    )
        return result
