#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Evaluate AArch32 Decode pseudocode on concrete words.

The decoder's operand programs are C++ translated from the XML's Decode
pseudocode (a32_operand_codegen.py). This module is the reference they are
tested against: it interprets the same parsed pseudocode directly, on Python
values, so a mistake in the translation (a width, a slice, a precedence) shows
as a disagreement between the two on the stored sample words. It shares the
parser with the translator and nothing else; the semantics below are written
from the ASL definitions.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from dataclasses import dataclass

import a32_operand_codegen as codegen


class Outcome(Exception):
    def __init__(self, kind: str):
        super().__init__(kind)
        self.kind = kind


class EvalError(ValueError):
    pass


@dataclass(frozen=True)
class Bits:
    width: int
    value: int

    def __post_init__(self):
        if self.value < 0 or self.value >> self.width:
            raise EvalError(f"bits({self.width}) holds {self.value}")


@dataclass(frozen=True)
class Carry:
    """A carry-out: the constant bit, or the carry flag itself."""

    value: int
    from_flags: bool


def mask(width: int) -> int:
    return (1 << width) - 1


def to_int(value) -> int:
    if isinstance(value, Bits):
        return value.value
    if isinstance(value, Carry):
        return value.value
    if isinstance(value, bool):
        raise EvalError("boolean used as an integer")
    if isinstance(value, int):
        return value
    raise EvalError(f"{value!r} is not an integer")


def rotate_right(value: int, amount: int, width: int = 32) -> int:
    amount %= width
    return ((value >> amount) | (value << (width - amount))) & mask(width)


