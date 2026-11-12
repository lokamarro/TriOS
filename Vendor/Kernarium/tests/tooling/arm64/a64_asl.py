#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Evaluate the decode and check-selection pseudocode of Arm A64 XML pages.

The SVE and SME encoding table needs to know, for each field value of an
encoding, whether the decode pseudocode ends in EndOfDecode(Decode_UNDEF),
under which features and maximum vector lengths, and which access check the
execute pseudocode calls. This module parses the small part of ASL those
sections use: let, var and constant bindings, assignments, if and case
statements, and expressions over fields, literals and a fixed set of
functions. Evaluation is concrete: fields have values, features are a set of
names, and the maximum vector lengths are numbers. Anything outside that set
evaluates to an opaque value, and using an opaque value where a decision
depends on it raises AslError, so a caller never acts on a guess. A
CONSTRAINED UNPREDICTABLE choice is opaque too unless the caller fixes it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
from dataclasses import dataclass


class AslError(ValueError):
    pass


class Undefined(Exception):
    """The decode pseudocode reached EndOfDecode(Decode_UNDEF)."""


@dataclass(frozen=True)
class Bits:
    width: int
    value: int
    # Bits that a pattern literal leaves as 'x'; zero for values.
    wild: int = 0

    def __post_init__(self) -> None:
        if self.width < 0 or self.value >> self.width or self.wild >> self.width:
            raise AslError(f"malformed bit value {self!r}")


@dataclass(frozen=True)
class Opaque:
    reason: str


TOKEN = re.compile(
    r"\s*(?:(?P<bits>'[01x ]*')|(?P<number>\d+)|(?P<name>[A-Za-z_][A-Za-z0-9_.]*)"
    r"|(?P<op>::|==|!=|<=|>=|<<|>>|&&|\|\||=>|[-+*/<>!(){}\[\],.:;=^]))"
)

def tokenize(text: str) -> list[str]:
    text = re.sub(r"//[^\n]*", "", text)
    result: list[str] = []
    position = 0
    while True:
        while position < len(text) and text[position].isspace():
            position += 1
        if position >= len(text):
            return result
        match = TOKEN.match(text, position)
        if match is None or match.end() == position:
            raise AslError(f"cannot tokenize ASL near {text[position:position + 40]!r}")
        result.append(match.group(match.lastgroup))
        position = match.end()


# Expression nodes are tuples: ("num", n), ("bits", Bits), ("bool", b),
# ("name", s), ("call", name, args), ("slice", expr, [(hi, lo)]),
# ("unary", op, expr), ("binary", op, left, right), ("in", expr, [patterns]),
# ("cond", test, then, else), ("typed", name, params) for Zeros{esize}-style
# calls is folded into "call".

PRECEDENCE = {
    "||": 1, "&&": 2,
    "==": 3, "!=": 3, "<": 3, "<=": 3, ">": 3, ">=": 3, "IN": 3,
    "OR": 4, "EOR": 4, "XOR": 4, "AND": 4,
    "+": 5, "-": 5, "::": 5,
    "*": 6, "DIV": 6, "MOD": 6, "/": 6,
    "<<": 7, ">>": 7,
}


