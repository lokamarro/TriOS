#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Value and metadata checks shared by the kernel-artifact and Android release verifiers."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import re
from collections.abc import Callable
from pathlib import Path, PurePosixPath
from typing import NoReturn

HEX40_RE = re.compile(r"^[0-9a-f]{40}$")
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
ASSIGNMENT_RE = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([0-9]+))$")


def parse_assignments(
    path: Path, error: Callable[[str], BaseException] = ValueError
) -> dict[str, str]:
    """Parse KEY='value' and KEY=digits lines, raising error(message) on anything else."""
    if not path.is_file() or path.is_symlink():
        raise error(f"metadata file is missing or unsafe: {path}")
    values: dict[str, str] = {}
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = ASSIGNMENT_RE.fullmatch(line)
        if match is None:
            raise error(f"{path}:{line_number}: malformed metadata assignment")
        key, quoted, numeric = match.groups()
        if key in values:
            raise error(f"{path}:{line_number}: duplicate metadata key {key}")
        values[key] = quoted if quoted is not None else numeric
    return values


def accepted_directory_name(
    path: Path, key: str, error: Callable[[str], BaseException] = ValueError
) -> str:
    """Return the single directory name an acceptance descriptor assigns to key."""
    value = parse_assignments(path, error).get(key)
    if value is None:
        raise error(f"{path} does not define {key}")
    if value in ("", ".", "..") or PurePosixPath(value).name != value:
        raise error(f"unsafe {key} in {path}")
    return value


def replace_assignment(
    text: str, key: str, value: str, error: Callable[[str], BaseException] = ValueError
) -> str:
    """Replace the one assignment of key in metadata text."""
    pattern = re.compile(rf"^{re.escape(key)}=(?:'[^']*'|[0-9]+)$", re.MULTILINE)
    updated, count = pattern.subn(f"{key}={value}", text)
    if count != 1:
        raise error(f"metadata does not define {key} exactly once")
    return updated


class ContractChecks:
    """Checks that fail with one verifier's message prefix."""

    def __init__(self, prefix: str) -> None:
        self.prefix = prefix

    def error(self, message: str) -> SystemExit:
        return SystemExit(f"{self.prefix}: {message}")

    def fail(self, message: str) -> NoReturn:
        raise self.error(message)

    def require_exact_keys(self, value: object, expected: set[str], label: str) -> dict:
        if not isinstance(value, dict):
            self.fail(f"{label} must be an object")
        keys = set(value)
        if keys != expected:
            missing = sorted(expected - keys)
            extra = sorted(keys - expected)
            self.fail(f"{label} keys differ (missing={missing}, extra={extra})")
        return value

    def require_string(self, value: object, label: str) -> str:
        if not isinstance(value, str) or not value:
            self.fail(f"{label} must be a nonempty string")
        if "\0" in value or "\n" in value or "\r" in value:
            self.fail(f"{label} contains a control character")
        return value

    def require_integer(self, value: object, label: str, *, minimum: int = 0) -> int:
        if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
            self.fail(f"{label} must be an integer at least {minimum}")
        return value

    def require_hex(self, value: object, length: int, label: str) -> str:
        result = self.require_string(value, label)
        pattern = HEX40_RE if length == 40 else HEX64_RE
        if pattern.fullmatch(result) is None:
            self.fail(f"{label} must be {length} lowercase hexadecimal characters")
        return result

    def safe_relative(self, value: object, label: str) -> str:
        text = self.require_string(value, label)
        if "\\" in text:
            self.fail(f"{label} contains a backslash")
        path = PurePosixPath(text)
        if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
            self.fail(f"{label} is not a normalized relative path")
        if str(path) != text:
            self.fail(f"{label} is not normalized")
        return text

    def parse_env(self, path: Path) -> dict[str, str]:
        return parse_assignments(path, self.error)


def sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def parse_config(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        if raw.startswith("CONFIG_") and "=" in raw:
            key, value = raw.split("=", 1)
            values[key] = value
        elif raw.startswith("# CONFIG_") and raw.endswith(" is not set"):
            values[raw[2:-11]] = "n"
    return values
