#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Translate the Decode pseudocode of an AArch32 encoding to C++.

Every encoding's Decode section computes the operands its execute
pseudocode uses: register numbers, expanded immediates, shift types, element
sizes, and the booleans such as wback and setflags. The virtual CPU's
engines and its semantic library all need those values and must not disagree
about them, so the decoder computes them once, from this translation of
Arm's own pseudocode, into the members of aarch32::Operands.

The translation is static: each ASL variable becomes a member of Operands, a
field of the instruction word becomes a shift and a mask, and the small set
of functions the Decode sections call (UInt, DecodeImmShift, A32ExpandImm and
so on) are the helpers of decode_support.hpp. The profile fixes the rest:

- FPSCR().Len and Stride are zero (short vectors are RAZ/WI), so a check
  that they are nonzero is dropped;
- IsFeatureImplemented(FEAT_X) is the caller's feature set for the features
  the profile can have, true for the ones the profile always has and false
  for the ones it lacks;
- an UNPREDICTABLE choice is UNDEFINED, and ConstrainUnpredictable returns
  Constraint_UNDEF;
- InITBlock() and LastInITBlock() read the IT state the caller passes; and
- PSTATE.C is a runtime flag, so an expanded immediate's carry-out is either
  a constant or "the flag itself" (Operands::carry_flags).

A construct this module does not know fails the generation.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
from dataclasses import dataclass, field

import a32_asl as asl
import a32_profile_policy as policy


class CodegenError(ValueError):
    pass


CPP_KEYWORDS = {
    "unsigned", "signed", "register", "default", "int", "long", "short",
    "double", "float", "char", "bool", "auto", "case", "class", "const",
    "delete", "export", "extern", "inline", "new", "not", "or", "and",
    "private", "public", "template", "this", "typename", "union", "using",
    "virtual", "volatile", "xor",
}
ENUM_VALUES = {
    "SRType": ("SRType_LSL", "SRType_LSR", "SRType_ASR", "SRType_ROR", "SRType_RRX"),
    "InstrSet": ("InstrSet_A32", "InstrSet_T32"),
    "VBitOps": ("VBitOps_VBIF", "VBitOps_VBIT", "VBitOps_VBSL"),
    "VFPNegMul": ("VFPNegMul_VNMLA", "VFPNegMul_VNMLS", "VFPNegMul_VNMUL"),
    "VCGEType": ("VCGEType_signed", "VCGEType_unsigned", "VCGEType_fp"),
    "VCGTtype": ("VCGTtype_signed", "VCGTtype_unsigned", "VCGTtype_fp"),
    "FPRounding": (
        "FPRounding_TIEEVEN", "FPRounding_POSINF", "FPRounding_NEGINF",
        "FPRounding_ZERO", "FPRounding_TIEAWAY", "FPRounding_ODD",
    ),
    "Constraint": ("Constraint_UNDEF", "Constraint_NOP"),
}
# What distinguishes same-named members of different enumerations.
ENUM_SUFFIX = {
    "SRType": "srtype",
    "InstrSet": "instrset",
    "VBitOps": "bitops",
    "VFPNegMul": "negmul",
    "VCGEType": "vcge",
    "VCGTtype": "vcgt",
    "FPRounding": "rounding",
}
ENUM_OF = {value: name for name, values in ENUM_VALUES.items() for value in values}


def enum_literal(value: str) -> str:
    """The C++ enumerator for an ASL enumeration constant."""
    name = ENUM_OF[value]
    return f"{name}::{sanitize(value[len(name) + 1:])}"
# The profile's feature set. Each XML feature maps to the aarch32::Feature
# the caller's profile carries, or to a constant when the profile always or
# never has it.
FEATURES = {**policy.FEATURES, **policy.PROGRAM_FEATURES}
UNIMPLEMENTED_FEATURES = policy.ABSENT_FEATURES


@dataclass(frozen=True)
class Ty:
    kind: str  # int, bool, bits, enum, tuple
    width: object = None  # bits: an int, a C++ expression string, or None
    name: str = ""  # enum
    items: tuple = ()  # tuple

    def is_bits(self) -> bool:
        return self.kind == "bits"


INT = Ty("int")
BOOL = Ty("bool")


