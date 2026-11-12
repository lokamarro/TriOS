#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that a Darwin UML kernel image imports exactly the active bridge.

The image must import every darwin_uml_* function declared in this
directory's darwin_uml_*.h headers, except those import-policy.tsv limits to
the arm64 slice, and none of the host functions that host code must not call
directly. Kernel builds run this command on the built image; the kernel
artifact verifier applies the same checks to a bundle.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import struct
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import NamedTuple

Error = Callable[[str], BaseException]

ABI_ROOT = Path(__file__).resolve().parent
BRIDGE_SYMBOL_RE = re.compile(r"darwin_uml_[A-Za-z0-9_]+")
DECLARATION_RE = re.compile(r"\b(darwin_uml_[A-Za-z0-9_]+)\s*\(")
POLICY_HEADER = "symbol\tavailability\trationale"
FORBIDDEN_IMPORTS = (
    # Darwin's sigprocmask() sets the mask of every thread in the process,
    # vCPU carriers included; host code must change a mask with
    # pthread_sigmask().
    (re.compile(r"_?sigprocmask(?:\$.*)?"), "the process-wide Darwin sigprocmask"),
    # Apple's arm64 ABI passes variadic arguments on the stack and the
    # kernel's host objects pass them in registers, so a direct fcntl() hands
    # libc an arbitrary third argument; host code calls darwin_uml_fcntl().
    (re.compile(r"_?fcntl(?:\$.*)?"), "the variadic Darwin fcntl"),
)

SHT_SYMTAB = 2
SHT_NOBITS = 8
SHT_DYNSYM = 11
SHN_UNDEF = 0


def bounded_slice(
    payload: bytes, offset: int, size: int, label: str, error: Error = ValueError
) -> bytes:
    if offset < 0 or size < 0 or offset > len(payload) or size > len(payload) - offset:
        raise error(f"ELF {label} is outside the file")
    return payload[offset : offset + size]


class SectionHeader(NamedTuple):
    name: int
    kind: int
    flags: int
    offset: int
    size: int
    link: int
    entry_size: int


def section_headers(payload: bytes, error: Error = ValueError) -> list[SectionHeader]:
    """Return every ELF64 section header, each with contents inside the file."""
    section_offset = struct.unpack_from("<Q", payload, 40)[0]
    section_entry_size, section_count = struct.unpack_from("<HH", payload, 58)
    if section_entry_size < 64 or section_count == 0:
        raise error("ELF section-header table is missing or malformed")
    bounded_slice(
        payload,
        section_offset,
        section_entry_size * section_count,
        "section-header table",
        error,
    )
    headers = []
    for index in range(section_count):
        offset = section_offset + index * section_entry_size
        name, kind, flags = struct.unpack_from("<IIQ", payload, offset)
        file_offset, size = struct.unpack_from("<QQ", payload, offset + 24)
        link = struct.unpack_from("<I", payload, offset + 40)[0]
        entry_size = struct.unpack_from("<Q", payload, offset + 56)[0]
        if kind != SHT_NOBITS:
            bounded_slice(payload, file_offset, size, f"section {index}", error)
        headers.append(
            SectionHeader(name, kind, flags, file_offset, size, link, entry_size)
        )
    return headers


def undefined_symbols(payload: bytes, error: Error = ValueError) -> set[str]:
    """Return every ASCII undefined symbol name in the ELF64 symbol tables."""
    sections = section_headers(payload, error)
    undefined: set[str] = set()
    found_symbol_table = False
    for index, section in enumerate(sections):
        if section.kind not in (SHT_SYMTAB, SHT_DYNSYM):
            continue
        found_symbol_table = True
        entry_size = section.entry_size
        if entry_size < 24 or section.size % entry_size != 0:
            raise error(f"ELF symbol table {index} is malformed")
        if section.link >= len(sections):
            raise error(f"ELF symbol table {index} has an invalid string table")
        string_table = sections[section.link]
        strings = bounded_slice(
            payload, string_table.offset, string_table.size, "string table", error
        )
        end = section.offset + section.size
        for symbol_offset in range(section.offset, end, entry_size):
            name_offset = struct.unpack_from("<I", payload, symbol_offset)[0]
            section_index = struct.unpack_from("<H", payload, symbol_offset + 6)[0]
            if section_index != SHN_UNDEF or name_offset >= len(strings):
                continue
            terminator = strings.find(b"\0", name_offset)
            if terminator < 0:
                raise error("ELF symbol name is unterminated")
            try:
                name = strings[name_offset:terminator].decode("ascii")
            except UnicodeDecodeError:
                continue
            if name:
                undefined.add(name)
    if not found_symbol_table:
        raise error("ELF contains no symbol table for bridge verification")
    return undefined