class Evaluator:
    def __init__(self, fields, thumb: bool, it_state: int, features: set[str]):
        self.fields = {
            name: Bits(width, 0) for name, (_low, width) in fields.items()
        }
        self.thumb = thumb
        self.it_active = (it_state & 0xF) != 0
        self.it_last = (it_state & 0xF) == 8
        self.features = features
        self.locals: dict[str, object] = {}
        self.types: dict[str, codegen.Ty] = {}
        self.carry_flags = False

    def load(self, word: int, fields) -> None:
        for name, (low, width) in fields.items():
            self.fields[name] = Bits(width, word >> low & mask(width))

    # Expressions -----------------------------------------------------------

    def expr(self, node, hint=None):
        kind = node[0]
        if kind == "num":
            return node[1]
        if kind == "bool":
            return node[1]
        if kind == "bits":
            if node[3]:
                raise EvalError("pattern used as a value")
            return Bits(node[1], node[2])
        if kind == "name":
            return self.name(node[1])
        if kind == "unary":
            return self.unary(node)
        if kind == "binary":
            return self.binary(node, hint)
        if kind == "in":
            return self.membership(node)
        if kind == "cond":
            return self.expr(node[2], hint) if self.expr(node[1]) else self.expr(node[3], hint)
        if kind == "slice":
            return self.slice(node)
        if kind == "call":
            return self.call(node, hint)
        if kind == "member":
            return self.member(node)
        if kind == "tuple":
            return tuple(self.expr(item) for item in node[1])
        if kind == "arbitrary":
            ty = codegen.declared_type(node[1])
            if ty.kind == "enum":
                return codegen.ENUM_VALUES[ty.name][0]
            if ty.kind == "bool":
                return False
            if ty.kind == "bits":
                return Bits(ty.width, 0)
            return 0
        raise EvalError(f"unsupported expression {kind}")

    def name(self, name: str):
        if name in self.locals:
            return self.locals[name]
        if name in self.fields:
            return self.fields[name]
        if name in codegen.ENUM_OF:
            return name
        if name in ("EL2", "EL3"):
            return 0
        raise EvalError(f"unknown name {name}")

    def member(self, node):
        base, name = node[1], node[2]
        if base[0] == "call" and base[1] == "FPSCR" and name in ("Len", "Stride"):
            return Bits(3 if name == "Len" else 2, 0)
        if base[0] == "name" and base[1] == "PSTATE" and name == "C":
            return Carry(0, True)
        raise EvalError(f"unsupported member {name}")

    def unary(self, node):
        op, operand = node[1], self.expr(node[2])
        if op == "!":
            return not operand
        if op == "-":
            return -to_int(operand)
        if op == "NOT":
            return Bits(operand.width, ~operand.value & mask(operand.width))
        raise EvalError(f"unsupported unary {op}")

    def equal(self, left, right) -> bool:
        if isinstance(left, (Bits, Carry, int)) and not isinstance(left, bool):
            return to_int(left) == to_int(right)
        return left == right

    def pattern_matches(self, value, item_node) -> bool:
        if item_node[0] == "bits" and item_node[3]:
            return to_int(value) & ~item_node[3] == item_node[2] & ~item_node[3]
        return self.equal(value, self.expr(item_node))

    def binary(self, node, hint):
        op = node[1]
        if op == "&&":
            return bool(self.expr(node[2])) and bool(self.expr(node[3]))
        if op == "||":
            return bool(self.expr(node[2])) or bool(self.expr(node[3]))
        if op in ("==", "!="):
            left_node, right_node = node[2], node[3]
            for pattern, other in ((right_node, left_node), (left_node, right_node)):
                if pattern[0] == "bits" and pattern[3]:
                    result = self.pattern_matches(self.expr(other), pattern)
                    return result if op == "==" else not result
            result = self.equal(self.expr(left_node), self.expr(right_node))
            return result if op == "==" else not result
        left, right = self.expr(node[2], hint), self.expr(node[3], hint)
        if op == "::":
            return Bits(left.width + right.width, left.value << right.width | right.value)
        if op in ("AND", "OR", "EOR", "XOR"):
            fn = {"AND": int.__and__, "OR": int.__or__, "EOR": int.__xor__, "XOR": int.__xor__}[op]
            return Bits(left.width, fn(left.value, right.value))
        a, b = to_int(left), to_int(right)
        if op == "+":
            return a + b
        if op == "-":
            return a - b
        if op == "*":
            return a * b
        if op in ("DIV", "DIVRM"):
            return a // b
        if op == "MOD":
            return a % b
        if op == "<<":
            return a << b
        if op == ">>":
            return a >> b
        if op == "^":
            if a != 2:
                raise EvalError("only 2^n is supported")
            return 1 << b
        if op == "<":
            return a < b
        if op == "<=":
            return a <= b
        if op == ">":
            return a > b
        if op == ">=":
            return a >= b
        raise EvalError(f"unsupported operator {op}")

    def membership(self, node) -> bool:
        value = self.expr(node[1])
        return any(self.pattern_matches(value, item) for item in node[2])

    def slice(self, node):
        base = self.expr(node[1])
        pieces = []
        for high_node, low_node in node[2]:
            if low_node is None:
                raise EvalError("indexed slices are not supported in Decode")
            high = to_int(self.expr(high_node))
            low = to_int(self.expr(low_node))
            width = high - low + 1
            pieces.append(Bits(width, to_int(base) >> low & mask(width)))
        result = pieces[0]
        for piece in pieces[1:]:
            result = Bits(result.width + piece.width, result.value << piece.width | piece.value)
        return result

    # Calls -----------------------------------------------------------------

    def call(self, node, hint):
        name, args, params = node[1], node[2], node[3]
        if name == "UInt":
            return to_int(self.expr(args[0]))
        if name == "SInt":
            value = self.expr(args[0])
            sign = 1 << (value.width - 1)
            return (value.value ^ sign) - sign
        if name in ("ZeroExtend", "SignExtend", "Zeros"):
            width = None
            if params:
                width = to_int(self.expr(params[0]))
            if name == "Zeros":
                if args:
                    width = to_int(self.expr(args[0]))
                if width is None and hint is not None and hint.kind == "bits":
                    width = hint.width
                return Bits(width, 0)
            value = self.expr(args[0])
            if len(args) > 1:
                width = to_int(self.expr(args[1]))
            if width is None and hint is not None and hint.kind == "bits":
                width = hint.width
            if width is None:
                width = value.width
            if name == "ZeroExtend":
                return Bits(width, value.value)
            sign = 1 << (value.width - 1)
            return Bits(width, ((value.value ^ sign) - sign) & mask(width))
        if name == "IsZero":
            return to_int(self.expr(args[0])) == 0
        if name == "BitCount":
            return bin(to_int(self.expr(args[0]))).count("1")
        if name == "HighestSetBitNZ":
            return to_int(self.expr(args[0])).bit_length() - 1
        if name == "LowestSetBit":
            value = self.expr(args[0])
            if value.value == 0:
                return value.width
            return (value.value & -value.value).bit_length() - 1
        if name == "InITBlock":
            return self.it_active
        if name == "LastInITBlock":
            return self.it_last
        if name == "CurrentInstrSet":
            return "InstrSet_T32" if self.thumb else "InstrSet_A32"
        if name == "IsFeatureImplemented":
            feature = args[0][1]
            if feature in codegen.FEATURES:
                return codegen.FEATURES[feature] in self.features
            if feature in codegen.UNIMPLEMENTED_FEATURES:
                return False
            raise EvalError(f"unknown feature {feature}")
        if name == "ConstrainUnpredictable":
            return "Constraint_UNDEF"
        if name == "DecodeImmShift":
            kind, amount = to_int(self.expr(args[0])), to_int(self.expr(args[1]))
            if kind == 0:
                return ("SRType_LSL", amount)
            if kind in (1, 2):
                return (("SRType_LSR", "SRType_ASR")[kind - 1], 32 if amount == 0 else amount)
            if amount == 0:
                return ("SRType_RRX", 1)
            return ("SRType_ROR", amount)
        if name == "DecodeRegShift":
            return ("SRType_LSL", "SRType_LSR", "SRType_ASR", "SRType_ROR")[
                to_int(self.expr(args[0]))
            ]
        if name in ("A32ExpandImm", "T32ExpandImm"):
            return self.expand_immediate(name[0], to_int(self.expr(args[0])))[0]
        if name in ("A32ExpandImm_C", "T32ExpandImm_C"):
            carry_arg = self.expr(args[1])
            if not isinstance(carry_arg, Carry):
                raise EvalError("ExpandImm_C with a carry other than PSTATE.C")
            return self.expand_immediate(name[0], to_int(self.expr(args[0])))
        if name == "AdvSIMDExpandImm":
            op, cmode, imm8 = (to_int(self.expr(arg)) for arg in args)
            return Bits(64, advsimd_expand_imm(op, cmode, imm8))
        if name == "VFPExpandImm":
            width = to_int(self.expr(params[0])) if params else hint.width
            return Bits(width, vfp_expand_imm(to_int(self.expr(args[0])), width))
        if name == "FPDecodeRM":
            return ("FPRounding_TIEAWAY", "FPRounding_TIEEVEN", "FPRounding_POSINF",
                    "FPRounding_NEGINF")[to_int(self.expr(args[0]))]
        raise EvalError(f"unsupported function {name}")

    @staticmethod
    def expand_immediate(isa: str, imm12: int):
        if isa == "A":
            rotation = 2 * (imm12 >> 8 & 0xF)
            value = imm12 & 0xFF
            if rotation == 0:
                return Bits(32, value), Carry(0, True)
            result = rotate_right(value, rotation)
            return Bits(32, result), Carry(result >> 31, False)
        byte = imm12 & 0xFF
        if imm12 >> 10 & 3 == 0:
            selector = imm12 >> 8 & 3
            value = (byte, byte << 16 | byte, byte << 24 | byte << 8,
                     byte * 0x01010101)[selector]
            return Bits(32, value), Carry(0, True)
        unrotated = 0x80 | (imm12 & 0x7F)
        result = rotate_right(unrotated, imm12 >> 7 & 0x1F)
        return Bits(32, result), Carry(result >> 31, False)

    # Statements ------------------------------------------------------------

    def coerce(self, value, ty: codegen.Ty):
        if ty.kind == "int":
            return to_int(value)
        if ty.kind == "bool":
            return bool(value)
        if ty.kind == "bits":
            return Bits(ty.width, to_int(value) & mask(ty.width))
        if ty.kind == "enum":
            return value
        if ty.kind == "tuple":
            return tuple(self.coerce(v, t) for v, t in zip(value, ty.items))
        raise EvalError(f"cannot coerce to {ty}")

    def declared(self, type_node) -> codegen.Ty:
        def width_of(node):
            return to_int(self.expr(node))

        return codegen.declared_type(type_node, width_of)

    def set_local(self, name: str, value, ty: codegen.Ty) -> None:
        if isinstance(value, Carry):
            self.carry_flags = self.carry_flags or value.from_flags
            value = Bits(1, value.value)
        if ty.kind == "enum" and ty.name == "Constraint":
            self.locals[name] = value
            return
        self.locals[name] = self.coerce(value, ty)
        self.types[name] = ty

    def execute(self, statements) -> None:
        for statement in statements:
            self.statement(statement)

    def statement(self, statement) -> None:
        kind = statement[0]
        if kind == "let" or kind == "var":
            names, type_node, value_node = statement[1], statement[2], statement[3]
            ty = self.declared(type_node)
            if value_node is None:
                self.set_local(names[0], self.default(ty), ty)
                return
            value = self.expr(value_node, ty if len(names) == 1 else None)
            if len(names) == 1:
                self.set_local(names[0], value, ty)
            else:
                if isinstance(value, tuple) and len(value) == 2 and isinstance(value[1], Carry):
                    self.carry_flags = self.carry_flags or value[1].from_flags
                for name, item_ty, item in zip(names, ty.items, value):
                    self.set_local(name, item, item_ty)
        elif kind == "assign":
            name = statement[1][1]
            ty = self.types[name]
            self.set_local(name, self.expr(statement[2], ty), ty)
        elif kind == "if":
            self.conditional(statement)
        elif kind == "case":
            self.case(statement)
        elif kind == "call-statement":
            self.procedure(statement[1][1])
        elif kind == "assert":
            return
        else:
            raise EvalError(f"unsupported statement {kind}")

    @staticmethod
    def default(ty: codegen.Ty):
        if ty.kind == "bool":
            return False
        if ty.kind == "bits":
            return Bits(ty.width, 0)
        if ty.kind == "enum":
            return codegen.ENUM_VALUES[ty.name][0]
        return 0

    def procedure(self, name: str) -> None:
        kinds = {
            "Undefined": "undefined",
            "UnpredictableProcedure": "unpredictable",
            "ExecuteAsNOP": "nop",
            "See": "see",
        }
        if name not in kinds:
            raise EvalError(f"unsupported procedure {name}")
        raise Outcome(kinds[name])

    def conditional(self, statement) -> None:
        arms, other = statement[1], statement[2]
        if len(arms) == 1 and not other:
            test_node, body = arms[0]
            if (
                len(body) == 1
                and body[0][0] == "call-statement"
                and body[0][1][0] == "call"
                and body[0][1][1] in ("Undefined", "UnpredictableProcedure", "ExecuteAsNOP", "See")
            ):
                if self.expr(test_node):
                    name = body[0][1][1]
                    if name == "Undefined":
                        import a32_asl as asl

                        feature = any(
                            sub[0] == "call" and sub[1] == "IsFeatureImplemented"
                            for sub in asl.walk_expressions(test_node)
                        )
                        raise Outcome("feature" if feature else "undefined")
                    self.procedure(name)
                return
        for test_node, body in arms:
            if self.expr(test_node):
                self.execute(body)
                return
        if other:
            self.execute(other)

    def case(self, statement) -> None:
        selector = self.expr(statement[1])
        for patterns, body in statement[2]:
            for pattern in patterns:
                if self.pattern_matches(selector, pattern):
                    self.execute(body)
                    return
        if statement[3]:
            self.execute(statement[3])