def bits(width) -> Ty:
    return Ty("bits", width)


def enum(name: str) -> Ty:
    return Ty("enum", name=name)


def declared_type(node, width_of=None) -> Ty:
    """The Ty a declared ASL type names.

    width_of translates a non-constant bits(width) expression to a C++
    expression when a translator is at hand.
    """
    if node is None:
        raise CodegenError("declaration has no type")
    kind = node[0]
    if kind == "integer":
        return INT
    if kind == "bits":
        width = node[1]
        if width[0] == "num":
            return bits(width[1])
        if width_of is not None:
            return bits(width_of(width))
        return bits(None)
    if kind == "tuple":
        return Ty(
            "tuple",
            items=tuple(declared_type(item, width_of) for item in node[1]),
        )
    if kind == "name":
        name = node[1]
        if name == "integer":
            return INT
        if name == "boolean":
            return BOOL
        if name == "bit":
            return bits(1)
        if name in ENUM_VALUES:
            return enum(name)
    raise CodegenError(f"unsupported declared type {node!r}")


def kind_key(ty: Ty) -> str:
    """The kind a member is keyed by: enumerations by their own type."""
    return f"enum:{ty.name}" if ty.kind == "enum" else ty.kind


@dataclass
class Member:
    """One Operands member: the ASL variables of one name and kind."""

    name: str
    ty: Ty
    cpp: str  # C++ type
    uses: int = 0

    def bitfield(self) -> bool:
        return self.ty.kind == "bool"


def member_cpp_type(types: set[Ty]) -> str:
    kinds = {ty.kind for ty in types}
    if kinds == {"bool"}:
        return "bool"
    if kinds == {"int"}:
        return "std::int32_t"
    if kinds == {"enum"}:
        (ty,) = types
        return ty.name
    if kinds <= {"bits", "int"}:
        widths = {ty.width for ty in types if ty.kind == "bits"}
        if "int" in kinds:
            return "std::int64_t"
        if None in widths or any(width > 32 for width in widths):
            return "std::uint64_t"
        return "std::uint8_t" if max(widths) <= 8 else (
            "std::uint16_t" if max(widths) <= 16 else "std::uint32_t"
        )
    raise CodegenError(f"cannot store kinds {sorted(kinds)}")


class Members:
    """The Operands members, decided over every Decode section."""

    def __init__(self) -> None:
        self.types: dict[str, set[Ty]] = {}

    def declare(self, name: str, ty: Ty) -> None:
        self.types.setdefault(name, set()).add(ty)

    def resolve(self) -> dict[tuple[str, str], Member]:
        """(ASL name, kind) -> Member, naming collisions deterministically."""
        result: dict[tuple[str, str], Member] = {}
        for name in sorted(self.types):
            by_kind: dict[str, set[Ty]] = {}
            for ty in self.types[name]:
                group = "bits" if ty.kind == "int" else ty.kind
                if ty.kind == "enum":
                    group = f"enum:{ty.name}"
                by_kind.setdefault(group, set()).add(ty)
            # bits and int merge into one wider integer member; other kinds of
            # one name get distinct members.
            if "bits" in by_kind and any(t.kind == "int" for t in self.types[name]):
                pass
            groups = sorted(by_kind)
            for position, group in enumerate(groups):
                types = by_kind[group]
                member_name = sanitize(name)
                if len(groups) > 1:
                    if group == "bool":
                        suffix = ""
                    elif group.startswith("enum:"):
                        suffix = "_" + ENUM_SUFFIX[group[5:]]
                    else:
                        suffix = "_value" if "bool" in groups else "_alt"
                    member_name += suffix
                member = Member(member_name, next(iter(types)), member_cpp_type(types))
                for ty in types:
                    result[(name, kind_key(ty))] = member
        return result


def sanitize(name: str) -> str:
    return name + "_" if name in CPP_KEYWORDS else name


@dataclass
class Translation:
    """C++ for one Decode section and the fields it reads."""

    body: list[str]
    fields_used: set[str] = field(default_factory=set)


@dataclass
class Value:
    cpp: str
    ty: Ty
    const: object = None  # a Python value when known at generation time


