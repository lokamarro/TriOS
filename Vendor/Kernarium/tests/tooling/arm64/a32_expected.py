#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""What the AArch32 decoder must answer for a word, computed independently.

The C++ decoder (darwin/cpu/arm64/aarch32/decode.cpp) is checked against this
module on the stored sample words. It reads the same table model, but the row
comes from the walked decision tree's Python mirror, the operands from an
interpreter of the Decode pseudocode (a32_asl_eval.py) rather than the C++
translation, and the profile and EL0 rules are written out again here, so an
error in either implementation shows as a disagreement.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from dataclasses import dataclass

import a32_asl as asl
import a32_asl_eval as evaluator
import a32_operand_codegen as codegen
import a32_profile_policy as policy
import a32_table_model as tm
import a32_table_render as render


FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3


@dataclass
class Expected:
    form: str
    status: str  # ok, nop or undefined
    reason: str  # none, unallocated, unpredictable, profile or feature
    sysreg: str
    cond: int
    digest: int


class Reference:
    def __init__(self, model: tm.Model, catalog):
        self.model = model
        cache = render.tables(model)
        self.trees, self.rows = cache["trees"], cache["rows"]
        self.parsed: dict[tuple[str, str], list] = {}
        self.catalog = catalog
        self.storage = {id(m): storage_type(m.cpp) for m in model.member_list}
        self.last: evaluator.Evaluator | None = None

    def program_ast(self, form: tm.FormModel):
        """The form's own Decode pseudocode, parsed: forms that share an
        operand program may differ in text, and each is evaluated from its
        own."""
        encoding = form.encoding
        key = (encoding.file, encoding.psname)
        if key not in self.parsed:
            text = self.catalog.decode_text(*key)
            self.parsed[key] = asl.parse(text, strict=True)
        return self.parsed[key]

    def digest(self, ev: evaluator.Evaluator) -> int:
        values: dict[int, int] = {}
        for name, ty in ev.types.items():
            member = self.model.members[(name, codegen.kind_key(ty))]
            bits, signed = self.storage[id(member)]
            raw = ev.locals[name]
            if isinstance(raw, bool):
                number = int(raw)
            elif isinstance(raw, evaluator.Bits):
                number = raw.value
            elif isinstance(raw, str):
                number = codegen.ENUM_VALUES[ty.name].index(raw)
            else:
                number = raw
            number &= (1 << bits) - 1
            values[id(member)] = number
        words = []
        for member in self.model.member_list:
            bits, signed = self.storage[id(member)]
            number = values.get(id(member), 0)
            if signed and number >> (bits - 1) & 1:
                number -= 1 << bits
            words.append(number & 0xFFFFFFFFFFFFFFFF)
        words.append(int(ev.carry_flags))
        return fnv(words)

    def decode(self, isa: str, word: int, it_state: int,
               features: set[str]) -> Expected:
        thumb = isa != "A32"
        row = render.walk_tree(self.trees, self.rows, isa, word)
        if row is None:
            return Expected("Undefined", "undefined", "unallocated", "none", 14, 0)
        if row.kind == "reserved_hint":
            if reserved_hint_violation(isa, word):
                return Expected("ReservedHint", "undefined", "unpredictable", "none", 14, 0)
            return Expected("ReservedHint", "nop", "none", "none", 14, 0)
        form = self.model.forms[row.form]
        result = Expected(form.name, "undefined", "none", "none", 14, 0)
        if row.kind == "unpredictable":
            result.reason = "unpredictable"
            return result
        if form.availability == "undefined_at_el0":
            result.reason = "profile"
            return result
        if form.feature is not None and form.feature not in features:
            result.reason = "feature"
            return result
        encoding = form.encoding
        if word & encoding.should_mask != encoding.should_value:
            result.reason = "unpredictable"
            return result
        fields = {f.name: (f.low, f.width) for f in encoding.fields}
        ev = evaluator.Evaluator(fields, thumb, it_state, features)
        self.last = ev
        ev.load(word, fields)
        try:
            ev.execute(self.program_ast(form))
        except evaluator.Outcome as outcome:
            mapped = {
                "nop": ("nop", "none"),
                "unpredictable": ("undefined", "unpredictable"),
                "feature": ("undefined", "feature"),
                "undefined": ("undefined", "unallocated"),
                "see": ("undefined", "unallocated"),
            }[outcome.kind]
            result.status, result.reason = mapped
            self.conditioned(result, form, word, it_state, thumb)
            return result
        sysreg = "none"
        if form.check == "coprocessor":
            sysreg = coprocessor_register(form.name, word)
            if sysreg == "none":
                result.reason = "profile"
                return result
        elif form.check == "apsr":
            if ev.locals.get("read_spsr") or ev.locals.get("write_spsr"):
                result.reason = "profile"
                return result
            if form.name.startswith("MSR") and msr_mask(fields, word) & 2:
                result.reason = "profile"  # the x field would write PSTATE.E
                return result
        elif form.check == "fpscr":
            if word >> 16 & 0xF != 1:
                result.reason = "profile"
                return result
        if (
            form.alu_exception_return
            and evaluator.to_int(ev.locals.get("d", 0)) == 15
            and ev.locals.get("setflags")
        ):
            result.reason = "unpredictable"
            return result
        result.status = "nop" if form.nop else "ok"
        result.reason = "none"
        result.sysreg = sysreg
        result.digest = self.digest(ev) if result.status == "ok" else 0
        self.conditioned(result, form, word, it_state, thumb)
        return result

    def operands(self, isa: str, word: int, it_state: int,
                 features: set[str]) -> tuple[Expected, dict[str, object]]:
        """The decode result and the Decode pseudocode's variables as Python
        values (integers, booleans and enumerator names)."""
        self.last = None
        result = self.decode(isa, word, it_state, features)
        values: dict[str, object] = {}
        if self.last is not None and result.status != "undefined":
            for name, raw in self.last.locals.items():
                if isinstance(raw, (bool, str)):
                    values[name] = raw
                elif isinstance(raw, evaluator.Bits):
                    values[name] = raw.value
                elif isinstance(raw, int):
                    values[name] = raw
        return result, values

    @staticmethod
    def conditioned(result: Expected, form: tm.FormModel, word: int,
                    it_state: int, thumb: bool) -> None:
        if result.status == "undefined" or not form.conditional:
            return
        if form.cond_low is not None:
            result.cond = word >> form.cond_low & 0xF
        elif thumb and it_state & 0xF:
            result.cond = it_state >> 4


