#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the shared Mach-O reader and the tests that use it on small files.

The gadget-layout, gadget-label and bundle-identifier tests read built
images, which a tooling tree lacks. A synthetic file holds the segment,
sections and symbols the reader exists to find, so this checks its results
and refusals, then runs the label and identifier tests on files that pass
and on files that break their rules.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import plistlib
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import macho  # noqa: E402

STAB = 0x24
NOT_MACHO = b"not a Mach-O file" * 4


def build(sections, symbols, filetype=macho.MH_OBJECT, symtab=True) -> bytes:
    """A file of __TEXT sections (name, bytes) and symbols (name, type,
    value), without a symbol table when symtab is false."""
    size = 72 + 80 * len(sections)
    base = macho.HEADER_SIZE + size + 24 * symtab
    body, entries = b"", b""
    for name, data in sections:
        entries += struct.pack("<16s16sQQIIIIIIII", name.encode(), b"__TEXT",
                               0x1000 + len(body), len(data),
                               base + len(body), 2, 0, 0, 0, 0, 0, 0)
        body += data
    commands = struct.pack("<II16sQQQQIIII", macho.LC_SEGMENT_64, size,
                           b"__TEXT", 0x1000, len(body), base, len(body),
                           5, 5, len(sections), 0) + entries
    strings, entries = b"\0", b""
    for name, kind, value in symbols:
        entries += struct.pack("<IBBHQ", len(strings), kind, 1, 0, value)
        strings += name.encode() + b"\0"
    if symtab:
        commands += struct.pack("<IIIIII", macho.LC_SYMTAB, 24,
                                base + len(body), len(symbols),
                                base + len(body) + len(entries), len(strings))
    header = struct.pack("<IiiIIIII", macho.MH_MAGIC_64, 0x100000C, 0,
                         filetype, 1 + symtab, len(commands), 0, 0)
    return header + commands + body + entries + strings


def run(script: str, *arguments) -> tuple[int, str]:
    result = subprocess.run(
        [sys.executable, "-B", HERE.parent / script, *arguments],
        capture_output=True, text=True, timeout=60)
    return result.returncode, result.stdout + result.stderr


def check_reader() -> None:
    data = build([("__text", b"\1\0\0\0\2\0\0\0"), ("__info_plist", b"plist")],
                 [("_main", 0x0F, 0x1000), ("_main", 0x0F, 0x2000),
                  ("_debug", STAB, 0x3000), (".Lx", 0x0E, 0x1004)])
    image = macho.MachO(data, "synthetic")
    text = image.sections[("__TEXT", "__text")]
    assert image.filetype == macho.MH_OBJECT
    assert image.segments == {"__TEXT": 0x1000}
    assert (text.address, text.size, text.alignment) == (0x1000, 8, 2)
    assert image.words(text, 0x1004, 0x1008) == (2,)
    assert image.section_bytes("__TEXT", "__info_plist") == b"plist"
    assert image.section_bytes("__TEXT", "__absent") is None
    # Debug symbols are skipped, the table's order and repeats are kept, and
    # the first definition of a name wins.
    assert image.symbol_names == ["_main", "_main", ".Lx"]
    assert image.symbols == {"_main": 0x1000, ".Lx": 0x1004}
    assert macho.MachO(build([], [], symtab=False), "").symbol_names is None
    for refused in (b"", data[:31], NOT_MACHO):
        try:
            macho.MachO(refused, "refused")
        except ValueError as error:
            assert str(error) == "refused: not a thin 64-bit Mach-O file"
        else:
            raise AssertionError("a bad file was accepted")


def check_labels(directory: Path) -> None:
    code = [("__text", b"\0" * 4)]
    for rc, expected, data in (
            (0, "2 symbols, none named .L", build(
                code, [("_f", 0x0F, 0x1000), ("Lkeep", 0x0E, 0)])),
            (1, "names 1 .L symbol", build(code, [(".Lfoo", 0x0E, 0x1000)])),
            (1, "not a thin 64-bit Mach-O object", build(
                code, [("_f", 0x0F, 0x1000)], filetype=0x2)),
            (1, "not a thin 64-bit Mach-O file", NOT_MACHO),
            (1, "no symbol table", build([], [], symtab=False)),
            (1, "empty symbol table", build([], []))):
        path = directory / "engine.o"
        path.write_bytes(data)
        result = run("arm64/test_gadget_engine_labels.py", "target", path)
        assert result[0] == rc and expected in result[1], result


def check_identity(directory: Path) -> None:
    plist = plistlib.dumps({"CFBundleIdentifier": "com.example.app",
                            "CFBundleName": "uml-darwin"})
    with_plist = build([("__info_plist", plist)], [])
    for rc, expected, identifier, name, data in (
            (0, "", "com.example.app", "uml-darwin", with_plist),
            (1, "CFBundleIdentifier is 'com.example.app', not 'com.example.x'",
             "com.example.x", "uml-darwin", with_plist),
            (1, "CFBundleName is 'uml-darwin', not 'renamed'",
             "com.example.app", "renamed", with_plist),
            (1, "is not reverse-DNS", "no identifier", "uml-darwin",
             with_plist),
            (1, "has no __TEXT,__info_plist", "com.example.app", "uml-darwin",
             build([("__text", b"\0" * 4)], [])),
            (1, "not a thin 64-bit Mach-O file", "com.example.app",
             "uml-darwin", NOT_MACHO)):
        path = directory / name
        path.write_bytes(data)
        result = run("emulator/test_macos_app_identity.py",
                     "--identifier", identifier, "--executable", path)
        assert result[0] == rc and expected in result[1], result


def main() -> int:
    check_reader()
    with tempfile.TemporaryDirectory() as temporary:
        check_labels(Path(temporary))
        check_identity(Path(temporary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
