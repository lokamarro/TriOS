#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/kernel-dsym-symbols.py gives a dSYM the kernel's function symbols.

The tests link a real Mach-O that carries a synthetic kernel ELF as its
__TEXT,__uml_elf section, make its dSYM with dsymutil, add the symbols and then
read the result with the toolchain's own readers, including the symbolicator.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import struct
import sys
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TOOL = ROOT / "scripts/kernel-dsym-symbols.py"
VADDR = 0xC00000000
TEXT_OFFSET = 0x1000


def elf_with_functions(functions, objects=(), outside=()):
    """A minimal AArch64 ET_DYN image. `functions` are (name, text offset)
    pairs placed in the executable segment at TEXT_OFFSET; `objects` are data
    symbols in it, and `outside` functions in a read-only segment."""
    text = bytes(0x400)
    rodata_offset = TEXT_OFFSET + 0x1000
    rodata = bytes(0x100)
    strtab = bytearray(b"\0")
    symbols = [struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)]

    def add(name, kind, value, size=0x20):
        symbols.append(struct.pack("<IBBHQQ", len(strtab), kind, 0, 1, value, size))
        strtab.extend(name.encode() + b"\0")

    for name, offset in functions:
        add(name, 2, VADDR + TEXT_OFFSET + offset)
    for name, offset in objects:
        add(name, 1, VADDR + TEXT_OFFSET + offset)
    for name, offset in outside:
        add(name, 2, VADDR + rodata_offset + offset)
    undefined = struct.pack("<IBBHQQ", len(strtab), 2, 0, 0, 0, 0)
    strtab.extend(b"undefined_function\0")
    symbols.append(undefined)
    symtab = b"".join(symbols)
    shstrtab = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
    body = bytearray(TEXT_OFFSET)
    body += text
    body += bytes(rodata_offset - len(body))
    body += rodata
    symtab_offset = len(body)
    body += symtab
    strtab_offset = len(body)
    body += strtab
    shstrtab_offset = len(body)
    body += shstrtab
    body += bytes(-len(body) % 8)
    section_offset = len(body)

    def section(name, kind, flags, addr, offset, size, link=0, info=0, entsize=0):
        return struct.pack("<IIQQQQIIQQ", name, kind, flags, addr, offset, size,
                           link, info, 8, entsize)

    sections = (
        section(0, 0, 0, 0, 0, 0)
        + section(1, 1, 6, VADDR + TEXT_OFFSET, TEXT_OFFSET, len(text))
        + section(7, 2, 0, 0, symtab_offset, len(symtab), link=3, entsize=24)
        + section(15, 3, 0, 0, strtab_offset, len(strtab))
        + section(23, 3, 0, 0, shstrtab_offset, len(shstrtab))
    )
    body += sections
    program_headers = struct.pack(
        "<IIQQQQQQ", 1, 5, 0, VADDR, VADDR, TEXT_OFFSET + len(text),
        TEXT_OFFSET + len(text), 0x1000
    ) + struct.pack(
        "<IIQQQQQQ", 1, 4, rodata_offset, VADDR + rodata_offset,
        VADDR + rodata_offset, len(rodata), len(rodata), 0x1000
    )
    header = (
        b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
        + struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, VADDR + TEXT_OFFSET, 64,
                      section_offset, 0, 64, 56, 2, 64, 5, 4)
    )
    image = bytearray(header + program_headers)
    image += bytes(TEXT_OFFSET - len(image))
    image += body[TEXT_OFFSET:]
    return bytes(image)


def run(*command, check=True):
    result = subprocess.run(
        [str(part) for part in command], stdin=subprocess.DEVNULL,
        capture_output=True, text=True, timeout=120, check=False)
    if check and result.returncode != 0:
        raise AssertionError(
            f"{command[0]} failed ({result.returncode}): {result.stderr}")
    return result


class KernelDsymSymbolsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory(prefix="kernel dsym symbols ")
        cls.addClassCleanup(cls.scratch.cleanup)
        cls.root = Path(cls.scratch.name)
        cls.functions = [("alpha", 0x0), ("beta", 0x40), ("dup", 0x80),
                         ("dup", 0xC0), ("_leading", 0x100), ("dup", 0x140)]
        cls.elf = cls.root / "kernel.elf"
        cls.elf.write_bytes(elf_with_functions(
            cls.functions, objects=[("not_a_function", 0x180)],
            outside=[("in_rodata", 0x10)]))
        source = cls.root / "host.c"
        source.write_text("int main(void) { return 0; }\n")
        cls.binary = cls.root / "host"
        run("xcrun", "clang", "-arch", "arm64", "-g", "-o", cls.binary, source,
            f"-Wl,-sectcreate,__TEXT,__uml_elf,{cls.elf}",
            "-Wl,-sectalign,__TEXT,__uml_elf,0x4000")

    def make_dsym(self, name="host"):
        dsym = self.root / f"{name}.dSYM"
        run("rm", "-rf", dsym)
        run("xcrun", "dsymutil", self.binary, "-o", dsym)
        return dsym, dsym / "Contents/Resources/DWARF/host"

    def section_address(self, path):
        text = run("xcrun", "otool", "-l", path).stdout
        match = re.search(
            r"sectname __uml_elf\n\s+segname __TEXT\n\s+addr (0x[0-9a-f]+)\n"
            r"\s+size (0x[0-9a-f]+)", text)
        self.assertIsNotNone(match)
        return int(match.group(1), 16), int(match.group(2), 16)

    def segment_address(self, path):
        text = run("xcrun", "otool", "-l", path).stdout
        match = re.search(r"segname __TEXT\n\s+vmaddr (0x[0-9a-f]+)", text)
        return int(match.group(1), 16)

    def symbols(self, path):
        names = {}
        for line in run("xcrun", "nm", "-n", path).stdout.splitlines():
            parts = line.split()
            if len(parts) == 3:
                names[parts[2]] = int(parts[0], 16)
        return names

    def add_symbols(self, dwarf, elf=None, check=True):
        return run(sys.executable, "-B", TOOL, "--dsym", dwarf,
                   "--elf", elf or self.elf, check=check)

    def test_functions_appear_at_the_sections_addresses(self):
        _, dwarf = self.make_dsym()
        before = self.symbols(dwarf)
        self.add_symbols(dwarf)
        section, size = self.section_address(dwarf)
        self.assertEqual(size, self.elf.stat().st_size)
        after = self.symbols(dwarf)
        base = section + TEXT_OFFSET
        self.assertEqual(after["_alpha"], base)
        self.assertEqual(after["_beta"], base + 0x40)
        self.assertEqual(after["_dup"], base + 0x80)
        self.assertEqual(after["_dup~2"], base + 0xC0)
        self.assertEqual(after["_dup~3"], base + 0x140)
        self.assertEqual(after["__leading"], base + 0x100)
        self.assertEqual(after["___uml_kernel_symbols"], section)
        for name in ("_not_a_function", "_in_rodata", "_undefined_function"):
            self.assertNotIn(name, after, "only executable functions are named")
        self.assertTrue(set(before.items()) <= set(after.items()),
                        "the symbols the dSYM had are kept")
        verify = run("xcrun", "dwarfdump", "--verify", dwarf)
        self.assertIn("No errors", verify.stdout + verify.stderr)
        self.assertEqual(run("xcrun", "dwarfdump", "--uuid", dwarf).stdout.split()[1],
                         run("xcrun", "dwarfdump", "--uuid", self.binary).stdout.split()[1])

    def test_the_symbolicator_names_the_kernel_function(self):
        _, dwarf = self.make_dsym()
        self.add_symbols(dwarf)
        section, _ = self.section_address(dwarf)
        load = self.segment_address(dwarf)
        base = section + TEXT_OFFSET

        def symbolicate(address):
            return run("xcrun", "atos", "-o", dwarf, "-arch", "arm64",
                       "-l", hex(load), hex(address)).stdout.strip()

        self.assertRegex(symbolicate(base + 0x44), r"^beta \(in host\) \+ 4$")
        self.assertRegex(symbolicate(base + 0xC8), r"^dup~2 \(in host\) \+ 8$")
        self.assertRegex(symbolicate(base + 0x104), r"^_leading \(in host\) \+ 4$")

    def test_the_linked_binary_is_not_given_the_symbols(self):
        _, dwarf = self.make_dsym()
        self.add_symbols(dwarf)
        names = self.symbols(self.binary)
        self.assertNotIn("_alpha", names)
        self.assertNotIn("_dup~2", names)

    def test_it_is_idempotent(self):
        _, dwarf = self.make_dsym()
        self.add_symbols(dwarf)
        once = dwarf.read_bytes()
        self.add_symbols(dwarf)
        self.assertEqual(dwarf.read_bytes(), once)

    def test_it_refuses_a_mismatched_elf(self):
        _, dwarf = self.make_dsym()
        original = dwarf.read_bytes()
        other = self.root / "other.elf"
        other.write_bytes(self.elf.read_bytes() + bytes(8))
        result = self.add_symbols(dwarf, other, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("__uml_elf section", result.stderr)
        self.assertEqual(dwarf.read_bytes(), original, "a refusal leaves the dSYM alone")

    def test_it_refuses_a_dsym_without_the_section(self):
        source = self.root / "plain.c"
        source.write_text("int main(void) { return 0; }\n")
        plain = self.root / "plain"
        run("xcrun", "clang", "-arch", "arm64", "-g", "-o", plain, source)
        run("xcrun", "dsymutil", plain, "-o", self.root / "plain.dSYM")
        result = self.add_symbols(
            self.root / "plain.dSYM/Contents/Resources/DWARF/plain",
            check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no __TEXT,__uml_elf section", result.stderr)

    def test_it_refuses_an_elf_without_functions(self):
        _, dwarf = self.make_dsym()
        original = dwarf.read_bytes()
        empty = self.root / "empty.elf"
        image = elf_with_functions([])
        # The same size as the linked section, so only the symbols are wrong.
        empty.write_bytes(image + bytes(self.elf.stat().st_size - len(image)))
        result = self.add_symbols(dwarf, empty, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no function symbols", result.stderr)
        self.assertEqual(dwarf.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