def reserved_hint_violation(isa: str, word: int) -> bool:
    """The should-be bits of the hint spaces that the decode index leaves out: a
    reserved hint whose SBO or SBZ bits differ is UNPREDICTABLE."""
    if isa == "A32":
        return word >> 28 != 0xF and word >> 20 & 0xFF == 0x32 and word >> 12 & 0xF != 0xF
    if isa == "T32":
        return word >> 20 == 0xF3A and (
            word >> 16 & 0xF != 0xF or bool(word >> 13 & 1) or bool(word >> 8 & 0xF))
    return False


def msr_mask(fields: dict, word: int) -> int:
    low, width = fields["mask"]
    return word >> low & ((1 << width) - 1)


def coprocessor_register(form: str, word: int) -> str:
    """The register a permitted coprocessor access names."""
    if not word >> 8 & 1:
        return "none"  # coprocessor 14
    read = word >> 20 & 1
    opc1 = word >> 21 & 7
    crn = word >> 16 & 0xF
    crm = word & 0xF
    opc2 = word >> 5 & 7
    kind = form.split("_")[0]
    if kind == "MRC" and read and opc1 == 0 and crm == 0:
        return {(13, 3): "tpidruro", (13, 2): "tpidrurw_read", (14, 0): "cntfrq"}.get(
            (crn, opc2), "none"
        )
    if kind == "MCR" and not read and opc1 == 0:
        return {
            (13, 0, 2): "tpidrurw_write",
            (7, 10, 5): "cp15_dmb",
            (7, 10, 4): "cp15_dsb",
            (7, 5, 4): "cp15_isb",
        }.get((crn, crm, opc2), "none")
    if kind == "MRRC" and read and (word >> 4 & 0xF) == 1 and crm == 14:
        return "cntvct"
    return "none"


def storage_type(cpp: str) -> tuple[int, bool]:
    table = {
        "bool": (1, False),
        "std::int32_t": (32, True),
        "std::int64_t": (64, True),
        "std::uint8_t": (8, False),
        "std::uint16_t": (16, False),
        "std::uint32_t": (32, False),
        "std::uint64_t": (64, False),
    }
    return table.get(cpp, (8, False))  # enumerations are one byte


def fnv(words: list[int]) -> int:
    digest = FNV_OFFSET
    for word in words:
        for shift in range(0, 64, 8):
            digest = (digest ^ (word >> shift & 0xFF)) * FNV_PRIME & 0xFFFFFFFFFFFFFFFF
    return digest


def render_digest_header(model: tm.Model) -> str:
    """A test-support function hashing an Operands the same way."""
    lines = [
        "// SPDX-License-Identifier: GPL-2.0-or-later",
        render.GENERATED,
        "#pragma once",
        "",
        '#include "cpu/arm64/aarch32/a32_encodings.hpp"',
        "",
        "#include <cstdint>",
        "",
        f"namespace {render.NAMESPACE} {{",
        "",
        "/* FNV-1a over every Operands member, in order, widened to 64 bits. */",
        "[[nodiscard]] inline std::uint64_t operands_digest(const Operands &ops) noexcept",
        "{",
        "\tstd::uint64_t digest = UINT64_C(0xcbf29ce484222325);",
        "\tconst auto mix = [&digest](std::uint64_t word) {",
        "\t\tfor (unsigned shift = 0; shift < 64; shift += 8)",
        "\t\t\tdigest = (digest ^ (word >> shift & 0xFF)) * UINT64_C(0x100000001b3);",
        "\t};",
    ]
    for member in model.member_list:
        lines.append(f"\tmix(static_cast<std::uint64_t>(ops.{member.name}));")
    lines += [
        "\tmix(static_cast<std::uint64_t>(ops.carry_flags));",
        "\treturn digest;",
        "}",
        "",
        f"}} // namespace {render.NAMESPACE}",
        "",
    ]
    return "\n".join(lines)
