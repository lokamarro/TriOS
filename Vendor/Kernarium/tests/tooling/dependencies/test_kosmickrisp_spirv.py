#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import base64
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


REPO_ROOT = Path(__file__).resolve().parents[3]
GENERATOR = REPO_ROOT / "dependencies/kosmickrisp-spirv/generate.py"
SPEC = importlib.util.spec_from_file_location("kosmickrisp_spirv", GENERATOR)
assert SPEC is not None and SPEC.loader is not None
generator = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = generator
SPEC.loader.exec_module(generator)

# The files the guard reads, copied into each fixture root.
GUARDED = (
    "dependencies/manifest.tsv",
    "dependencies/patches/mesa",
    "tools/rootfs/vulkan/patches/mesa",
    "dependencies/kosmickrisp-spirv/record.tsv",
    "dependencies/kosmickrisp-spirv/toolchain.tsv",
)


def spirv_module(entry_points: list[str]) -> bytes:
    words = [generator.SPIRV_MAGIC, 0x00010400, 0, 1, 0]
    for index, name in enumerate(entry_points):
        raw = name.encode() + b"\0"
        raw += b"\0" * (-len(raw) % 4)
        operands = [6, index + 1, *struct.unpack(f"<{len(raw) // 4}I", raw)]
        words += [(len(operands) + 1) << 16 | generator.OP_ENTRY_POINT, *operands]
    return struct.pack(f"<{len(words)}I", *words)


class Fixture:
    def __init__(self, scratch: Path) -> None:
        self.root = scratch / "root"
        for relative in GUARDED:
            source = REPO_ROOT / relative
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target)
            else:
                shutil.copyfile(source, target)
        self.record = self.root / "dependencies/kosmickrisp-spirv/record.tsv"

    def check(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(GENERATOR), "check", "--root", str(self.root)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdin=subprocess.DEVNULL,
            text=True,
            timeout=60,
            check=False,
        )

    def edit(self, relative: str, old: str, new: str) -> None:
        path = self.root / relative
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if old not in text:
            raise AssertionError(f"{relative}: fixture text not found: {old!r}")
        path.write_text(text.replace(old, new, 1), encoding="utf-8",
                        errors="surrogateescape")

    def record_row(self, kind: str, name: str) -> str:
        for line in self.record.read_text().splitlines():
            fields = line.split("\t")
            if fields[0] == kind and fields[2] == name:
                return line + "\n"
        raise AssertionError(f"record has no {kind} row for {name}")

    def last_writer(self, path: str) -> tuple[str, str]:
        """Return (patch path, index line) of the last topic writing PATH."""
        for directory in reversed(generator.manifest_series(self.root)[1]):
            series = generator.load_patch_series().read_series(directory)
            for patch in reversed(series.patches):
                lines = patch.read_text(encoding="utf-8").split("\n")
                header = f"diff --git a/{path} b/{path}"
                if header in lines:
                    index = lines.index(header)
                    while not lines[index].startswith("index "):
                        index += 1
                    return str(patch.relative_to(self.root)), lines[index]
        raise AssertionError(f"no topic writes {path}")


