#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Parse the ASL of Arm AArch32 XML pages.

The AArch32 pages carry Decode and Execute pseudocode in ASL1. This module
parses the part of it the encoding-table generator reads: the Decode
sections in full, which the generator turns into operand-decoding code, and
the control structure of the Execute sections, which it scans for the calls
that decide a row's PC, flags and memory behaviour.

Expressions are tuples tagged by their first element:

    ("num", n)                       integer literal
    ("bits", width, value, wild)     bit-string literal; wild marks 'x' bits
    ("bool", b)                      TRUE or FALSE
    ("name", s)                      variable, field or constant
    ("call", name, args, params)     call; params are the {..} width list
    ("member", expr, name)           expr.name (FPSCR().Len)
    ("slice", expr, [(hi, lo)])      expr[hi:lo] or expr[i]
    ("unary", op, expr)              !, - or NOT
    ("binary", op, left, right)
    ("in", expr, [patterns])
    ("cond", test, then, else)       if .. then .. else .. expression
    ("tuple", [exprs])
    ("arbitrary", type)              ARBITRARY : type

Statements:

    ("let", names, type, expr)       let or constant binding; names is a list
    ("var", names, type, expr|None)  var declaration
    ("assign", target, expr)         target is an expression tree
    ("if", [(test, body)], else)
    ("case", selector, [([patterns], body)], otherwise)
    ("loop", header, body)
    ("call-statement", expr)
    ("assert", expr)
    ("opaque", text)                 a simple statement the parser leaves raw

A statement the lenient mode cannot read becomes ("opaque", text); the strict
mode (Decode sections) raises AslError instead, so a shape the generator does
not recognise fails the generation rather than taking a default.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re


class AslError(ValueError):
    pass


