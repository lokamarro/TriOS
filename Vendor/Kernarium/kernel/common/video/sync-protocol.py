#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synchronize the private Virtio video wire header into a kernel topic tree.

The encoder wire carries V4L2 control IDs, menu indices and colorimetry enums
as numbers. Every run, with or without --verify, also checks that each value in
virtio_video_v4l2_ids.h is the one the tree's own UAPI headers define for the
V4L2 symbol it names.
"""
__import__("sys").dont_write_bytecode = True

import argparse
import operator
import re
from pathlib import Path

TABLE = "virtio_video_v4l2_ids.h"
UAPI_HEADERS = ("v4l2-controls.h", "videodev2.h")
TABLE_ENTRY = re.compile(r"^#define\s+UML_(V4L2_\w+)\s+(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*\s*$")
# A function-like macro has "(" directly after its name, which this cannot match.
DEFINE = re.compile(r"^\s*#\s*define\s+(\w+)\s+(\S.*)$")
TOKEN = re.compile(r"\s*(?:(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*|(\w+)|(<<|>>|[-+|&^*~()]))")
BINARY = (
    {"|": operator.or_},
    {"^": operator.xor},
    {"&": operator.and_},
    {"<<": operator.lshift, ">>": operator.rshift},
    {"+": operator.add, "-": operator.sub},
    {"*": operator.mul},
)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


class Symbols:
    """Object-like macros and enumerators of C headers, evaluated on demand."""

    def __init__(self, texts):
        self.exprs = {}
        for text in texts:
            text = strip_comments(text).replace("\\\n", " ")
            for line in text.splitlines():
                match = DEFINE.match(line)
                if match:
                    self.exprs.setdefault(match.group(1), match.group(2).strip())
            for body in re.findall(r"\benum\b[^{;]*\{(.*?)\}", text, flags=re.S):
                body = re.sub(r"^\s*#.*$", " ", body, flags=re.M)
                previous = None
                for item in self.split_enumerators(body):
                    name, _, expr = (part.strip() for part in item.partition("="))
                    if not re.fullmatch(r"\w+", name):
                        continue
                    if expr:
                        self.exprs.setdefault(name, expr)
                    elif previous is None:
                        self.exprs.setdefault(name, "0")
                    else:
                        self.exprs.setdefault(name, f"({previous}) + 1")
                    previous = name

    @staticmethod
    def split_enumerators(body):
        items, depth, start = [], 0, 0
        for index, char in enumerate(body):
            depth += char == "("
            depth -= char == ")"
            if char == "," and depth == 0:
                items.append(body[start:index])
                start = index + 1
        items.append(body[start:])
        return [item for item in items if item.strip()]

    def value(self, name, active=()):
        if name in active:
            raise ValueError(f"{name} is defined in terms of itself")
        if name not in self.exprs:
            raise ValueError(f"{name} is not defined")
        return Expression(self, self.exprs[name], active + (name,)).evaluate()


class Expression:
    """C integer expressions: numbers, names, ( ) ~ - * + - << >> & ^ |."""

    def __init__(self, symbols, text, active):
        self.symbols, self.active, self.text = symbols, active, text
        self.tokens = []
        position, text = 0, text.strip()
        while position < len(text):
            match = TOKEN.match(text, position)
            if not match:
                raise ValueError(f"cannot evaluate '{self.text}'")
            number, name, punctuation = match.groups()
            self.tokens.append(("number", number) if number else ("name", name) if name else ("op", punctuation))
            position = match.end()
        self.index = 0

    def evaluate(self):
        value = self.binary(0)
        if self.index != len(self.tokens):
            raise ValueError(f"cannot evaluate '{self.text}'")
        return value

    def peek(self):
        return self.tokens[self.index] if self.index < len(self.tokens) else (None, None)

    def take(self):
        if self.index == len(self.tokens):
            raise ValueError(f"cannot evaluate '{self.text}'")
        self.index += 1
        return self.tokens[self.index - 1]

    def binary(self, level):
        if level == len(BINARY):
            return self.unary()
        left = self.binary(level + 1)
        while self.peek()[0] == "op" and self.peek()[1] in BINARY[level]:
            apply = BINARY[level][self.take()[1]]
            left = apply(left, self.binary(level + 1))
        return left

    def unary(self):
        kind, text = self.take()
        if kind == "number":
            return int(text, 0)
        if kind == "name":
            return self.symbols.value(text, self.active)
        if text == "(":
            value = self.binary(0)
            if self.take() != ("op", ")"):
                raise ValueError("unbalanced parenthesis")
            return value
        if text in ("-", "~"):
            value = self.unary()
            return -value if text == "-" else ~value
        raise ValueError(f"unexpected '{text}'")


def check_v4l2_ids(table, kernel_source):
    """Return a description of every table entry the tree's UAPI headers contradict."""
    directory = kernel_source / "include/uapi/linux"
    texts = []
    for name in UAPI_HEADERS:
        if not (directory / name).is_file():
            return [f"{directory / name} is missing"]
        texts.append((directory / name).read_text(errors="replace"))
    symbols = Symbols(texts)
    entries, problems = [], []
    for line in strip_comments(table.read_text()).splitlines():
        if not line.startswith("#define UML_V4L2_"):
            continue
        match = TABLE_ENTRY.match(line)
        if match:
            entries.append(match.groups())
        else:
            problems.append(f"{table.name}: unreadable line '{line.strip()}'")
    if not entries:
        problems.append(f"{table.name} lists no V4L2 value")
    for name, expected in entries:
        try:
            actual = symbols.value(name)
        except ValueError as error:
            problems.append(f"{name}: {error}")
            continue
        if actual != int(expected, 0):
            problems.append(f"{name}: table has {int(expected, 0):#x}, kernel has {actual:#x}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-source", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    source = Path(__file__).with_name("virtio_video.h")
    destination = args.kernel_source / "include/uapi/linux/virtio_video.h"
    if not (args.kernel_source / "Makefile").is_file() or not destination.parent.is_dir():
        parser.error("--kernel-source must name a prepared Linux source tree")
    if args.verify:
        if not destination.is_file() or source.read_bytes() != destination.read_bytes():
            parser.error("Virtio video protocol copy is stale")
    else:
        destination.write_bytes(source.read_bytes())
    problems = check_v4l2_ids(Path(__file__).with_name(TABLE), args.kernel_source)
    if problems:
        parser.error("Virtio video V4L2 values disagree with the kernel:\n  " + "\n  ".join(problems))


if __name__ == "__main__":
    main()
