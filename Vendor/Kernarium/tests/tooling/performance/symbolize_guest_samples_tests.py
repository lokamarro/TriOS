#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Contract of the offline guest-sample symbolizer on a synthetic ELF."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import lzma
import pathlib
import struct
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = (pathlib.Path(__file__).resolve().parents[2] /
          "perf/symbolize_guest_samples.py")
SPEC = importlib.util.spec_from_file_location("symbolize_guest_samples", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
symbolize = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = symbolize  # dataclasses resolve their module by name
SPEC.loader.exec_module(symbolize)

TEXT_VADDR = 0x10000
# Two functions of eight instructions each, distinct words throughout.
FIRST = [0x91000400 + i for i in range(8)]
SECOND = [0xD1000400 + i for i in range(8)]


# (name, st_info, section index, value, size): two global functions in .text
# and an arm64 mapping symbol, which the symbolizer must ignore.
SYMBOLS = [("first", 0x12, 1, TEXT_VADDR, 32),
           ("second", 0x12, 1, TEXT_VADDR + 32, 32),
           ("$x", 0, 1, TEXT_VADDR, 0)]


def build_elf(symbols: list[tuple[str, int, int, int, int]] = SYMBOLS,
              debugdata: bytes | None = None, text: bool = True) -> bytes:
    """An ELF64 with .text (and its PT_LOAD) unless text is false, a .symtab,
    and a .gnu_debugdata section when debugdata is given."""
    code = struct.pack(f"<{len(FIRST + SECOND)}I", *(FIRST + SECOND)) \
        if text else b""
    strtab = b"\0"
    table = [struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)]
    for name, info, index, value, size in symbols:
        table.append(struct.pack("<IBBHQQ", len(strtab), info, 0, index,
                                 value, size))
        strtab += name.encode() + b"\0"
    table = b"".join(table)
    # Section names at offsets 1, 7, 15, 23 and 33.
    names = b"\0.text\0.symtab\0.strtab\0.shstrtab\0.gnu_debugdata\0"
    extra = debugdata or b""
    header_size, phdr_size, shdr_size = 64, 56, 64
    phnum = 1 if text else 0
    text_offset = header_size + phdr_size * phnum
    symtab_offset = text_offset + len(code)
    strtab_offset = symtab_offset + len(table)
    names_offset = strtab_offset + len(strtab)
    extra_offset = names_offset + len(names)
    shoff = extra_offset + len(extra)
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 6, TEXT_VADDR, text_offset, len(code), 0, 0, 4, 0),
        (7, 2, 0, 0, symtab_offset, len(table), 3, 1, 8, 24),
        (15, 3, 0, 0, strtab_offset, len(strtab), 0, 0, 1, 0),
        (23, 3, 0, 0, names_offset, len(names), 0, 0, 1, 0),
    ]
    if debugdata is not None:
        sections.append((33, 1, 0, 0, extra_offset, len(extra), 0, 0, 1, 0))
    header = b"\x7fELF" + bytes([2, 1, 1]) + bytes(9) + struct.pack(
        "<HHIQQQIHHHHHH", 3, 183, 1, TEXT_VADDR,
        header_size if text else 0, shoff, 0, header_size, phdr_size, phnum,
        shdr_size, len(sections), 4)
    phdr = struct.pack("<IIQQQQQQ", 1, 5, text_offset, TEXT_VADDR,
                       TEXT_VADDR, len(code), len(code), 0x1000) \
        if text else b""
    return (header + phdr + code + table + strtab + names + extra +
            b"".join(struct.pack("<IIQQQQIIQQ", *s) for s in sections))


def words(values: list[int]) -> str:
    return ",".join(f"{value:08x}" for value in values)