def advsimd_expand_imm(op: int, cmode: int, imm8: int) -> int:
    def replicate(value: int, width: int) -> int:
        return sum(value << shift for shift in range(0, 64, width))

    kind = cmode >> 1
    if kind == 0:
        return replicate(imm8, 32)
    if kind == 1:
        return replicate(imm8 << 8, 32)
    if kind == 2:
        return replicate(imm8 << 16, 32)
    if kind == 3:
        return replicate(imm8 << 24, 32)
    if kind == 4:
        return replicate(imm8, 16)
    if kind == 5:
        return replicate(imm8 << 8, 16)
    if kind == 6:
        if cmode & 1 == 0:
            return replicate(imm8 << 8 | 0xFF, 32)
        return replicate(imm8 << 16 | 0xFFFF, 32)
    if cmode & 1 == 0 and op == 0:
        return replicate(imm8, 8)
    if cmode & 1 == 0 and op == 1:
        return sum(0xFF << (8 * bit) for bit in range(8) if imm8 >> bit & 1)
    if cmode & 1 == 1 and op == 0:
        b6 = imm8 >> 6 & 1
        imm32 = (imm8 >> 7 & 1) << 31 | (b6 ^ 1) << 30 | (0x1F * b6) << 25 | (imm8 & 0x3F) << 19
        return replicate(imm32, 32)
    return 0


def vfp_expand_imm(imm8: int, width: int) -> int:
    exp_bits = {16: 5, 32: 8, 64: 11}[width]
    frac_bits = width - exp_bits - 1
    sign = imm8 >> 7 & 1
    b6 = imm8 >> 6 & 1
    exp = (b6 ^ 1) << (exp_bits - 1) | (mask(exp_bits - 3) * b6) << 2 | (imm8 >> 4 & 3)
    frac = (imm8 & 0xF) << (frac_bits - 4)
    return sign << (width - 1) | exp << frac_bits | frac