TOKEN = re.compile(
    r"\s*(?:(?P<bits>'[01x ]*')|(?P<string>\"[^\"]*\")"
    r"|(?P<number>0x[0-9A-Fa-f]+|\d+)"
    r"|(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"|(?P<op>::|==|!=|<=|>=|<<|>>|&&|\|\||=>|\*:|\+:|-:|"
    r"[-+*/<>!(){}\[\],.:;=^|&]))"
)
PRECEDENCE = {
    "||": 1, "&&": 2,
    "==": 3, "!=": 3, "<": 3, "<=": 3, ">": 3, ">=": 3, "IN": 3,
    "OR": 4, "EOR": 4, "XOR": 4, "AND": 4,
    "+": 5, "-": 5, "::": 5,
    "*": 6, "DIV": 6, "MOD": 6, "DIVRM": 6, "/": 6,
    "<<": 7, ">>": 7,
    "^": 8,
}
TERMINATORS = ("elsif", "else", "end", "when", "otherwise", "until")


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


class Parser:
    def __init__(self, tokens: list[str], strict: bool):
        self.tokens = tokens
        self.index = 0
        self.strict = strict

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

    # Types ---------------------------------------------------------------

    def type_(self):
        """A type as a tuple: ("integer",), ("bits", expr), ("name", s), ..."""
        if self.peek() == "(":
            self.take("(")
            items = [self.type_()]
            while self.peek() == ",":
                self.take(",")
                items.append(self.type_())
            self.take(")")
            return ("tuple", items)
        name = self.take()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise AslError(f"unexpected type token {name!r}")
        if name == "bits" and self.peek() == "(":
            self.take("(")
            width = self.expression()
            self.take(")")
            return ("bits", width)
        if name == "integer" and self.peek() == "{":
            self.take("{")
            depth = 1
            while depth:
                token = self.take()
                depth += token == "{"
                depth -= token == "}"
            return ("integer",)
        return ("name", name)

    # Expressions ---------------------------------------------------------

    def expression(self, minimum: int = 0):
        if self.peek() == "if":
            self.take("if")
            test = self.expression()
            self.take("then")
            then = self.expression()
            if self.peek() == "elsif":
                self.tokens[self.index] = "if"
                other = self.expression()
            else:
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
        if token == "!":
            # ASL1 reads `! x IN {..}` and `! a == b` as the negation of the
            # whole comparison.
            self.take()
            return ("unary", token, self.expression(PRECEDENCE["IN"]))
        if token in ("-", "NOT"):
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
            wild = int("".join("1" if c == "x" else "0" for c in text) or "0", 2)
            return ("bits", len(text), value, wild)
        if token.startswith('"'):
            return ("string", token[1:-1])
        if token.isdigit() or token.startswith("0x"):
            return ("num", int(token, 0))
        if token in ("TRUE", "FALSE"):
            return ("bool", token == "TRUE")
        if token == "ARBITRARY":
            self.take(":")
            return ("arbitrary", self.type_())
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", token):
            raise AslError(f"unexpected ASL token {token!r}")
        params: list = []
        if self.peek() == "{":
            self.take("{")
            while self.peek() != "}":
                params.append(self.expression())
                if self.peek() == ",":
                    self.take(",")
            self.take("}")
            if self.peek() != "(":
                return ("call", token, [], params)
        if self.peek() == "(":
            self.take("(")
            args = []
            while self.peek() != ")":
                args.append(self.expression())
                if self.peek() == ",":
                    self.take(",")
            self.take(")")
            return ("call", token, args, params)
        return ("name", token)

    def postfix(self, node):
        while True:
            token = self.peek()
            if token == "[":
                self.take("[")
                slices = []
                while self.peek() != "]":
                    high = self.expression()
                    low = high
                    if self.peek() == ":":
                        self.take(":")
                        low = self.expression()
                    elif self.peek() in ("*:", "+:", "-:"):
                        kind = self.take()
                        width = self.expression()
                        high = ("indexed", kind, high, width)
                        low = None
                    slices.append((high, low))
                    if self.peek() == ",":
                        self.take(",")
                self.take("]")
                node = ("slice", node, slices)
            elif token == "." and re.fullmatch(
                r"[A-Za-z_][A-Za-z0-9_]*", self.peek(1) or ""
            ):
                self.take(".")
                node = ("member", node, self.take())
            else:
                return node

    # Statements ----------------------------------------------------------

    def statements(self, terminators: tuple[str, ...] = ()) -> list:
        result = []
        while self.peek() is not None and self.peek() not in terminators:
            result.append(self.statement())
        return result

    def raw_statement(self):
        """Consume a statement's tokens up to its terminating semicolon."""
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
        return ("opaque", " ".join(self.tokens[start:self.index]))

    def statement(self):
        start = self.index
        try:
            return self._statement()
        except AslError:
            if self.strict:
                raise
            self.index = start
            return self.raw_statement()

    def _statement(self):
        token = self.peek()
        if token in ("let", "constant", "var"):
            return self.binding()
        if token == "if":
            self.take("if")
            arms = []
            test = self.expression()
            self.take("then")
            body = self.statements(TERMINATORS)
            arms.append((test, body))
            other: list = []
            while self.peek() == "elsif":
                self.take("elsif")
                test = self.expression()
                self.take("then")
                arms.append((test, self.statements(TERMINATORS)))
            if self.peek() == "else":
                self.take("else")
                other = self.statements(TERMINATORS)
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
                    arms.append((patterns, self.statements(TERMINATORS)))
                else:
                    self.take("=>")
                    other = self.statements(TERMINATORS)
            self.take("end")
            self.take(";")
            return ("case", selector, arms, other)
        if token in ("for", "while"):
            header_start = self.index
            while self.peek() not in (None, "do"):
                self.take()
            header = " ".join(self.tokens[header_start:self.index])
            self.take("do")
            body = self.statements(TERMINATORS)
            self.take("end")
            self.take(";")
            return ("loop", header, body)
        if token == "repeat":
            self.take("repeat")
            body = self.statements(TERMINATORS)
            self.take("until")
            condition = self.expression()
            self.take(";")
            return ("loop", "repeat", body + [("call-statement", condition)])
        if token == "assert":
            self.take("assert")
            node = self.expression()
            self.take(";")
            return ("assert", node)
        # Assignment or procedure call.
        node = self.expression()
        if self.peek() == "=":
            self.take("=")
            value = self.expression()
            self.take(";")
            return ("assign", node, value)
        if self.peek() == ":" and node[0] == "name":
            # An lvalue with a type annotation, as in `x : bits(8) = ..`.
            raise AslError("typed assignment")
        self.take(";")
        return ("call-statement", node)

    def binding(self):
        kind = self.take()
        if self.peek() == "(":
            self.take("(")
            names = [self.take()]
            while self.peek() == ",":
                self.take(",")
                names.append(self.take())
            self.take(")")
        else:
            names = [self.take()]
            while self.peek() == ",":
                self.take(",")
                names.append(self.take())
        type_ = None
        if self.peek() == ":":
            self.take(":")
            type_ = self.type_()
        value = None
        if self.peek() == "=":
            self.take("=")
            value = self.expression()
        self.take(";")
        if kind == "var":
            return ("var", names, type_, value)
        if value is None:
            raise AslError("let binding has no value")
        return ("let", names, type_, value)


def parse(text: str, strict: bool = True) -> list:
    parser = Parser(tokenize(text), strict)
    result = parser.statements(())
    if parser.peek() is not None:
        raise AslError(f"trailing ASL {parser.peek()!r}")
    return result


def walk_expressions(node):
    """Yield every sub-expression of an expression tree, node first."""
    yield node
    kind = node[0]
    if kind in ("unary",):
        yield from walk_expressions(node[2])
    elif kind == "binary":
        yield from walk_expressions(node[2])
        yield from walk_expressions(node[3])
    elif kind == "in":
        yield from walk_expressions(node[1])
        for item in node[2]:
            yield from walk_expressions(item)
    elif kind == "cond":
        for child in node[1:]:
            yield from walk_expressions(child)
    elif kind == "tuple":
        for child in node[1]:
            yield from walk_expressions(child)
    elif kind == "call":
        for child in node[2] + node[3]:
            yield from walk_expressions(child)
    elif kind == "member":
        yield from walk_expressions(node[1])
    elif kind == "slice":
        yield from walk_expressions(node[1])
        for high, low in node[2]:
            for part in (high, low):
                if part is None:
                    continue
                if part[0] == "indexed":
                    yield from walk_expressions(part[2])
                    yield from walk_expressions(part[3])
                else:
                    yield from walk_expressions(part)