class Parser:
    def __init__(self, tokens: list[str]):
        self.tokens = tokens
        self.index = 0

    def peek(self, offset: int = 0) -> str | None:
        position = self.index + offset
        return self.tokens[position] if position < len(self.tokens) else None

    def take(self, expected: str | None = None) -> str:
        token = self.peek()
        if token is None or (expected is not None and token != expected):
            raise AslError(
                f"expected {expected!r}, found {token!r} in "
                f"{' '.join(self.tokens[max(0, self.index - 8):self.index + 8])!r}"
            )
        self.index += 1
        return token

    # Expressions.
    def expression(self, minimum: int = 0):
        if self.peek() == "if":
            self.take("if")
            test = self.expression()
            self.take("then")
            then = self.expression()
            self.take("else")
            other = self.expression()
            return ("cond", test, then, other)
        left = self.unary()
        while True:
            op = self.peek()
            precedence = PRECEDENCE.get(op or "")
            if precedence is None or precedence < minimum:
                return left
            self.take()
            if op == "IN":
                left = ("in", left, self.pattern_set())
                continue
            right = self.expression(precedence + 1)
            left = ("binary", op, left, right)

    def pattern_set(self):
        self.take("{")
        items = []
        while self.peek() != "}":
            items.append(self.expression(PRECEDENCE["||"] + 1))
            if self.peek() == ",":
                self.take(",")
        self.take("}")
        return items

    def unary(self):
        token = self.peek()
        if token in ("!", "-", "NOT"):
            self.take()
            return ("unary", token, self.unary())
        return self.postfix(self.primary())

    def primary(self):
        token = self.take()
        if token == "(":
            node = self.expression()
            if self.peek() == ",":
                items = [node]
                while self.peek() == ",":
                    self.take(",")
                    items.append(self.expression())
                self.take(")")
                return ("tuple", items)
            self.take(")")
            return node
        if token.startswith("'"):
            text = token[1:-1].replace(" ", "")
            value = int(text.replace("x", "0") or "0", 2)
            wild = int(
                "".join("1" if c == "x" else "0" for c in text) or "0", 2
            )
            return ("bits", Bits(len(text), value, wild))
        if token.isdigit():
            return ("num", int(token))
        if token in ("TRUE", "FALSE"):
            return ("bool", token == "TRUE")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.]*", token):
            raise AslError(f"unexpected ASL token {token!r}")
        params = None
        if self.peek() == "{":
            # Width parameters: Zeros{esize}(...), X{64}(n), Z{}(t).
            self.take("{")
            params = []
            while self.peek() != "}":
                params.append(self.expression())
                if self.peek() == ",":
                    self.take(",")
            self.take("}")
        if self.peek() == "(":
            self.take("(")
            args = []
            while self.peek() != ")":
                args.append(self.expression())
                if self.peek() == ",":
                    self.take(",")
            self.take(")")
            return ("call", token, args, params or [])
        if params is not None:
            return ("call", token, [], params)
        return ("name", token)

    def postfix(self, node):
        while self.peek() == "[":
            self.take("[")
            slices = []
            while self.peek() != "]":
                high = self.expression()
                low = high
                if self.peek() == ":":
                    self.take(":")
                    low = self.expression()
                elif self.peek() == "*:":
                    raise AslError("indexed slices are not supported")
                slices.append((high, low))
                if self.peek() == ",":
                    self.take(",")
            self.take("]")
            node = ("slice", node, slices)
        return node

    # Statements.
    def statements(self, terminators: tuple[str, ...]) -> list:
        result = []
        while self.peek() is not None and self.peek() not in terminators:
            result.append(self.statement())
        return result

    def skip_statement(self, reason: str):
        depth = 0
        start = self.index
        while self.peek() is not None:
            token = self.take()
            if token in ("(", "{", "["):
                depth += 1
            elif token in (")", "}", "]"):
                depth -= 1
            elif token == ";" and depth == 0:
                break
        return ("opaque", reason, " ".join(self.tokens[start:self.index]))

    def statement(self):
        token = self.peek()
        if token in ("let", "constant", "var"):
            self.take()
            name = self.take()
            names = [name]
            while self.peek() == ",":
                self.take(",")
                names.append(self.take())
            if self.peek() == ":":
                self.take(":")
                # Skip the type up to '=' or ';'.
                depth = 0
                while True:
                    current = self.peek()
                    if current is None:
                        raise AslError("unterminated declaration")
                    if depth == 0 and current in ("=", ";"):
                        break
                    if current in ("(", "{", "["):
                        depth += 1
                    elif current in (")", "}", "]"):
                        depth -= 1
                    self.take()
            if self.peek() == ";":
                self.take(";")
                return ("declare", names)
            self.take("=")
            value = self.expression()
            self.take(";")
            if len(names) != 1:
                return ("opaque", "multiple binding", " ".join(names))
            return ("let", names[0], value)
        if token == "if":
            self.take("if")
            arms = []
            test = self.expression()
            self.take("then")
            body = self.statements(("elsif", "else", "end"))
            arms.append((test, body))
            other: list = []
            while self.peek() == "elsif":
                self.take("elsif")
                test = self.expression()
                self.take("then")
                arms.append((test, self.statements(("elsif", "else", "end"))))
            if self.peek() == "else":
                self.take("else")
                other = self.statements(("end",))
            self.take("end")
            self.take(";")
            return ("if", arms, other)
        if token == "case":
            self.take("case")
            selector = self.expression()
            self.take("of")
            arms = []
            other = None
            while self.peek() in ("when", "otherwise"):
                if self.take() == "when":
                    patterns = [self.expression(PRECEDENCE["||"] + 1)]
                    while self.peek() == ",":
                        self.take(",")
                        patterns.append(self.expression(PRECEDENCE["||"] + 1))
                    self.take("=>")
                    arms.append(
                        (patterns, self.statements(("when", "otherwise", "end")))
                    )
                else:
                    self.take("=>")
                    other = self.statements(("end",))
            self.take("end")
            self.take(";")
            return ("case", selector, arms, other)
        if token in ("for", "while"):
            return self.skip_loop()
        if token == "EndOfDecode":
            self.take("EndOfDecode")
            self.take("(")
            reason = self.take()
            self.take(")")
            self.take(";")
            return ("end-of-decode", reason)
        # Assignment or procedure call.
        start = self.index
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", token or "") and \
                self.peek(1) == "=":
            name = self.take()
            self.take("=")
            value = self.expression()
            self.take(";")
            return ("assign", name, value)
        try:
            node = self.expression()
            self.take(";")
            return ("call-statement", node)
        except AslError:
            self.index = start
            return self.skip_statement("unparsed statement")

    def skip_loop(self):
        start = self.index
        while self.peek() not in (None, "do"):
            self.take()
        self.take("do")
        body = self.statements(("end",))
        self.take("end")
        self.take(";")
        return ("loop", " ".join(self.tokens[start:self.index]), body)


