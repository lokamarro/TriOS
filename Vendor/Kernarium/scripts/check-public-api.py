#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that an emulator archive defines exactly the public C API.

The functions declared in the public headers are the API. Each must be
defined exactly once in the archive's global symbols, and the archive must
define no other `uml_emulator_` function, so a public change needs no second
list to update.

    check-public-api.py --headers darwin/api/include/uml --defined NM_OUTPUT
    check-public-api.py --headers darwin/api/include/uml --list

NM_OUTPUT is the output of `nm -gU` on the archive.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import sys
from collections import Counter
from pathlib import Path


# Symbols with this prefix belong to the API, so the archive may not define
# one the headers do not declare.
OWNED_PREFIX = "_uml_emulator_"
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
STRING = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'')
DIRECTIVE = re.compile(r"^[ \t]*#(?:[^\n\\]|\\\n|\\.)*", re.MULTILINE)
EXTERN_C = re.compile(r'extern\s+""\s*$')
NAME_BEFORE = re.compile(r"([A-Za-z_]\w*)\s*$")


def strip(text: str) -> str:
    text = COMMENT.sub(" ", text)
    # Directives first, so an apostrophe in one cannot open a character
    # literal; string literals keep their quotes so `extern "C"` survives.
    text = DIRECTIVE.sub(" ", text)
    return STRING.sub(lambda match: '""' if match.group(0)[0] == '"' else "0",
                      text)


def statements(text: str) -> list[str]:
    """The text of each top-level statement, braces of bodies removed.

    The braces of `extern "C"` are transparent; the contents of every other
    brace pair (struct, enum, function body) are dropped.
    """
    result: list[str] = []
    current = ""
    stack: list[bool] = []  # True for a dropped block, False for extern "C"
    for char in text:
        dropped = any(stack)
        if char == "{":
            if not dropped and EXTERN_C.search(current):
                stack.append(False)
                current = ""
            else:
                stack.append(True)
                if not dropped:
                    current += "{"
            continue
        if char == "}":
            if not stack:
                raise ValueError("unbalanced closing brace")
            was_dropped = stack.pop()
            if was_dropped and not any(stack):
                current += "}"
                # A function body ends its own statement; the braces of a
                # struct or enum are followed by a declarator and a `;`.
                if re.search(r"\)\s*\{\}$", current):
                    result.append(current)
                    current = ""
            continue
        if any(stack):
            continue
        if char == ";":
            result.append(current)
            current = ""
        else:
            current += char
    if stack:
        raise ValueError("unbalanced opening brace")
    if current.strip():
        result.append(current)
    return result


def declared_functions(text: str) -> list[str]:
    names: list[str] = []
    for statement in statements(strip(text)):
        statement = statement.strip()
        # A body is a definition, which no archive exports by this name.
        if (not statement or statement.startswith("typedef")
                or statement.endswith("{}")):
            continue
        paren = statement.find("(")
        if paren < 0:
            continue
        # A function-pointer variable is not a function declaration.
        if statement[paren + 1:].lstrip().startswith("*"):
            continue
        found = NAME_BEFORE.search(statement[:paren])
        if found is None:
            raise ValueError(f"cannot name the function in: {statement!r}")
        names.append(found.group(1))
    return names


def header_functions(headers: Path) -> list[str]:
    paths = sorted(headers.rglob("*.h"))
    if not paths:
        raise ValueError(f"no public header below {headers}")
    names: list[str] = []
    for path in paths:
        names.extend(declared_functions(path.read_text(encoding="utf-8")))
    if not names:
        raise ValueError(f"no public function declared below {headers}")
    duplicated = sorted(name for name, count in Counter(names).items()
                        if count > 1)
    if duplicated:
        raise ValueError("declared more than once: " + ", ".join(duplicated))
    return sorted(names)


def problems(declared: list[str], defined_text: str) -> list[str]:
    definitions: Counter[str] = Counter()
    for line in defined_text.splitlines():
        fields = line.split()
        if fields and fields[-1].startswith(OWNED_PREFIX):
            definitions[fields[-1]] += 1
    wanted = {"_" + name for name in declared}
    found = []
    for symbol in sorted(definitions):
        if symbol not in wanted:
            found.append(f"unexpected public API symbol: {symbol}")
    for symbol in sorted(wanted):
        if definitions[symbol] != 1:
            found.append(f"public API symbol {symbol} has "
                         f"{definitions[symbol]} definitions")
    return found


def main(arguments: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--headers", type=Path, required=True,
                        help="the public header directory")
    parser.add_argument("--defined", type=Path,
                        help="`nm -gU` output of the archive")
    parser.add_argument("--list", action="store_true",
                        help="print the declared functions and stop")
    options = parser.parse_args(arguments)
    try:
        declared = header_functions(options.headers)
    except ValueError as error:
        print(f"check-public-api.py: {error}", file=sys.stderr)
        return 1
    if options.list:
        print("\n".join(declared))
        return 0
    if options.defined is None:
        parser.error("--defined or --list is required")
    found = problems(declared, options.defined.read_text(encoding="utf-8"))
    for message in found:
        print(message, file=sys.stderr)
    return 1 if found else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