def unparen(text: str) -> str:
    """text without one pair of parentheses that wraps all of it."""
    if not (text.startswith("(") and text.endswith(")")):
        return text
    depth = 0
    for position, char in enumerate(text):
        depth += (char == "(") - (char == ")")
        if depth == 0 and position != len(text) - 1:
            return text
    return text[1:-1]


def literal(value) -> str:
    return f"{value}"


def mask_cpp(width) -> str:
    if isinstance(width, int):
        return f"UINT64_C({(1 << width) - 1})" if width < 64 else "~UINT64_C(0)"
    return f"((UINT64_C(1) << ({width})) - 1)"


class Translator:
    def __init__(self, fields: dict[str, tuple[int, int]], members: dict, label: str):
        self.fields = fields
        self.members = members
        self.label = label
        self.out = Translation(body=[])
        self.locals: dict[str, Value] = {}
        self.cpp_types: dict[str, str] = {}
        self.indent = 1

    def error(self, message: str):
        raise CodegenError(f"{self.label}: {message}")

    # Output --------------------------------------------------------------

    def emit(self, text: str) -> None:
        self.out.body.append("\t" * self.indent + text)

    # Names ---------------------------------------------------------------

    def member_for(self, name: str, ty: Ty) -> Member:
        kind = kind_key(ty)
        member = self.members.get((name, kind))
        if member is None and kind == "bits":
            member = self.members.get((name, "int"))
        if member is None:
            self.error(f"variable {name!r} of kind {kind} has no member")
        return member

    def name(self, name: str) -> Value:
        if name in self.locals:
            return self.locals[name]
        if name in self.fields:
            low, width = self.fields[name]
            self.out.fields_used.add(name)
            return Value(f"f_{name}", bits(width))
        if name in ENUM_OF:
            return Value(enum_literal(name), enum(ENUM_OF[name]), name)
        if name in ("EL2", "EL3"):
            return Value("0", INT)
        self.error(f"unknown name {name!r}")

    # Expressions ---------------------------------------------------------

    def expr(self, node, hint=None) -> Value:
        kind = node[0]
        if kind == "num":
            return Value(f"INT64_C({node[1]})", INT, node[1])
        if kind == "bool":
            return Value("true" if node[1] else "false", BOOL, node[1])
        if kind == "bits":
            return Value(f"UINT64_C({node[2]})", bits(node[1]), node[2])
        if kind == "name":
            return self.name(node[1])
        if kind == "unary":
            return self.unary(node)
        if kind == "binary":
            return self.binary(node, hint)
        if kind == "in":
            return self.membership(node)
        if kind == "cond":
            test = self.expr(node[1])
            if test.ty != BOOL:
                self.error("if-expression test is not boolean")
            then = self.expr(node[2], hint)
            other = self.expr(node[3], hint)
            ty = then.ty
            if then.ty != other.ty:
                if {then.ty.kind, other.ty.kind} <= {"int", "bits"}:
                    ty = INT
                else:
                    self.error("if-expression arms differ in type")
            return Value(f"({test.cpp} ? {then.cpp} : {other.cpp})", ty)
        if kind == "slice":
            return self.slice(node)
        if kind == "call":
            return self.call(node, hint)
        if kind == "member":
            return self.member(node)
        if kind == "tuple":
            return Value("", Ty("tuple", items=tuple(self.expr(item).ty for item in node[1])))
        if kind == "arbitrary":
            ty = declared_type(node[1])
            if ty.kind == "enum":
                return Value(enum_literal(ENUM_VALUES[ty.name][0]), ty)
            if ty == BOOL:
                return Value("false", ty, False)
            return Value("0", ty, 0)
        self.error(f"unsupported expression {kind}")

    def member(self, node) -> Value:
        base, name = node[1], node[2]
        if base[0] == "call" and base[1] == "FPSCR" and name == "Len":
            return Value("UINT64_C(0)", bits(3), 0)
        if base[0] == "call" and base[1] == "FPSCR" and name == "Stride":
            return Value("UINT64_C(0)", bits(2), 0)
        if base[0] == "name" and base[1] == "PSTATE" and name == "C":
            return Value("<PSTATE.C>", bits(1))
        self.error(f"unsupported member {name!r}")

    def unary(self, node) -> Value:
        op = node[1]
        operand = self.expr(node[2])
        if op == "!":
            if operand.ty != BOOL:
                self.error("! of a non-boolean")
            const = None if operand.const is None else (not operand.const)
            return Value(f"(!{operand.cpp})", BOOL, const)
        if op == "-":
            if operand.ty != INT:
                self.error("negation of a non-integer")
            const = None if operand.const is None else -operand.const
            return Value(f"(-{operand.cpp})", INT, const)
        if op == "NOT":
            if not operand.ty.is_bits() or operand.ty.width is None:
                self.error("NOT of a value without a static width")
            const = None
            if operand.const is not None:
                const = ~operand.const & ((1 << operand.ty.width) - 1)
            return Value(
                f"(~{operand.cpp} & {mask_cpp(operand.ty.width)})", operand.ty, const
            )
        self.error(f"unsupported unary {op}")

    def binary(self, node, hint) -> Value:
        op = node[1]
        if op in ("&&", "||"):
            left = self.expr(node[2])
            right = self.expr(node[3])
            if left.ty != BOOL or right.ty != BOOL:
                self.error(f"{op} of non-booleans")
            if op == "&&":
                if left.const is False or right.const is False:
                    return Value("false", BOOL, False)
                if left.const is True:
                    return right
                if right.const is True:
                    return left
                return Value(f"({left.cpp} && {right.cpp})", BOOL)
            if left.const is True or right.const is True:
                return Value("true", BOOL, True)
            if left.const is False:
                return right
            if right.const is False:
                return left
            return Value(f"({left.cpp} || {right.cpp})", BOOL)
        left = self.expr(node[2], hint)
        right = self.expr(node[3], hint)
        if op == "::":
            return self.concat(left, right)
        if op in ("==", "!="):
            return self.equality(op, node, left, right)
        if op in ("<", "<=", ">", ">="):
            return self.relational(op, left, right)
        if op in ("AND", "OR", "EOR", "XOR"):
            symbol = {"AND": "&", "OR": "|", "EOR": "^", "XOR": "^"}[op]
            if not (left.ty.is_bits() and right.ty.is_bits()):
                self.error(f"{op} of non-bits")
            return Value(f"({left.cpp} {symbol} {right.cpp})", left.ty)
        if op in ("+", "-", "*", "DIV", "MOD", "DIVRM", "<<", ">>", "/", "^"):
            ints = self.as_int(left), self.as_int(right)
            symbol = {"DIV": "/", "DIVRM": "/", "MOD": "%", "^": "<<"}.get(op, op)
            if op == "^":
                if left.const != 2:
                    self.error("only 2^n is supported")
                text = f"(INT64_C(1) << {ints[1]})"
            else:
                text = f"({ints[0]} {symbol} {ints[1]})"
            const = None
            if left.const is not None and right.const is not None:
                a, b = left.const, right.const
                const = {
                    "+": a + b, "-": a - b, "*": a * b,
                    "DIV": a // b if b else None, "DIVRM": a // b if b else None,
                    "MOD": a % b if b else None, "<<": a << b, ">>": a >> b,
                    "^": 1 << b,
                }.get(op)
            return Value(text, INT, const)
        self.error(f"unsupported operator {op}")

    def as_int(self, value: Value) -> str:
        if value.ty == INT:
            return value.cpp
        if value.ty.is_bits():
            return f"static_cast<int64_t>({value.cpp})"
        self.error(f"expected an integer, found {value.ty.kind}")

    def concat(self, left: Value, right: Value) -> Value:
        if not (left.ty.is_bits() and right.ty.is_bits()):
            self.error(":: of non-bits")
        lw, rw = left.ty.width, right.ty.width
        if lw is None or rw is None:
            self.error(":: of a value without a width")
        const = None
        if left.const is not None and right.const is not None and isinstance(rw, int):
            const = left.const << rw | right.const
        if isinstance(lw, int) and isinstance(rw, int):
            width = lw + rw
        else:
            width = f"({lw} + {rw})"
        return Value(f"(({left.cpp} << {rw}) | {right.cpp})", bits(width), const)

    def equality(self, op, node, left: Value, right: Value) -> Value:
        negate = op == "!="
        # A bit-string literal with 'x' bits is a pattern.
        for pattern_node, other in ((node[3], left), (node[2], right)):
            if pattern_node[0] == "bits" and pattern_node[3]:
                if not other.ty.is_bits():
                    self.error("pattern compared with a non-bits value")
                wild = pattern_node[3]
                value = pattern_node[2]
                keep = ((1 << pattern_node[1]) - 1) & ~wild
                text = f"(({other.cpp} & UINT64_C({keep})) == UINT64_C({value}))"
                return Value(f"(!{text})" if negate else text, BOOL)
        if left.ty.kind != right.ty.kind and not (
            {left.ty.kind, right.ty.kind} <= {"int", "bits"}
        ):
            self.error(f"comparison of {left.ty.kind} with {right.ty.kind}")
        const = None
        if left.const is not None and right.const is not None:
            const = (left.const == right.const) != negate
            if isinstance(const, bool):
                return Value("true" if const else "false", BOOL, const)
        symbol = "!=" if negate else "=="
        return Value(f"({left.cpp} {symbol} {right.cpp})", BOOL)

    def relational(self, op, left: Value, right: Value) -> Value:
        a, b = self.as_int(left), self.as_int(right)
        const = None
        if left.const is not None and right.const is not None:
            const = {
                "<": left.const < right.const, "<=": left.const <= right.const,
                ">": left.const > right.const, ">=": left.const >= right.const,
            }[op]
            return Value("true" if const else "false", BOOL, const)
        return Value(f"({a} {op} {b})", BOOL)

    def membership(self, node) -> Value:
        value = self.expr(node[1])
        terms: list[str] = []
        constants: list = []
        for item in node[2]:
            if item[0] == "bits" and item[3]:
                keep = ((1 << item[1]) - 1) & ~item[3]
                terms.append(f"(({value.cpp} & UINT64_C({keep})) == UINT64_C({item[2]}))")
                continue
            other = self.expr(item)
            if value.const is not None and other.const is not None:
                constants.append(value.const == other.const)
            terms.append(f"({value.cpp} == {other.cpp})")
        if constants and len(constants) == len(node[2]):
            const = any(constants)
            return Value("true" if const else "false", BOOL, const)
        return Value("(" + " || ".join(terms) + ")", BOOL)

    def slice(self, node) -> Value:
        base = self.expr(node[1])
        if not base.ty.is_bits() and base.ty != INT:
            self.error("slice of a non-bits value")
        pieces = [self.slice_part(base, high, low) for high, low in node[2]]
        result = pieces[0]
        for piece in pieces[1:]:
            result = self.concat(result, piece)
        return result

    def slice_part(self, base: Value, high, low) -> Value:
        if low is None:
            self.error("indexed slices are not supported in Decode")
        if high is low:
            index = self.expr(high)
            text = f"(({base.cpp} >> {index.cpp}) & UINT64_C(1))"
            const = None
            if base.const is not None and index.const is not None:
                const = (base.const >> index.const) & 1
            return Value(text, bits(1), const)
        hi, lo = self.expr(high), self.expr(low)
        if hi.const is not None and lo.const is not None:
            width = hi.const - lo.const + 1
            const = None
            if base.const is not None:
                const = (base.const >> lo.const) & ((1 << width) - 1)
            return Value(
                f"(({base.cpp} >> {lo.const}) & {mask_cpp(width)})", bits(width), const
            )
        width = f"({hi.cpp} - {lo.cpp} + 1)"
        return Value(
            f"(({base.cpp} >> {lo.cpp}) & {mask_cpp(width)})", bits(width)
        )

    # Calls ---------------------------------------------------------------

    def call(self, node, hint) -> Value:
        name, args, params = node[1], node[2], node[3]
        if name == "UInt":
            (arg,) = args
            value = self.expr(arg)
            return Value(f"static_cast<int64_t>({value.cpp})", INT, value.const)
        if name == "SInt":
            (arg,) = args
            value = self.expr(arg)
            if value.ty.width is None:
                self.error("SInt of a value without a static width")
            return Value(f"detail::sint({value.cpp}, {value.ty.width})", INT)
        if name in ("ZeroExtend", "Zeros"):
            width = hint.width if hint is not None and hint.is_bits() else None
            if params:
                param = self.expr(params[0])
                width = param.const if param.const is not None else width
            if name == "Zeros":
                if args:
                    width = self.expr(args[0]).const
                return Value("UINT64_C(0)", bits(width), 0)
            value = self.expr(args[0])
            if len(args) > 1:
                width = self.expr(args[1]).const
            return Value(value.cpp, bits(width), value.const)
        if name == "SignExtend":
            width = hint.width if hint is not None and hint.is_bits() else None
            if params:
                width = self.expr(params[0]).const
            value = self.expr(args[0])
            if len(args) > 1:
                width = self.expr(args[1]).const
            if width is None or value.ty.width is None:
                self.error("SignExtend without static widths")
            return Value(
                f"(static_cast<uint64_t>(detail::sint({value.cpp}, {value.ty.width}))"
                f" & {mask_cpp(width)})",
                bits(width),
            )
        if name == "IsZero":
            value = self.expr(args[0])
            return Value(f"({value.cpp} == 0)", BOOL)
        if name == "BitCount":
            value = self.expr(args[0])
            return Value(f"static_cast<int64_t>(std::popcount({value.cpp}))", INT)
        if name == "HighestSetBitNZ":
            value = self.expr(args[0])
            return Value(
                f"static_cast<int64_t>(63 - std::countl_zero({value.cpp}))", INT
            )
        if name == "LowestSetBit":
            value = self.expr(args[0])
            width = value.ty.width
            if width is None:
                self.error("LowestSetBit of a value without a static width")
            return Value(
                f"detail::lowest_set_bit({value.cpp}, {width})", INT
            )
        if name == "InITBlock":
            return Value("ctx.it.active()", BOOL)
        if name == "LastInITBlock":
            return Value("ctx.it.last()", BOOL)
        if name == "CurrentInstrSet":
            return Value(
                "(ctx.thumb ? InstrSet::T32 : InstrSet::A32)", enum("InstrSet")
            )
        if name == "IsFeatureImplemented":
            feature = node[2][0][1]
            return self.feature(feature)
        if name == "ConstrainUnpredictable":
            return Value("Constraint::UNDEF", enum("Constraint"), "Constraint_UNDEF")
        if name == "DecodeImmShift":
            kind, amount = (self.expr(arg) for arg in args)
            return Value(
                f"detail::decode_imm_shift({kind.cpp}, {amount.cpp})",
                Ty("tuple", items=(enum("SRType"), INT)),
            )
        if name == "DecodeRegShift":
            (arg,) = args
            return Value(
                f"detail::decode_reg_shift({self.expr(arg).cpp})", enum("SRType")
            )
        if name in ("A32ExpandImm", "T32ExpandImm"):
            (arg,) = args
            helper = "a32_expand_imm" if name[0] == "A" else "t32_expand_imm"
            return Value(f"detail::{helper}({self.expr(arg).cpp})", bits(32))
        if name in ("A32ExpandImm_C", "T32ExpandImm_C"):
            arg, carry = args
            helper = "a32_expand_imm_c" if name[0] == "A" else "t32_expand_imm_c"
            if self.expr(carry).cpp != "<PSTATE.C>":
                self.error("ExpandImm_C with a carry other than PSTATE.C")
            return Value(
                f"detail::{helper}({self.expr(arg).cpp})",
                Ty("tuple", items=(bits(32), bits(1))),
            )
        if name == "AdvSIMDExpandImm":
            op, cmode, imm8 = (self.expr(arg) for arg in args)
            return Value(
                f"detail::advsimd_expand_imm({op.cpp}, {cmode.cpp}, {imm8.cpp})",
                bits(64),
            )
        if name == "VFPExpandImm":
            imm8 = self.expr(args[0])
            width = self.expr(params[0]).const if params else None
            if width is None:
                width = hint.width if hint is not None and hint.is_bits() else None
            if width is None:
                self.error("VFPExpandImm without a width")
            return Value(f"detail::vfp_expand_imm({imm8.cpp}, {width})", bits(width))
        if name == "FPDecodeRM":
            (arg,) = args
            return Value(
                f"detail::fp_decode_rm({self.expr(arg).cpp})", enum("FPRounding")
            )
        self.error(f"unsupported function {name!r}")

    def feature(self, feature: str) -> Value:
        if feature in FEATURES:
            return Value(f"ctx.features.has(Feature::{FEATURES[feature]})", BOOL)
        if feature in UNIMPLEMENTED_FEATURES:
            return Value("false", BOOL, False)
        self.error(f"unknown feature {feature}")

    # Statements ----------------------------------------------------------

    def statements(self, statements: list) -> None:
        for statement in statements:
            self.statement(statement)

    def statement(self, statement) -> None:
        kind = statement[0]
        if kind == "let":
            self.binding(statement[1], statement[2], statement[3])
        elif kind == "var":
            names, type_, value = statement[1], statement[2], statement[3]
            for name in names:
                ty = self.declared(type_) if len(names) == 1 else None
                if ty is not None:
                    self.declare(name, ty)
            if value is not None:
                self.binding(names, type_, value)
        elif kind == "assign":
            self.assign(statement[1], statement[2])
        elif kind == "if":
            self.conditional(statement)
        elif kind == "case":
            self.case(statement)
        elif kind == "call-statement":
            self.procedure(statement[1])
        elif kind == "assert":
            return
        else:
            self.error(f"unsupported statement {kind}")

    def declared(self, type_) -> Ty:
        return declared_type(type_, lambda width: self.expr(width).cpp)

    def declare(self, name: str, ty: Ty) -> None:
        if name in self.fields:
            self.error(f"variable {name!r} shadows a field")
        member = self.member_for(name, ty)
        self.locals[name] = Value(f"out.{member.name}", ty)
        self.cpp_types[f"out.{member.name}"] = member.cpp

    def binding(self, names: list[str], type_, value_node) -> None:
        if len(names) == 1:
            ty = self.declared(type_)
            value = self.expr(value_node, ty)
            if ty == enum("Constraint"):
                # ConstrainUnpredictable resolves to UNDEF at generation time.
                self.locals[names[0]] = value
                return
            self.declare(names[0], ty)
            target = self.locals[names[0]]
            self.store(target, value, ty)
            return
        tys = self.declared(type_).items
        value = self.expr(value_node, tys[0] if tys else None)
        if value.ty.kind != "tuple" or len(value.ty.items) != len(names):
            self.error(f"tuple binding of {names} from {value.ty}")
        temporary = f"t{len(self.out.body)}"
        self.emit(f"const auto {temporary} = {value.cpp};")
        parts = self.tuple_parts(value, temporary)
        for name, ty, (part, part_ty) in zip(names, tys, zip(parts, value.ty.items)):
            self.declare(name, ty)
            self.store(self.locals[name], Value(part, part_ty), ty)

    def tuple_parts(self, value: Value, temporary: str) -> list[str]:
        if "decode_imm_shift" in value.cpp:
            return [f"{temporary}.type", f"{temporary}.amount"]
        if "expand_imm_c" in value.cpp:
            # A carry-out of "the flag itself" (rotation zero) is recorded
            # apart from the constant carry-out of a rotated immediate.
            self.emit(f"out.carry_flags = {temporary}.carry_in;")
            return [f"{temporary}.imm32", f"{temporary}.carry"]
        self.error("tuple helper has no part names")

    def store(self, target: Value, value: Value, ty: Ty) -> None:
        cpp = value.cpp
        if cpp == "<PSTATE.C>":
            self.emit(f"{target.cpp} = 0;")
            self.emit("out.carry_flags = true;")
            return
        if ty.kind == "bits" and value.ty.kind == "bits":
            pass
        elif ty.kind != value.ty.kind and not (
            {ty.kind, value.ty.kind} <= {"int", "bits"}
        ):
            self.error(f"assigning {value.ty.kind} to {ty.kind}")
        if ty.kind == "bool":
            self.emit(f"{target.cpp} = {cpp};")
        elif ty.kind == "enum":
            self.emit(f"{target.cpp} = {cpp};")
        else:
            self.emit(f"{target.cpp} = static_cast<{self.cpp_types[target.cpp]}>({cpp});")

    def assign(self, target_node, value_node) -> None:
        if target_node[0] != "name":
            self.error("assignment to a non-variable")
        name = target_node[1]
        if name not in self.locals:
            self.error(f"assignment to undeclared {name!r}")
        target = self.locals[name]
        ty = self.local_type(name)
        value = self.expr(value_node, ty)
        self.store(target, value, ty)

    def local_type(self, name: str) -> Ty:
        return self.locals[name].ty

    def condition_outcome(self, test_node) -> str:
        for sub in asl.walk_expressions(test_node):
            if sub[0] == "call" and sub[1] == "IsFeatureImplemented":
                return "feature"
        return "undefined"

    def conditional(self, statement) -> None:
        arms, other = statement[1], statement[2]
        first = True
        # A guard whose body is just an outcome is a plain return.
        if len(arms) == 1 and not other:
            test_node, body = arms[0]
            outcome = self.outcome_body(test_node, body)
            if outcome is not None:
                test = self.expr(test_node)
                if test.ty != BOOL:
                    self.error("if test is not boolean")
                if test.const is False:
                    return
                if test.const is True:
                    self.emit(f"return Outcome::{outcome};")
                else:
                    self.emit(f"if ({unparen(test.cpp)})")
                    self.emit(f"\treturn Outcome::{outcome};")
                return
        for test_node, body in arms:
            test = self.expr(test_node)
            if test.ty != BOOL:
                self.error("if test is not boolean")
            self.emit(("if" if first else "else if") + f" ({unparen(test.cpp)}) {{")
            first = False
            self.indent += 1
            self.statements(body)
            self.indent -= 1
            self.emit("}")
        if other:
            self.emit("else {")
            self.indent += 1
            self.statements(other)
            self.indent -= 1
            self.emit("}")

    def outcome_body(self, test_node, body) -> str | None:
        if len(body) != 1 or body[0][0] != "call-statement":
            return None
        node = body[0][1]
        if node[0] != "call":
            return None
        mapping = {
            "Undefined": None,
            "UnpredictableProcedure": "unpredictable",
            "ExecuteAsNOP": "nop",
            "See": "see",
        }
        if node[1] not in mapping:
            return None
        if node[1] == "Undefined":
            return self.condition_outcome(test_node)
        return mapping[node[1]]

    def procedure(self, node) -> None:
        if node[0] != "call":
            self.error("statement is not a call")
        name = node[1]
        if name == "Undefined":
            self.emit("return Outcome::undefined;")
        elif name == "UnpredictableProcedure":
            self.emit("return Outcome::unpredictable;")
        elif name == "ExecuteAsNOP":
            self.emit("return Outcome::nop;")
        elif name == "See":
            self.emit("return Outcome::see;")
        else:
            self.error(f"unsupported procedure {name!r}")

    def case(self, statement) -> None:
        selector = self.expr(statement[1])
        arms, other = statement[2], statement[3]
        if selector.const is not None:
            for patterns, body in arms:
                for pattern in patterns:
                    value = self.expr(pattern)
                    if value.const == selector.const:
                        self.statements(body)
                        return
            if other:
                self.statements(other)
            return
        self.emit("switch (" + selector.cpp + ") {")
        for patterns, body in arms:
            labels = []
            for pattern in patterns:
                if pattern[0] == "bits" and pattern[3]:
                    self.error("case patterns with wildcards are not supported")
                value = self.expr(pattern)
                labels.append(f"case {value.cpp}:")
            self.emit(" ".join(labels) + " {")
            self.indent += 1
            self.statements(body)
            self.emit("break;")
            self.indent -= 1
            self.emit("}")
        if other:
            self.emit("default: {")
            self.indent += 1
            self.statements(other)
            self.emit("break;")
            self.indent -= 1
            self.emit("}")
        self.emit("}")


def collect_declarations(statements: list, members: Members, label: str) -> None:
    """Record the type of every variable a Decode section declares."""
    for statement in statements:
        kind = statement[0]
        if kind in ("let", "var"):
            names, type_ = statement[1], statement[2]
            if len(names) == 1:
                ty = declared_type(type_)
                if ty != enum("Constraint"):
                    members.declare(names[0], ty)
            else:
                for name, ty in zip(names, declared_type(type_).items):
                    members.declare(name, ty)
        elif kind == "if":
            for _test, body in statement[1]:
                collect_declarations(body, members, label)
            collect_declarations(statement[2], members, label)
        elif kind == "case":
            for _patterns, body in statement[2]:
                collect_declarations(body, members, label)
            if statement[3]:
                collect_declarations(statement[3], members, label)