def parse_statements(text: str) -> list:
    parser = Parser(tokenize(text))
    result = parser.statements(())
    if parser.peek() is not None:
        raise AslError(f"trailing ASL {parser.peek()!r}")
    return result


@dataclass
class Environment:
    """Concrete inputs: field values, implemented features, vector lengths."""

    fields: dict[str, Bits]
    features: frozenset[str]
    max_svl: int | None = None
    max_vl: int | None = None
    values: dict | None = None
    # The Constraint_ value every ConstrainUnpredictable() returns, or None
    # to leave it opaque.
    unpredictable: str | None = None

    def __post_init__(self) -> None:
        if self.values is None:
            self.values = {}


def _bits_of(value, what: str) -> Bits:
    if isinstance(value, Bits):
        if value.wild:
            raise AslError(f"{what}: a pattern is not a value")
        return value
    if isinstance(value, Opaque):
        raise AslError(f"{what} depends on {value.reason}")
    raise AslError(f"{what}: expected bits, found {value!r}")


def _int_of(value, what: str) -> int:
    if isinstance(value, bool):
        raise AslError(f"{what}: expected an integer, found a boolean")
    if isinstance(value, int):
        return value
    if isinstance(value, Opaque):
        raise AslError(f"{what} depends on {value.reason}")
    raise AslError(f"{what}: expected an integer, found {value!r}")


def _bool_of(value, what: str) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, Opaque):
        raise AslError(f"{what} depends on {value.reason}")
    raise AslError(f"{what}: expected a boolean, found {value!r}")


def _equal(left, right) -> bool:
    if isinstance(left, Opaque) or isinstance(right, Opaque):
        reason = left.reason if isinstance(left, Opaque) else right.reason
        raise AslError(f"comparison depends on {reason}")
    if isinstance(left, Bits) and isinstance(right, Bits):
        if left.width != right.width:
            raise AslError(f"comparison of {left.width}- and {right.width}-bit values")
        wild = left.wild | right.wild
        return (left.value & ~wild) == (right.value & ~wild)
    if isinstance(left, bool) != isinstance(right, bool):
        raise AslError("comparison of a boolean with a non-boolean")
    if type(left) is not type(right):
        raise AslError(f"comparison of {left!r} with {right!r}")
    return left == right


