#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject malformed or duplicate-key first-party JSON and JSONC inputs."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Any


class JsonObject(list[tuple[str, Any]]):
    """Retain object pairs so duplicate members survive parsing."""


EXCLUDED_ROOT_DIRECTORIES = frozenset({".build", ".git", "external", "linux"})
IDENTIFIER_KEY = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def json_path(parent: str, key: str) -> str:
    if IDENTIFIER_KEY.fullmatch(key):
        return f"{parent}.{key}"
    return f"{parent}[{json.dumps(key, ensure_ascii=False)}]"


def strip_jsonc_comments(source: str) -> str:
    """Replace JSONC comments with whitespace while preserving locations."""
    output: list[str] = []
    index = 0
    in_string = False
    escaped = False
    while index < len(source):
        character = source[index]
        if in_string:
            output.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            index += 1
            continue
        if character == '"':
            in_string = True
            output.append(character)
            index += 1
            continue
        if source.startswith("//", index):
            output.extend((" ", " "))
            index += 2
            while index < len(source) and source[index] not in "\r\n":
                output.append(" ")
                index += 1
            continue
        if source.startswith("/*", index):
            output.extend((" ", " "))
            index += 2
            while index < len(source) and not source.startswith("*/", index):
                output.append(source[index] if source[index] in "\r\n" else " ")
                index += 1
            if index == len(source):
                raise ValueError("unterminated block comment")
            output.extend((" ", " "))
            index += 2
            continue
        output.append(character)
        index += 1
    return "".join(output)


def strip_jsonc_trailing_commas(source: str) -> str:
    """Accept the trailing commas supported by maintained JSONC inputs."""
    output = list(source)
    index = 0
    in_string = False
    escaped = False
    while index < len(source):
        character = source[index]
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            index += 1
            continue
        if character == '"':
            in_string = True
            index += 1
            continue
        if character == ",":
            lookahead = index + 1
            while lookahead < len(source) and source[lookahead].isspace():
                lookahead += 1
            if lookahead < len(source) and source[lookahead] in "}]":
                output[index] = " "
        index += 1
    return "".join(output)


def duplicate_paths(value: Any, parent: str = "$") -> list[str]:
    duplicates: list[str] = []
    if isinstance(value, JsonObject):
        seen: set[str] = set()
        for key, child in value:
            child_path = json_path(parent, key)
            if key in seen:
                duplicates.append(child_path)
            else:
                seen.add(key)
            duplicates.extend(duplicate_paths(child, child_path))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            duplicates.extend(duplicate_paths(child, f"{parent}[{index}]"))
    return duplicates


def validate(path: Path) -> list[str]:
    try:
        source = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        return [f"{path}: cannot read UTF-8 JSON input: {error}"]
    if path.suffix.lower() == ".jsonc":
        try:
            source = strip_jsonc_comments(source)
        except ValueError as error:
            return [f"{path}: invalid JSONC: {error}"]
        source = strip_jsonc_trailing_commas(source)
    try:
        parsed = json.loads(source, object_pairs_hook=JsonObject)
    except json.JSONDecodeError as error:
        return [
            f"{path}:{error.lineno}:{error.colno}: invalid JSON: {error.msg}"
        ]
    return [
        f"{path}: duplicate JSON member at {member}"
        for member in duplicate_paths(parsed)
    ]


def maintained_inputs(source_root: Path) -> list[Path]:
    inputs: list[Path] = []
    for directory, directories, files in os.walk(source_root):
        current = Path(directory)
        if current == source_root:
            directories[:] = sorted(
                name
                for name in directories
                if name not in EXCLUDED_ROOT_DIRECTORIES
            )
        else:
            directories[:] = sorted(
                name for name in directories if name != "__pycache__"
            )
        for name in sorted(files):
            if Path(name).suffix.lower() in {".json", ".jsonc"}:
                inputs.append(current / name)
    return sorted(inputs)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument(
        "--source-root",
        type=Path,
        help=(
            "scan maintained inputs while excluding build, external, "
            "and Linux trees"
        ),
    )
    selection.add_argument(
        "--input",
        action="append",
        type=Path,
        help="validate an explicit input (repeatable)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.source_root is not None:
        source_root = arguments.source_root.resolve()
        if not source_root.is_dir():
            print(
                f"JSON input policy: source root is not a directory: "
                f"{source_root}",
                file=sys.stderr,
            )
            return 2
        inputs = maintained_inputs(source_root)
    else:
        inputs = [path.resolve() for path in arguments.input]
    errors = [error for path in inputs for error in validate(path)]
    if errors:
        print("JSON input policy failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print(f"validated {len(inputs)} first-party JSON/JSONC inputs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
