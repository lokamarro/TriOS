#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The manually embedded device framework supplies its dSYM to an archive."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
EMBED = ROOT / "Kernarium/scripts/embed-device-framework.sh"


class DeviceFrameworkSymbolsTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="framework symbols ")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.framework = self.root / "Driver.xcframework"
        self.slice = self.framework / "ios-arm64"
        bundle = self.slice / "Driver.framework"
        bundle.mkdir(parents=True)
        (bundle / "Driver").write_bytes(b"framework fixture")
        self.symbols = self.slice / "dSYMs/Driver.framework.dSYM"
        dwarf = self.symbols / "Contents/Resources/DWARF/Driver"
        dwarf.parent.mkdir(parents=True)
        dwarf.write_bytes(b"debug symbol fixture")
        self.destination = self.root / "archive symbols"
        self.environment = {
            **os.environ, "ACTION": "install", "PLATFORM_NAME": "iphoneos",
            "CODE_SIGNING_ALLOWED": "NO", "TARGET_BUILD_DIR": str(self.root / "build"),
            "TARGET_TEMP_DIR": str(self.root / "intermediates"),
            "FRAMEWORKS_FOLDER_PATH": "Fixture.app/Frameworks",
            "DWARF_DSYM_FOLDER_PATH": str(self.destination),
        }

    def embed(self, **overrides):
        return subprocess.run(
            [str(EMBED), str(self.framework)],
            env={**self.environment, **overrides}, stdin=subprocess.DEVNULL,
            capture_output=True, text=True, timeout=30, check=False,
        )

    def test_archive_copies_symbols_outside_the_application(self):
        result = self.embed()
        self.assertEqual(result.returncode, 0, result.stderr)
        dwarf = self.destination / "Driver.framework.dSYM/Contents/Resources/DWARF/Driver"
        self.assertEqual(dwarf.read_bytes(), b"debug symbol fixture")
        app = self.root / "build/Fixture.app"
        self.assertEqual((app / "Frameworks/Driver.framework/Driver").read_bytes(), b"framework fixture")
        self.assertEqual(list(app.rglob("*.dSYM")), [])

    def test_archive_refuses_a_framework_without_symbols(self):
        self.symbols.rename(self.slice / "removed-symbols")
        result = self.embed()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing debug symbols", result.stderr)

    def test_a_simulator_build_does_not_embed_the_device_framework(self):
        result = self.embed(PLATFORM_NAME="iphonesimulator")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.destination.exists())
        self.assertFalse((self.root / "build").exists())


if __name__ == "__main__":
    unittest.main()