class KosmicKrispSpirvGuardTest(unittest.TestCase):
    def setUp(self) -> None:
        # A timeout's KeyboardInterrupt skips cleanups, but the object's
        # finalizer still removes the directory when the interpreter exits.
        scratch = tempfile.TemporaryDirectory(prefix="uml-kosmickrisp-spirv-test.")
        self.addCleanup(scratch.cleanup)
        self.fixture = Fixture(Path(scratch.name))

    def assert_rejected(self, fragment: str) -> None:
        result = self.fixture.check()
        self.assertNotEqual(result.returncode, 0, "a stale blob was accepted")
        self.assertIn(fragment, result.stderr)

    def test_committed_series_matches_its_blobs(self) -> None:
        result = self.fixture.check()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_cl_edit_without_regeneration_is_rejected(self) -> None:
        cl = "src/kosmickrisp/libkk/kk_geometry.cl"
        patch, index = self.fixture.last_writer(cl)
        new = index.split()[1].split("..")[1]
        edited = index.replace(f"..{new}", f"..{'1' * 40}")
        self.fixture.edit(patch, index + "\n", edited + "\n")
        self.assert_rejected(f"input {cl} is {'1' * 40}")

    def test_record_input_must_match_the_series(self) -> None:
        row = self.fixture.record_row("input", "src/poly/cl/tessellation.cl")
        blob = row.split("\t")[3].strip()
        self.fixture.edit("dependencies/kosmickrisp-spirv/record.tsv", row,
                          row.replace(blob, "2" * 40))
        self.assert_rejected("src/poly/cl/tessellation.cl")

    def test_kernel_missing_from_the_blob_is_rejected(self) -> None:
        row = self.fixture.record_row("kernel", "libkk_gs_setup_indirect")
        self.fixture.edit("dependencies/kosmickrisp-spirv/record.tsv", row,
                          row + row.replace("libkk_gs_setup_indirect",
                                            "libkk_not_compiled"))
        self.assert_rejected("missing from the blob ['libkk_not_compiled']")

    def test_entry_point_without_a_kernel_is_rejected(self) -> None:
        row = self.fixture.record_row("kernel", "libkk_gs_setup_indirect")
        self.fixture.edit("dependencies/kosmickrisp-spirv/record.tsv", row, "")
        self.assert_rejected("not in the sources ['libkk_gs_setup_indirect']")

    def test_corrupted_literal_is_rejected(self) -> None:
        patch = "dependencies/patches/mesa/kosmickrisp-embed-LLVM-free-build-shaders.patch"
        lines = (self.fixture.root / patch).read_text().split("\n")
        header = lines.index(
            "diff --git a/src/kosmickrisp/libkk/libkk.spv "
            "b/src/kosmickrisp/libkk/libkk.spv")
        start = next(i for i in range(header, len(lines))
                     if lines[i].startswith("literal "))
        line = lines[start + 2]
        replacement = line[:10] + ("0" if line[10] != "0" else "1") + line[11:]
        self.fixture.edit(patch, "\n" + line + "\n", "\n" + replacement + "\n")
        result = self.fixture.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libkk.spv", result.stderr)

    def test_toolchain_change_requires_regeneration(self) -> None:
        self.fixture.edit("dependencies/kosmickrisp-spirv/toolchain.tsv",
                          "\t19.1.6-1\t", "\t19.1.7-1\t")
        self.assert_rejected("toolchain.tsv changed since")

    def test_toolchain_notes_do_not_require_regeneration(self) -> None:
        self.fixture.edit("dependencies/kosmickrisp-spirv/toolchain.tsv",
                          "\tits headers\n", "\tthe translator's headers\n")
        result = self.fixture.check()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_mesa_pin_change_requires_regeneration(self) -> None:
        base, _ = generator.manifest_series(self.fixture.root)
        self.fixture.edit("dependencies/manifest.tsv", f"\t{base}\t", f"\t{'3' * 40}\t")
        self.assert_rejected("the Mesa pin is " + "3" * 40)

    def test_source_digest_is_bound_to_the_inputs(self) -> None:
        row = self.fixture.record_row("source", "inputs")
        digest = row.split("\t")[3].strip()
        self.fixture.edit("dependencies/kosmickrisp-spirv/record.tsv", row,
                          row.replace(digest, "4" * 64))
        self.assert_rejected("source digest does not match")


class KosmicKrispSpirvParserTest(unittest.TestCase):
    def test_entry_points_are_read_from_spirv(self) -> None:
        module = spirv_module(["libkk_one", "libkk_two"])
        self.assertEqual(generator.entry_points(module, "fixture"),
                         ["libkk_one", "libkk_two"])
        with self.assertRaises(generator.SpirvError):
            generator.entry_points(module[:-4], "truncated")

    def test_every_kernel_macro_is_recognised(self) -> None:
        text = ("KERNEL(1)\nlibkk_a(uint x)\n{}\n"
                "KERNEL2D(8, 8)\nlibkk_b(void)\n{}\n"
                "KERNEL3D(4, 4, 4) libkk_c(int y) {}\n"
                "static void helper(void) {}\n")
        self.assertEqual(generator.kernels_in([text]), ["libkk_a", "libkk_b", "libkk_c"])

    def test_binary_literal_round_trips(self) -> None:
        payload = bytes(range(256)) * 3
        packed = zlib.compress(payload)
        lines = [f"literal {len(payload)}"]
        for offset in range(0, len(packed), 52):
            chunk = packed[offset : offset + 52]
            length = chr(ord("A") + len(chunk) - 1) if len(chunk) <= 26 else chr(
                ord("a") + len(chunk) - 27)
            chunk += b"\0" * (-len(chunk) % 4)
            lines.append(length + base64.b85encode(chunk).decode())
        self.assertEqual(generator.decode_literal(lines, "fixture"), payload)

    def test_depfile_rejects_foreign_absolute_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            depfile = Path(scratch) / "fixture.d"
            depfile.write_text(
                "out.spv: src/a.cl /usr/lib/llvm-19/lib/clang/19/include/opencl-c.h "
                "include/b.h\n")
            self.assertEqual(generator.depfile_inputs(depfile, "fixture"),
                             ["include/b.h", "src/a.cl"])
            depfile.write_text("out.spv: src/a.cl /opt/other/c.h\n")
            with self.assertRaises(generator.SpirvError):
                generator.depfile_inputs(depfile, "fixture")


if __name__ == "__main__":
    unittest.main()
