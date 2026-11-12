#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the starter ELF audit against synthetic ELF trees."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import importlib.util
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

PT_LOAD, PT_DYNAMIC, PT_INTERP = 1, 2, 3
ET_REL, ET_EXEC, ET_DYN = 1, 2, 3
EM_ARM, EM_AARCH64 = 40, 183
EABI5_HARD = 0x05000400
EABI5_SOFT = 0x05000200
EABI4_HARD = 0x04000400
R, RX, RW = 4, 5, 6
STARTER = Path(__file__).resolve().parents[3] / "tools/rootfs/starter"


def load_audit():
    spec = importlib.util.spec_from_file_location(
        "audit_elf", STARTER / "audit-elf.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


AUDIT = load_audit()


def ident() -> bytes:
    return b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)


def elf32(kind=ET_DYN, machine=EM_ARM, flags=EABI5_HARD, base=0x10000,
          align=0x10000, extra=(), interp=None, needed=(), runpath=()):
    """Return a little-endian ELF32 whose first PT_LOAD spans the file.

    The segment maps file offset 0 at `base`, so `base` modulo the page size
    decides its congruence. `extra` appends (offset, vaddr, size, flags,
    align) segments that map nothing the audit reads."""
    strings = bytearray(b"\0")
    entries = []
    for name in needed:
        entries.append((1, len(strings)))
        strings += name.encode() + b"\0"
    if runpath:
        entries.append((29, len(strings)))
        strings += ":".join(runpath).encode() + b"\0"
    interpreter = interp.encode() + b"\0" if interp else b""
    has_dynamic = bool(entries)
    count = 1 + len(extra) + bool(interp) + has_dynamic
    interp_offset = 52 + 32 * count
    strtab_offset = interp_offset + len(interpreter)
    dynamic_offset = strtab_offset + len(strings)
    dynamic = b"".join(struct.pack("<iI", tag, value) for tag, value in entries)
    dynamic += struct.pack("<iI", 5, base + strtab_offset)
    dynamic += struct.pack("<iI", 10, len(strings))
    dynamic += struct.pack("<iI", 0, 0)
    total = dynamic_offset + (len(dynamic) if has_dynamic else 0)
    phdrs = [struct.pack("<IIIIIIII", PT_LOAD, 0, base, base, total, total,
                         RX, align)]
    for offset, vaddr, size, segment_flags, segment_align in extra:
        phdrs.append(struct.pack("<IIIIIIII", PT_LOAD, offset, vaddr, vaddr,
                                 size, size, segment_flags, segment_align))
    if interp:
        phdrs.append(struct.pack("<IIIIIIII", PT_INTERP, interp_offset,
                                 base + interp_offset, 0, len(interpreter),
                                 len(interpreter), R, 1))
    if has_dynamic:
        phdrs.append(struct.pack("<IIIIIIII", PT_DYNAMIC, dynamic_offset,
                                 base + dynamic_offset, 0, len(dynamic),
                                 len(dynamic), RW, 4))
    header = ident() + struct.pack("<HHIIIIIHHHHHH", kind, machine, 1, 0, 52,
                                   0, flags, 52, 32, len(phdrs), 0, 0, 0)
    body = header + b"".join(phdrs) + interpreter + bytes(strings)
    if has_dynamic:
        body += dynamic
    assert len(body) == total
    return body


def relocatable() -> bytes:
    return ident() + struct.pack("<HHIIIIIHHHHHH", ET_REL, EM_ARM, 1, 0, 0, 0,
                                 EABI5_HARD, 52, 0, 0, 0, 0, 0)


class Options(argparse.Namespace):
    machine = "arm"
    bits = 32
    closed = True


class AuditTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="uml-elf-audit-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name, "root")
        multiarch = self.root / "usr/lib/arm-linux-gnueabihf"
        multiarch.mkdir(parents=True)
        (self.root / "usr/bin").mkdir()
        (self.root / "etc").mkdir()
        # A merged-/usr tree: /lib is a link, and the loader is a link in it.
        (self.root / "lib").symlink_to("usr/lib")
        (multiarch / "ld-linux-armhf.so.3").write_bytes(elf32())
        (self.root / "usr/lib/ld-linux-armhf.so.3").symlink_to(
            "arm-linux-gnueabihf/ld-linux-armhf.so.3")
        (multiarch / "libc.so.6").write_bytes(
            elf32(needed=("ld-linux-armhf.so.3",)))

    def put(self, relative: str, data: bytes) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def audit(self, **overrides):
        options = Options()
        for name, value in overrides.items():
            setattr(options, name, value)
        return AUDIT.audit_tree(self.root, options)

    def problems(self, **overrides) -> dict[str, list[str]]:
        return {row.path: row.problems for row in self.audit(**overrides)
                if row.problems}

    def test_a_closed_congruent_tree_passes(self) -> None:
        self.put("usr/bin/tool", elf32(
            interp="/lib/ld-linux-armhf.so.3", needed=("libc.so.6",)))
        self.put("usr/bin/script", b"#!/bin/sh\n")
        (self.root / "usr/bin/link").symlink_to("tool")
        rows = {row.path: row for row in self.audit()}
        self.assertEqual(self.problems(), {})
        self.assertEqual(sorted(rows), [
            "usr/bin/tool",
            "usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3",
            "usr/lib/arm-linux-gnueabihf/libc.so.6",
        ])
        self.assertEqual(
            AUDIT.format_row(rows["usr/bin/tool"]),
            "usr/bin/tool\t32\tarm\tdyn\teabi5-hard\t"
            "/lib/ld-linux-armhf.so.3\t0x10000\t1\t4k\tok")

    def test_a_segment_not_congruent_to_the_page_fails(self) -> None:
        self.put("usr/bin/odd", elf32(ET_EXEC, base=0x10800, align=0x1000))
        self.assertEqual(self.problems(),
                         {"usr/bin/odd": ["not-congruent"]})
        row, = [r for r in self.audit() if r.path == "usr/bin/odd"]
        self.assertEqual(AUDIT.format_row(row).split("\t")[8:],
                         ["none", "not-congruent"])

    def test_a_small_alignment_that_still_maps_passes(self) -> None:
        # Address 0x11000 against file offset 0 differs by one 4 KiB page.
        self.put("usr/bin/small", elf32(ET_EXEC, base=0x14000, align=0x1000))
        self.put("usr/bin/offset", elf32(ET_EXEC, base=0x11000, align=0x1000))
        self.assertEqual(self.problems(), {})

    def test_segments_of_different_permissions_may_not_share_a_page(self) -> None:
        # The data segment starts 2 KiB after the text and in the same page.
        self.put("usr/bin/shared", elf32(
            ET_EXEC, extra=[(0x800, 0x10800, 0x400, RW, 0x1000)]))
        self.assertEqual(self.problems(),
                         {"usr/bin/shared": ["shares-page"]})
        # 4 KiB after the text it is in the next page.
        self.put("usr/bin/shared", elf32(
            ET_EXEC, extra=[(0x1000, 0x11000, 0x400, RW, 0x1000)]))
        self.assertEqual(self.problems(), {})

    def test_soft_float_wrong_machine_and_old_abi_fail(self) -> None:
        self.put("usr/bin/soft", elf32(ET_EXEC, flags=EABI5_SOFT))
        self.put("usr/bin/arm64", elf32(ET_EXEC, machine=EM_AARCH64))
        self.put("usr/bin/old", elf32(ET_EXEC, flags=EABI4_HARD))
        self.assertEqual(self.problems(), {
            "usr/bin/soft": ["soft-float"],
            "usr/bin/arm64": ["machine-aarch64"],
            "usr/bin/old": ["not-eabi5"],
        })

    def test_an_open_dynamic_link_fails_when_closure_is_required(self) -> None:
        self.put("usr/bin/lost", elf32(
            interp="/lib/ld-missing.so.3", needed=("libgone.so.1",)))
        self.assertEqual(self.problems(), {"usr/bin/lost": [
            "interpreter-missing:/lib/ld-missing.so.3",
            "needed-missing:libgone.so.1"]})
        self.assertEqual(self.problems(closed=False), {"usr/bin/lost": [
            "interpreter-missing:/lib/ld-missing.so.3"]})

    def test_a_library_a_runpath_reaches_satisfies_other_requests(self) -> None:
        # The loader holds a library once any object has loaded it, so a
        # plug-in beside it needs no RUNPATH of its own.
        self.put("usr/lib/helper/libshared.so", elf32())
        self.put("usr/lib/helper/libcore.so", elf32(needed=("libshared.so",)))
        self.assertEqual(self.problems(), {"usr/lib/helper/libcore.so": [
            "needed-missing:libshared.so"]})
        self.put("usr/bin/main", elf32(
            needed=("libshared.so",), runpath=("/usr/lib/helper",)))
        self.assertEqual(self.problems(), {})

    def test_runpath_origin_and_loader_configuration_find_libraries(self) -> None:
        self.put("usr/lib/private/libprivate.so", elf32())
        self.put("opt/app/lib/libapp.so.1", elf32())
        self.put("opt/app/bin/app", elf32(
            needed=("libprivate.so", "libapp.so.1"),
            runpath=("$ORIGIN/../lib", "/usr/lib/private")))
        self.assertEqual(self.problems(), {})
        self.put("srv/libconfigured.so.1", elf32())
        self.put("usr/bin/configured", elf32(needed=("libconfigured.so.1",)))
        self.assertEqual(self.problems(), {"usr/bin/configured": [
            "needed-missing:libconfigured.so.1"]})
        (self.root / "etc/ld.so.conf").write_text(
            "include /etc/ld.so.conf.d/*.conf\n")
        (self.root / "etc/ld.so.conf.d").mkdir()
        (self.root / "etc/ld.so.conf.d/srv.conf").write_text("/srv\n")
        self.assertEqual(self.problems(), {})

    def test_relocatable_files_have_no_segments_to_check(self) -> None:
        self.put("usr/lib/module.o", relocatable())
        rows = {row.path: row for row in self.audit()}
        self.assertEqual(rows["usr/lib/module.o"].problems, [])
        self.assertEqual(AUDIT.format_row(rows["usr/lib/module.o"]),
                         "usr/lib/module.o\t32\tarm\trel\teabi5-hard\t-\t-\t0\t"
                         "4k\tok")

    def test_a_truncated_or_big_endian_file_stops_the_audit(self) -> None:
        self.put("usr/bin/cut", elf32(ET_EXEC)[:40])
        with self.assertRaises(AUDIT.AuditError):
            self.audit()
        self.put("usr/bin/cut", b"\x7fELF" + bytes([1, 2, 1, 0]) + bytes(60))
        with self.assertRaises(AUDIT.AuditError):
            self.audit()

    def test_the_command_writes_a_sorted_report_and_fails_on_a_problem(self) -> None:
        self.put("usr/bin/bad", elf32(ET_EXEC, base=0x10800, align=0x1000))
        report = Path(self.directory.name, "report.tsv")
        arguments = ["--root", str(self.root), "--output", str(report),
                     "--machine", "arm", "--closed"]
        error = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(error):
            status = AUDIT.main(arguments)
        self.assertEqual(status, 1)
        lines = report.read_text().splitlines()
        self.assertEqual(lines, sorted(lines))
        self.assertIn("usr/bin/bad\t32\tarm\texec\teabi5-hard\t-\t0x1000\t1\t"
                      "none\tnot-congruent", lines)
        self.assertIn("usr/bin/bad: not-congruent", error.getvalue())
        (self.root / "usr/bin/bad").unlink()
        report.unlink()
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = AUDIT.main(arguments)
        self.assertEqual(status, 0)
        self.assertIn("audited 2 ELF files", output.getvalue())


if __name__ == "__main__":
    unittest.main()
