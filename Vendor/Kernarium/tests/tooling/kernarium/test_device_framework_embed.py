#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The manually embedded device framework is touched only when it changes.

Xcode skips signing the application when nothing else changed, so a build that
copies and signs the framework again leaves the application's seal invalid.
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
EMBED = ROOT / "Kernarium/scripts/embed-device-framework.sh"
INFO_PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>test.%(name)s</string>
<key>CFBundleExecutable</key><string>%(name)s</string>
<key>CFBundlePackageType</key><string>%(type)s</string>
</dict></plist>
"""


def run(command, **kwargs):
    if "input" not in kwargs:
        kwargs["stdin"] = subprocess.DEVNULL
    return subprocess.run(command, capture_output=True, text=True, timeout=60,
                          check=False, **kwargs)


def compile_binary(output: Path, source: str, *flags: str):
    result = run(["/usr/bin/xcrun", "clang", "-x", "c", "-", "-o", str(output), *flags],
                 input=source)
    if result.returncode != 0:
        raise RuntimeError(result.stderr)


def snapshot(tree: Path):
    """The identity of every file below tree: a rewritten file differs."""
    state = {}
    for path in sorted(tree.rglob("*")):
        status = path.lstat()
        state[str(path.relative_to(tree))] = (
            status.st_ino, status.st_mtime_ns, status.st_ctime_ns,
            hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else "",
        )
    return state


class DeviceFrameworkEmbedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory(prefix="framework embed ")
        cls.addClassCleanup(cls.scratch.cleanup)
        cls.library = Path(cls.scratch.name) / "libraries"
        cls.library.mkdir()
        compile_binary(cls.library / "Driver", "int driver(void){return 7;}",
                       "-dynamiclib", "-install_name", "@rpath/Driver.framework/Driver")
        compile_binary(cls.library / "Driver.changed", "int driver(void){return 8;}",
                       "-dynamiclib", "-install_name", "@rpath/Driver.framework/Driver")
        compile_binary(cls.library / "Fixture", "int main(void){return 0;}")

    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="framework embed ")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        self.framework = self.root / "Driver.xcframework"
        self.source = self.framework / "ios-arm64/Driver.framework"
        self.source.mkdir(parents=True)
        self.install_driver("Driver")
        (self.source / "Info.plist").write_text(
            INFO_PLIST % {"name": "Driver", "type": "FMWK"})
        self.app = self.root / "build/Fixture.app"
        self.app.mkdir(parents=True)
        (self.app / "Fixture").write_bytes((self.library / "Fixture").read_bytes())
        (self.app / "Fixture").chmod(0o755)
        (self.app / "Info.plist").write_text(
            INFO_PLIST % {"name": "Fixture", "type": "APPL"})
        self.embedded = self.app / "Frameworks/Driver.framework"
        self.environment = {
            **os.environ, "ACTION": "build", "PLATFORM_NAME": "iphoneos",
            "CODE_SIGNING_ALLOWED": "YES", "EXPANDED_CODE_SIGN_IDENTITY": "-",
            "TARGET_BUILD_DIR": str(self.root / "build"),
            "TARGET_TEMP_DIR": str(self.root / "intermediates"),
            "FRAMEWORKS_FOLDER_PATH": "Fixture.app/Frameworks",
        }

    def install_driver(self, library):
        (self.source / "Driver").write_bytes((self.library / library).read_bytes())

    def embed(self, **overrides):
        result = run([str(EMBED), str(self.framework)],
                     env={**self.environment, **overrides})
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def sign_application(self):
        """What Xcode does when it signs the application."""
        result = run(["/usr/bin/codesign", "--force", "--sign", "-", str(self.app)])
        self.assertEqual(result.returncode, 0, result.stderr)

    def assert_valid(self, bundle):
        result = run(["/usr/bin/codesign", "--verify", "--strict", str(bundle)])
        self.assertEqual(result.returncode, 0, f"{bundle.name}: {result.stderr}")

    def test_an_unchanged_build_leaves_the_framework_and_the_seal_alone(self):
        self.assertIn("embedded Driver", self.embed())
        self.assert_valid(self.embedded)
        self.sign_application()
        self.assert_valid(self.app)
        before = snapshot(self.embedded)
        output = self.embed()
        self.assertEqual(snapshot(self.embedded), before)
        self.assert_valid(self.app)
        self.assertIn("already embedded", output)

    def test_a_changed_framework_is_embedded_and_signed_again(self):
        self.embed()
        before = snapshot(self.embedded)
        self.install_driver("Driver.changed")
        self.assertIn("embedded Driver", self.embed())
        self.assertNotEqual(snapshot(self.embedded)["Driver"], before["Driver"])
        self.assert_valid(self.embedded)
        unchanged = snapshot(self.embedded)
        self.assertIn("already embedded", self.embed())
        self.assertEqual(snapshot(self.embedded), unchanged)

    def test_changed_signing_signs_the_framework_again(self):
        self.embed()
        before = snapshot(self.embedded)
        self.assertIn("embedded Driver", self.embed(OTHER_CODE_SIGN_FLAGS="--identifier=test.signed"))
        self.assertNotEqual(snapshot(self.embedded), before)
        self.assert_valid(self.embedded)
        signed = snapshot(self.embedded)
        self.assertIn("already embedded", self.embed(OTHER_CODE_SIGN_FLAGS="--identifier=test.signed"))
        self.assertEqual(snapshot(self.embedded), signed)

    def test_a_modified_embedded_framework_is_replaced(self):
        self.embed()
        expected = snapshot(self.embedded)["Driver"][3]
        with (self.embedded / "Driver").open("ab") as binary:
            binary.write(b"modified")
        self.assertIn("embedded Driver", self.embed())
        self.assertEqual(snapshot(self.embedded)["Driver"][3], expected)
        self.assert_valid(self.embedded)

    def test_a_removed_embedded_framework_is_embedded_again(self):
        self.embed()
        for path in sorted(self.embedded.rglob("*"), reverse=True):
            path.rmdir() if path.is_dir() else path.unlink()
        self.embedded.rmdir()
        self.assertIn("embedded Driver", self.embed())
        self.assert_valid(self.embedded)

    def test_an_unsigned_build_embeds_the_framework_once(self):
        unsigned = {"CODE_SIGNING_ALLOWED": "NO", "EXPANDED_CODE_SIGN_IDENTITY": ""}
        self.assertIn("embedded Driver", self.embed(**unsigned))
        before = snapshot(self.embedded)
        self.assertIn("already embedded", self.embed(**unsigned))
        self.assertEqual(snapshot(self.embedded), before)
        self.assertFalse((self.embedded / "_CodeSignature").exists())


if __name__ == "__main__":
    unittest.main()