def _lowest_set(bits: Bits) -> int:
    if bits.value == 0:
        return bits.width
    return (bits.value & -bits.value).bit_length() - 1


def _highest_set(bits: Bits) -> int:
    return bits.value.bit_length() - 1


class Evaluator:
    def __init__(self, environment: Environment):
        self.env = environment

    def name(self, name: str):
        if name in self.env.values:
            return self.env.values[name]
        if name in self.env.fields:
            return self.env.fields[name]
        if re.fullmatch(r"Constraint_[A-Z_]+", name):
            return name
        return Opaque(f"unbound name {name}")

    def evaluate(self, node):
        kind = node[0]
        if kind == "num":
            return node[1]
        if kind == "bits":
            return node[1]
        if kind == "bool":
            return node[1]
        if kind == "name":
            return self.name(node[1])
        if kind == "tuple":
            return Opaque("tuple")
        if kind == "cond":
            test = self.evaluate(node[1])
            if isinstance(test, Opaque):
                return test
            return self.evaluate(node[2] if _bool_of(test, "if") else node[3])
        if kind == "unary":
            value = self.evaluate(node[2])
            if isinstance(value, Opaque):
                return value
            if node[1] == "!":
                return not _bool_of(value, "!")
            if node[1] == "-":
                return -_int_of(value, "-")
            bits = _bits_of(value, "NOT")
            return Bits(bits.width, ~bits.value & ((1 << bits.width) - 1))
        if kind == "in":
            value = self.evaluate(node[1])
            if isinstance(value, Opaque):
                return value
            return any(_equal(value, self.evaluate(item)) for item in node[2])
        if kind == "binary":
            return self.binary(node[1], node[2], node[3])
        if kind == "slice":
            return self.slice(node)
        if kind == "call":
            return self.call(node[1], node[2], node[3])
        raise AslError(f"unsupported ASL node {kind}")

    def binary(self, op: str, left_node, right_node):
        if op in ("&&", "||"):
            left = self.evaluate(left_node)
            if isinstance(left, Opaque):
                # The other side can still decide: FALSE && x, TRUE || x.
                right = self.evaluate(right_node)
                if isinstance(right, bool) and right == (op == "||"):
                    return right
                return left
            if _bool_of(left, op) == (op == "||"):
                return left
            right = self.evaluate(right_node)
            if isinstance(right, Opaque):
                return right
            return _bool_of(right, op)
        left = self.evaluate(left_node)
        right = self.evaluate(right_node)
        if isinstance(left, Opaque):
            return left
        if isinstance(right, Opaque):
            return right
        if op == "==":
            return _equal(left, right)
        if op == "!=":
            return not _equal(left, right)
        if op == "::":
            a = _bits_of(left, "::")
            b = _bits_of(right, "::")
            return Bits(a.width + b.width, a.value << b.width | b.value)
        if op in ("AND", "OR", "EOR", "XOR"):
            a = _bits_of(left, op)
            b = _bits_of(right, op)
            if a.width != b.width:
                raise AslError(f"{op} of different widths")
            value = {"AND": a.value & b.value, "OR": a.value | b.value,
                     "EOR": a.value ^ b.value, "XOR": a.value ^ b.value}[op]
            return Bits(a.width, value)
        a = _int_of(left, op)
        b = _int_of(right, op)
        if op == "+":
            return a + b
        if op == "-":
            return a - b
        if op == "*":
            return a * b
        if op == "DIV":
            if b == 0 or a % b:
                raise AslError("inexact DIV")
            return a // b
        if op == "MOD":
            return a % b
        if op == "<<":
            return a << b
        if op == ">>":
            return a >> b
        if op == "<":
            return a < b
        if op == "<=":
            return a <= b
        if op == ">":
            return a > b
        if op == ">=":
            return a >= b
        raise AslError(f"unsupported ASL operator {op}")

    def slice(self, node):
        value = self.evaluate(node[1])
        if isinstance(value, Opaque):
            return value
        bits = _bits_of(value, "slice") if not isinstance(value, int) else None
        result_width = 0
        result = 0
        for high_node, low_node in node[2]:
            high = _int_of(self.evaluate(high_node), "slice")
            low = _int_of(self.evaluate(low_node), "slice")
            if high < low:
                raise AslError("descending slice")
            width = high - low + 1
            source = value if bits is None else bits.value
            if bits is not None and high >= bits.width:
                raise AslError("slice beyond the value")
            result = result << width | (source >> low) & ((1 << width) - 1)
            result_width += width
        return Bits(result_width, result)

    def call(self, name: str, args: list, params: list):
        if name == "IsFeatureImplemented":
            if len(args) != 1 or args[0][0] != "name":
                raise AslError("malformed IsFeatureImplemented")
            return args[0][1] in self.env.features
        if name == "MaxImplementedSVL":
            if self.env.max_svl is None:
                return Opaque("the maximum SVL")
            return self.env.max_svl
        if name == "MaxImplementedVL":
            if self.env.max_vl is None:
                return Opaque("the maximum VL")
            return self.env.max_vl
        if name == "MaxImplementedAnyVL":
            values = [v for v in (self.env.max_svl, self.env.max_vl) if v]
            if self.env.max_svl is None:
                return Opaque("the maximum SVL")
            return max(values)
        if name == "ConstrainUnpredictable":
            if self.env.unpredictable is None:
                return Opaque("a CONSTRAINED UNPREDICTABLE choice")
            return self.env.unpredictable
        if name == "Halted":
            # EL0 code never runs in Debug state.
            return False
        values = [self.evaluate(arg) for arg in args]
        for value in values:
            if isinstance(value, Opaque):
                return value
        if name == "UInt" and len(values) == 1:
            return _bits_of(values[0], "UInt").value
        if name == "SInt" and len(values) == 1:
            bits = _bits_of(values[0], "SInt")
            sign = 1 << (bits.width - 1)
            return (bits.value ^ sign) - sign
        if name == "LowestSetBitNZ" and len(values) == 1:
            bits = _bits_of(values[0], name)
            if bits.value == 0:
                raise AslError("LowestSetBitNZ of zero")
            return _lowest_set(bits)
        if name == "LowestSetBit" and len(values) == 1:
            return _lowest_set(_bits_of(values[0], name))
        if name == "HighestSetBitNZ" and len(values) == 1:
            bits = _bits_of(values[0], name)
            if bits.value == 0:
                raise AslError("HighestSetBitNZ of zero")
            return _highest_set(bits)
        if name == "HighestSetBit" and len(values) == 1:
            return _highest_set(_bits_of(values[0], name))
        if name == "IsZero" and len(values) == 1:
            return _bits_of(values[0], name).value == 0
        if name == "IsOnes" and len(values) == 1:
            bits = _bits_of(values[0], name)
            return bits.value == (1 << bits.width) - 1
        if name == "BitCount" and len(values) == 1:
            return _bits_of(values[0], name).value.bit_count()
        if name == "NOT" and len(values) == 1:
            bits = _bits_of(values[0], name)
            return Bits(bits.width, ~bits.value & ((1 << bits.width) - 1))
        if name == "ZeroExtend" and len(values) == 2:
            bits = _bits_of(values[0], name)
            return Bits(_int_of(values[1], name), bits.value)
        if name == "ZeroExtend" and len(values) == 1 and len(params) == 1:
            bits = _bits_of(values[0], name)
            return Bits(_int_of(self.evaluate(params[0]), name), bits.value)
        return Opaque(f"function {name}")

    # Statements.
    def run(self, statements: list) -> None:
        for statement in statements:
            self.execute(statement)

    def execute(self, statement) -> None:
        kind = statement[0]
        if kind in ("let", "assign"):
            self.env.values[statement[1]] = self.evaluate(statement[2])
        elif kind == "declare":
            for name in statement[1]:
                self.env.values[name] = Opaque(f"unassigned {name}")
        elif kind == "end-of-decode":
            if statement[1] != "Decode_UNDEF":
                raise AslError(f"unsupported EndOfDecode({statement[1]})")
            raise Undefined()
        elif kind == "if":
            for test, body in statement[1]:
                value = self.evaluate(test)
                if isinstance(value, Opaque):
                    if _ends_in_undefined(body) or _assigns(body):
                        raise AslError(
                            f"decode decision depends on {value.reason}"
                        )
                    continue
                if _bool_of(value, "if"):
                    self.run(body)
                    return
            self.run(statement[2])
        elif kind == "case":
            selector = self.evaluate(statement[1])
            for patterns, body in statement[2]:
                if isinstance(selector, Opaque):
                    if _ends_in_undefined(body) or _assigns(body):
                        raise AslError(
                            f"decode case depends on {selector.reason}"
                        )
                    continue
                if any(_equal(selector, self.evaluate(p)) for p in patterns):
                    self.run(body)
                    return
            if statement[3] is not None:
                if isinstance(selector, Opaque) and (
                    _ends_in_undefined(statement[3]) or _assigns(statement[3])
                ):
                    raise AslError(f"decode case depends on {selector.reason}")
                if not isinstance(selector, Opaque):
                    self.run(statement[3])
        elif kind == "call-statement":
            node = statement[1]
            if node[0] == "call" and node[1] in ("Unreachable",):
                raise Undefined()
        elif kind == "opaque":
            for name in _bound_names_in_opaque(statement[2]):
                self.env.values[name] = Opaque(statement[1])
        elif kind == "loop":
            if _ends_in_undefined(statement[2]):
                raise AslError("decode loop can end in UNDEFINED")
            for name in _assigned_names(statement[2]):
                self.env.values[name] = Opaque("a loop")
        else:
            raise AslError(f"unsupported ASL statement {kind}")