def expected_imports(
    kernel_slice: str,
    abi_root: Path = ABI_ROOT,
    error: Error = ValueError,
) -> set[str]:
    """Return the bridge symbols a kernel of this slice imports."""
    if kernel_slice not in {"arm64", "x86_64"}:
        raise error(f"unsupported kernel slice for bridge imports: {kernel_slice}")
    declared: set[str] = set()
    for path in sorted(abi_root.glob("darwin_uml_*.h")):
        declared.update(DECLARATION_RE.findall(path.read_text(encoding="utf-8")))
    if not declared:
        raise error("shared ABI declarations are missing")
    conditional: dict[str, set[str]] = {"arm64-only": set()}
    policy = abi_root / "import-policy.tsv"
    if not policy.is_file():
        raise error(f"ABI import policy is missing: {policy}")
    rows = policy.read_text(encoding="utf-8").splitlines()
    if not rows or rows[0] != POLICY_HEADER:
        raise error("ABI import policy is malformed")
    for line_number, row in enumerate(rows[1:], 2):
        fields = row.split("\t")
        if (
            len(fields) != 3
            or BRIDGE_SYMBOL_RE.fullmatch(fields[0]) is None
            or fields[1] not in conditional
            or not fields[2]
            or any(fields[0] in symbols for symbols in conditional.values())
        ):
            raise error(f"ABI import policy is malformed at line {line_number}")
        conditional[fields[1]].add(fields[0])
    undeclared = set().union(*conditional.values()) - declared
    if undeclared:
        raise error(
            f"ABI import policy names undeclared bridge symbols: {sorted(undeclared)}"
        )
    expected = set(declared)
    if kernel_slice == "x86_64":
        expected -= conditional["arm64-only"]
    return expected


def verify_imports(
    undefined: Iterable[str],
    kernel_slice: str,
    abi_root: Path = ABI_ROOT,
    error: Error = ValueError,
) -> None:
    """Require an image with these undefined symbols to import the active bridge."""
    expected = expected_imports(kernel_slice, abi_root, error)
    undefined = set(undefined)
    for pattern, description in FORBIDDEN_IMPORTS:
        if any(pattern.fullmatch(name) for name in undefined):
            raise error(f"the Linux image imports {description}")
    imported = {name for name in undefined if BRIDGE_SYMBOL_RE.fullmatch(name)}
    if imported != expected:
        raise error(
            "kernel bridge imports differ "
            f"(missing={sorted(expected - imported)}, "
            f"extra={sorted(imported - expected)})"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--kernel-binary", type=Path, help="ELF image to check")
    source.add_argument("--nm-output", type=Path, help="`nm -u` listing to check")
    source.add_argument(
        "--print-expected",
        action="store_true",
        help="print the bridge symbols an image must import",
    )
    parser.add_argument(
        "--slice", choices=("arm64", "x86_64"), required=True, dest="kernel_slice"
    )
    args = parser.parse_args()

    def error(message: str) -> SystemExit:
        return SystemExit(f"kernel/common/abi/bridge_imports.py: {message}")

    if args.print_expected:
        for symbol in sorted(expected_imports(args.kernel_slice, error=error)):
            print(symbol)
        return
    if args.kernel_binary is not None:
        if not args.kernel_binary.is_file():
            raise error(f"kernel binary is missing: {args.kernel_binary}")
        undefined = undefined_symbols(args.kernel_binary.read_bytes(), error)
    else:
        if not args.nm_output.is_file():
            raise error(f"nm output is missing: {args.nm_output}")
        undefined = set(args.nm_output.read_bytes().decode("utf-8", "replace").split())
    verify_imports(undefined, args.kernel_slice, error=error)
    print(
        f"verified {args.kernel_slice} Linux imports exactly "
        "match the active synchronized bridge"
    )


if __name__ == "__main__":
    main()
