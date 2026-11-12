#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise source generation, independent golden data, and failed publication."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
TOOL = ROOT / "tools/instruction-corpus/source.py"
spec = importlib.util.spec_from_file_location("corpus_source", TOOL)
source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source)


class SourceTests(unittest.TestCase):
    def test_allocation_loops_keep_irregular_golden_values(self):
        rows = [{"word": f"{0x8b000000 | ((i // 32) << 5) | (i % 32):08x}",
                 "name": f"add destination={i % 32} source={i // 32}",
                 "state": {"x0": f"0x{0x1000 + i * 16:016x}"},
                 "observed": [0, 1, 4, 9, 2, 11, 3][i % 7]}
                for i in range(1024)]
        rows[137]["state"]["x1"] = "0xdeadbeef"
        recipe = source.loops.table(rows)
        self.assertLess(len(source.json_line(recipe)), len(source.json_line(rows)) // 4)
        self.assertEqual(list(source.loops.expand(recipe)), rows)
        with self.assertRaises(ValueError):
            list(source.loops.expand({"$table": {"count": 4,
                "row": {"$values": [1, 2]}}}))

    def test_pattern_generators_keep_irregular_bytes(self):
        expected = bytes((11 + i * 17) & 255 for i in range(4096))
        self.assertEqual(source.byte_value(source.byte_source(expected)), expected)
        page = bytearray(b"\x80" * 16384)
        page[123:127] = b"code"
        recipe = source.byte_source(bytes(page))
        self.assertIsInstance(recipe, dict)
        self.assertEqual(source.byte_value(recipe), page)
        patterned = bytearray((17 + i * 29) & 255 for i in range(16384))
        patterned[37:41] = b"code"
        recipe = source.byte_source(bytes(patterned))
        self.assertLess(len(source.json_line(recipe)), 200)
        self.assertEqual(source.byte_value(recipe), patterned)
        with self.assertRaises(ValueError):
            source.byte_value({"repeat": "00", "size": 4,
                               "patches": [[3, "abcd"]]})
        with self.assertRaises(ValueError):
            source.source_file(pathlib.Path("source"), "../pack")

    def test_regular_state_with_an_irregular_operand(self):
        state = {f"x{i}": f"0x{0x1000 + i * 16:016x}" for i in range(31)}
        state.update({f"v{i}": bytes((7 + i * 23 + b) & 255
                                     for b in range(16)).hex() for i in range(32)})
        state["x3"] = "0xffff000012341234"
        state["v17"] = "ff" * 16
        self.assertEqual(source.expand_bytes(source.state_source(state)), state)
        delta = source.difference({"retired": None}, {"event": "system-call"})
        self.assertEqual(source.overlay({"event": "system-call"}, delta), {"retired": None})

    def test_fex_store_loop_preserves_displacements_and_operands(self):
        # FEX's XeSS stress case expands this memory-store loop. Address
        # bytes carry across boundaries; the immediate wraps every 64 stores.
        code = bytes.fromhex("48bc0000000002000000") + b"".join(
            b"\xc6\x84\x24" + struct.pack("<I", 0x10000 + i) + bytes([(1 + 4 * i) & 255])
            for i in range(2048)) + b"\xf4\xc3"
        recipe = source.program_source(code)
        self.assertLess(len(source.json_line(recipe)), len(code))
        self.assertEqual(source.byte_value(recipe), code)
        rows = [{"code": recipe} for _ in range(7)] + [{"code": "f4"}]
        table = source.loops.table(rows)
        self.assertIsNotNone(table)
        self.assertEqual(list(source.loops.expand(table)), rows)

    def test_hardware_expectation_is_a_source_input(self):
        # A tiny independent MXF1 fixture: two cases of XORPS with distinct
        # observed flags. Generation cannot replace one by the other's flags.
        instruction = b"xorps"
        encoding = bytes.fromhex("0f57c0")
        original = (b"MXF1" + struct.pack("<I", 1) + bytes([len(instruction)]) +
                    instruction + bytes([len(encoding)]) + encoding + b"\x10" +
                    struct.pack("<I", 2) +
                    struct.pack("<HHB", 0, 0x1f80, 0) + bytes(48) +
                    struct.pack("<HHB", 0, 0x1fc0, 2) + b"\x01" * 48)
        with tempfile.TemporaryDirectory(prefix="corpus-source-") as directory:
            scratch = pathlib.Path(directory)
            pack = scratch / "fixture.bin"
            pack.write_bytes(original)
            data = scratch / "data"
            source_root = data / "source"
            source.import_masked(pack, source_root)
            index = json.loads((source_root / "index.json").read_text())
            self.assertEqual(source.generate_masked(source_root, index), original)
            (data / "provenance.json").write_text(json.dumps({
                "cases": 2, "subset_sha256": hashlib.sha256(original).hexdigest()}))
            output = scratch / "output"
            command = [sys.executable, "-B", str(TOOL), "--source", str(source_root),
                       "--output", str(output)]
            subprocess.run(command, check=True, capture_output=True, timeout=30)
            result = output / "xeon-flags.bin"
            self.assertEqual(result.read_bytes(), original)
            table = source_root / index["files"][0]
            rows = [json.loads(line) for line in table.read_text().splitlines()]
            rows[2]["flags"] = "0x0"
            table.write_text("".join(source.json_line(row) + "\n" for row in rows))
            failed = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("differs from its accepted provenance", failed.stderr)
            self.assertEqual(result.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