class SymbolizeGuestSamplesTests(unittest.TestCase):
    def test_blocks_are_placed_by_their_words_not_their_addresses(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "libdemo.so").write_bytes(build_elf())
            samples = root / "samples.tsv"
            # Guest addresses carry an arbitrary load bias; the third block's
            # words exist in no file.
            samples.write_text(
                "# guest code samples\tpasses 10\tsamples 10\tguest 10\n"
                f"guest\t6\t0x7f0000010010\t4\t{words(FIRST[4:8])}\t0x7000\n"
                f"guest\t3\t0x7f0000010020\t8\t{words(SECOND)}\t0x8000\n"
                f"guest\t1\t0x400000\t4\t{words([1, 2, 3, 4])}\t0x7000\n"
                "host\t5\tdarwin_idle\n")
            profile = symbolize.read_profile(samples)
            elves = [symbolize.read_elf(path)
                     for path in symbolize.elf_files([root])]
            located = symbolize.locate(profile.blocks, elves, 4)
            self.assertEqual(located[0][1], TEXT_VADDR + 16)
            self.assertEqual(located[1][1], TEXT_VADDR + 32)
            self.assertNotIn(2, located)
            self.assertEqual(symbolize.symbol_for(elves[0], TEXT_VADDR + 16),
                             "first+0x10")
            self.assertEqual(symbolize.symbol_for(elves[0], TEXT_VADDR + 36),
                             "second+0x4")

            output = io.StringIO()
            with contextlib.redirect_stdout(output), mock.patch.object(
                    sys, "argv", ["symbolize", str(samples), str(root)]):
                self.assertEqual(symbolize.main(), 0)
            report = output.getvalue()
            self.assertIn("located in 1 ELF files: 90.0%", report)
            self.assertIn("60.00%  first  [libdemo.so]", report)
            self.assertIn("30.00%  second  [libdemo.so]", report)
            self.assertIn("10.00%  0x400000  [[unmatched]]", report)
            self.assertIn("70.00%  0x7000", report)
            self.assertIn("30.00%  0x8000", report)

            output = io.StringIO()
            with contextlib.redirect_stdout(output), mock.patch.object(
                    sys, "argv", ["symbolize", str(samples), str(root),
                                  "--thread", "7000"]):
                self.assertEqual(symbolize.main(), 0)
            report = output.getvalue()
            self.assertIn("guest samples 7,", report)
            self.assertIn("85.71%  first  [libdemo.so]", report)
            self.assertNotIn("second", report)

    def test_baseline_leaves_the_interval_after_it(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            earlier = root / "earlier.tsv"
            later = root / "later.tsv"
            earlier.write_text(
                f"guest\t4\t0x1000\t4\t{words(FIRST[:4])}\t0x7000\n"
                f"guest\t2\t0x1000\t4\t{words(FIRST[:4])}\t0x8000\n"
                "host\t5\tdarwin_idle\n")
            later.write_text(
                f"guest\t9\t0x1000\t4\t{words(FIRST[:4])}\t0x7000\n"
                f"guest\t2\t0x1000\t4\t{words(FIRST[:4])}\t0x8000\n"
                f"guest\t3\t0x2000\t4\t{words(SECOND[:4])}\t0x8000\n"
                "host\t8\tdarwin_idle\n")
            profile = symbolize.subtract(symbolize.read_profile(later),
                                         symbolize.read_profile(earlier))
            self.assertEqual([(block.count, block.address, block.thread)
                              for block in profile.blocks],
                             [(5, 0x1000, 0x7000), (3, 0x2000, 0x8000)])
            self.assertEqual(profile.host, [(3, "darwin_idle")])

    def test_profiles_without_a_thread_column_still_read(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "old.tsv"
            path.write_text(f"guest\t1\t0x1000\t4\t{words(FIRST[:4])}\n")
            self.assertEqual(symbolize.read_profile(path).blocks[0].thread, 0)

    def test_minidebuginfo_names_hidden_functions(self) -> None:
        # A stripped Android library exports only `first`; `second`, a local
        # function, survives only in the xz-compressed .gnu_debugdata ELF.
        inner = build_elf([("second", 0x02, 1, TEXT_VADDR + 32, 32)],
                          text=False)
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            stripped = root / "libstripped.so"
            stripped.write_bytes(build_elf(
                [("first", 0x12, 1, TEXT_VADDR, 32)],
                debugdata=lzma.compress(inner, format=lzma.FORMAT_XZ)))
            elf = symbolize.read_elf(stripped)
            self.assertEqual(symbolize.symbol_for(elf, TEXT_VADDR + 16),
                             "first+0x10")
            self.assertEqual(symbolize.symbol_for(elf, TEXT_VADDR + 36),
                             "second+0x4")
            # A payload that is not xz leaves the exported symbols alone.
            damaged = root / "libdamaged.so"
            damaged.write_bytes(build_elf(
                [("first", 0x12, 1, TEXT_VADDR, 32)], debugdata=b"not xz"))
            elf = symbolize.read_elf(damaged)
            self.assertEqual(symbolize.symbol_for(elf, TEXT_VADDR + 16),
                             "first+0x10")
            self.assertEqual(symbolize.symbol_for(elf, TEXT_VADDR + 36),
                             f"{TEXT_VADDR + 36:#x}")

    def test_short_blocks_are_not_guessed(self) -> None:
        block = symbolize.Block(1, 0x1000, 2, tuple(FIRST[:2]))
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "libdemo.so"
            path.write_bytes(build_elf())
            elf = symbolize.read_elf(path)
            self.assertEqual(symbolize.locate([block], [elf], 4), {})


if __name__ == "__main__":
    unittest.main()