def _ends_in_undefined(statements: list) -> bool:
    for statement in statements:
        if statement[0] == "end-of-decode":
            return True
        if statement[0] == "if" and (
            any(_ends_in_undefined(body) for _test, body in statement[1])
            or _ends_in_undefined(statement[2])
        ):
            return True
        if statement[0] == "case" and (
            any(_ends_in_undefined(body) for _patterns, body in statement[2])
            or (statement[3] is not None and _ends_in_undefined(statement[3]))
        ):
            return True
    return False


def _assigns(statements: list) -> bool:
    for statement in statements:
        if statement[0] in ("assign", "let", "declare", "opaque"):
            return True
        if statement[0] == "if" and (
            any(_assigns(body) for _test, body in statement[1])
            or _assigns(statement[2])
        ):
            return True
        if statement[0] == "case" and (
            any(_assigns(body) for _patterns, body in statement[2])
            or (statement[3] is not None and _assigns(statement[3]))
        ):
            return True
    return False


def _assigned_names(statements: list) -> set[str]:
    result: set[str] = set()
    for statement in statements:
        if statement[0] in ("assign", "let"):
            result.add(statement[1])
        elif statement[0] == "declare":
            result.update(statement[1])
        elif statement[0] == "if":
            for _test, body in statement[1]:
                result |= _assigned_names(body)
            result |= _assigned_names(statement[2])
        elif statement[0] == "case":
            for _patterns, body in statement[2]:
                result |= _assigned_names(body)
            if statement[3] is not None:
                result |= _assigned_names(statement[3])
        elif statement[0] == "loop":
            result |= _assigned_names(statement[2])
        elif statement[0] == "opaque":
            result.update(_bound_names_in_opaque(statement[2]))
    return result


def _bound_names_in_opaque(text: str) -> list[str]:
    match = re.match(r"(?:let|var|constant)\s+([A-Za-z_][A-Za-z0-9_]*)", text)
    if match:
        return [match.group(1)]
    match = re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*=", text)
    return [match.group(1)] if match else []


def names_in(node) -> set[str]:
    """Return the names an expression reads."""
    if not isinstance(node, tuple):
        return set()
    kind = node[0]
    if kind == "name":
        return {node[1]}
    result: set[str] = set()
    for item in node[1:]:
        if isinstance(item, tuple):
            result |= names_in(item)
        elif isinstance(item, list):
            for element in item:
                if isinstance(element, tuple) and element and isinstance(element[0], str):
                    result |= names_in(element)
                elif isinstance(element, tuple):
                    for part in element:
                        result |= names_in(part)
    return result
