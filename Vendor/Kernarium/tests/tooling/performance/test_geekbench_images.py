#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the Geekbench measurement images from a stand-in release and disk."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import hashlib
import importlib.util
import io
import os
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

TESTS = pathlib.Path(__file__).resolve().parents[2]
TOOL = TESTS / "perf" / "make_geekbench_images.py"
FIXTURES = TESTS / "tooling" / "rootfs" / "test_starter_images.py"
STARTER = TESTS.parent / "tools" / "rootfs" / "starter"
RELEASE_DIRECTORY = "/opt/geekbench/release"
TOOLS = ("mkfs.erofs", "fsck.erofs", "mke2fs", "e2fsck", "resize2fs", "debugfs")


def load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


IMAGES = load("make_geekbench_images", TOOL)
sys.path.insert(0, str(STARTER))
import gpt  # noqa: E402

DISK_FIXTURE = load("starter_image_fixtures", FIXTURES)


def have_tools() -> bool:
    try:
        for name in TOOLS:
            IMAGES.find_tool(name)
    except IMAGES.ImageToolError:
        return False
    return True


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@unittest.skipUnless(have_tools(), "needs erofs-utils and e2fsprogs")
class GeekbenchImageTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="uml-geekbench-img-")
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name).resolve()
        self.release = self.root / "release"
        self.release.mkdir()
        # Not compressible and not a multiple of any block size.
        self.program = os.urandom(300_001)
        self.data = os.urandom(200_003)
        (self.release / "geekbench_armv7").write_bytes(self.program)
        (self.release / "geekbench.plar").write_bytes(self.data)
        self.digest = sha256(self.program)

    def starter_disk(self, with_opt: bool = True) -> tuple[pathlib.Path, bytes]:
        """A GPT disk whose sole partition holds a small real ext4."""
        tree = self.root / "tree"
        tree.mkdir(exist_ok=True)
        (tree / "etc").mkdir(exist_ok=True)
        (tree / "etc/os-release").write_text("ID=debian\n")
        if with_opt:
            (tree / "opt").mkdir(exist_ok=True)
        filesystem = self.root / "base.ext4"
        filesystem.unlink(missing_ok=True)
        IMAGES.run([IMAGES.find_tool("mke2fs"), "-q", "-t", "ext4", "-b", "4096",
                    "-d", str(tree), str(filesystem), "8M"])
        contents = filesystem.read_bytes()
        sectors = len(contents) // gpt.SECTOR_SIZE
        disk = self.root / "starter.img"
        lbas = DISK_FIXTURE.PARTITION_FIRST_LBA + sectors + 34
        with (
            mock.patch.object(DISK_FIXTURE, "DISK_LBAS", lbas),
            mock.patch.object(DISK_FIXTURE, "LAST_USABLE_LBA", lbas - 34),
        ):
            DISK_FIXTURE.write_gpt_ext4_fixture(disk, gpt)
        with disk.open("r+b") as image:
            image.seek(DISK_FIXTURE.PARTITION_FIRST_LBA * gpt.SECTOR_SIZE)
            image.write(contents)
        self.assertEqual(gpt.inspect(disk)["filesystem"]["bytes"], len(contents))
        return disk, contents

    def run_tool(self, *arguments: str) -> tuple[int, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            mock.patch.object(sys, "argv", [str(TOOL), *arguments]),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            status = IMAGES.main()
        return status, stdout.getvalue() + stderr.getvalue()

    def dump(self, image: pathlib.Path, path: str) -> bytes:
        target = self.root / "dump.bin"
        target.unlink(missing_ok=True)
        IMAGES.run([IMAGES.find_tool("debugfs"), "-R", f"dump {path} {target}",
                    str(image)])
        return target.read_bytes()

    def stat(self, image: pathlib.Path, path: str) -> str:
        return IMAGES.run([IMAGES.find_tool("debugfs"), "-R", f"stat {path}",
                           str(image)]).stdout

    def test_the_payload_image_holds_the_release_at_its_root(self) -> None:
        output = self.root / "payload.erofs"
        status, text = self.run_tool(
            "payload", "--release-dir", str(self.release),
            "--program-sha256", self.digest, "--output", str(output))
        self.assertEqual(status, 0, text)
        self.assertIn(sha256(output.read_bytes()), text)
        extracted = self.root / "extracted"
        IMAGES.run([IMAGES.find_tool("fsck.erofs"), f"--extract={extracted}",
                    str(output)])
        self.assertEqual(sorted(path.name for path in extracted.iterdir()),
                         ["geekbench.plar", "geekbench_armv7"])
        self.assertEqual((extracted / "geekbench_armv7").read_bytes(),
                         self.program)
        self.assertEqual((extracted / "geekbench.plar").read_bytes(), self.data)
        # Its 4 KiB blocks mount on a 16 KiB-page guest as well as a 4 KiB
        # one; the superblock's blkszbits is 12.
        self.assertEqual(output.read_bytes()[1024 + 12], 12)

    def test_the_aarch64_program_packages_the_same_way(self) -> None:
        program = os.urandom(100_003)
        (self.release / "geekbench_aarch64").write_bytes(program)
        output = self.root / "payload64.erofs"
        try:
            status, text = self.run_tool(
                "--program", "geekbench_aarch64", "payload",
                "--release-dir", str(self.release), "--program-sha256",
                sha256(program), "--output", str(output))
        finally:
            IMAGES.PROGRAM = "geekbench_armv7"
        self.assertEqual(status, 0, text)
        extracted = self.root / "extracted64"
        IMAGES.run([IMAGES.find_tool("fsck.erofs"), f"--extract={extracted}",
                    str(output)])
        self.assertEqual(sorted(path.name for path in extracted.iterdir()),
                         ["geekbench.plar", "geekbench_aarch64"])
        self.assertEqual((extracted / "geekbench_aarch64").read_bytes(), program)

    def test_a_wrong_pin_or_an_incomplete_release_is_refused(self) -> None:
        output = self.root / "payload.erofs"
        for pin, expected in (
            (sha256(b"other"), "does not match --program-sha256"),
            (self.digest.upper(), "lowercase hex"),
        ):
            status, text = self.run_tool(
                "payload", "--release-dir", str(self.release),
                "--program-sha256", pin, "--output", str(output))
            self.assertEqual(status, 1)
            self.assertIn(expected, text)
        (self.release / "geekbench.plar").unlink()
        status, text = self.run_tool(
            "payload", "--release-dir", str(self.release),
            "--program-sha256", self.digest, "--output", str(output))
        self.assertEqual(status, 1)
        self.assertIn("has no regular file geekbench.plar", text)
        self.assertFalse(output.exists())

    def test_an_existing_output_a_bad_name_or_the_repository_is_refused(self) -> None:
        existing = self.root / "payload.erofs"
        existing.write_bytes(b"keep")
        inside = IMAGES.REPOSITORY / "tests" / "payload.erofs"
        for output, expected in (
            (existing, "refusing to replace"),
            (self.root / "payload.img", "must be named *.erofs"),
            (inside, "must stay out of the repository"),
        ):
            status, text = self.run_tool(
                "payload", "--release-dir", str(self.release),
                "--program-sha256", self.digest, "--output", str(output))
            self.assertEqual(status, 1, text)
            self.assertIn(expected, text)
        self.assertEqual(existing.read_bytes(), b"keep")
        self.assertFalse(inside.exists())

    def test_a_root_image_is_the_starter_disks_filesystem(self) -> None:
        disk, contents = self.starter_disk()
        output = self.root / "rootfs.ext4"
        status, text = self.run_tool(
            "rootfs", "--starter-image", str(disk), "--output", str(output))
        self.assertEqual(status, 0, text)
        self.assertEqual(output.read_bytes(), contents)

    def test_a_measurement_image_carries_the_release(self) -> None:
        for with_opt in (True, False):
            with self.subTest(with_opt=with_opt):
                disk, _ = self.starter_disk(with_opt)
                output = self.root / f"measurement-{with_opt}.ext4"
                status, text = self.run_tool(
                    "rootfs", "--starter-image", str(disk),
                    "--release-dir", str(self.release),
                    "--program-sha256", self.digest, "--output", str(output))
                self.assertEqual(status, 0, text)
                self.assertEqual(
                    self.dump(output, f"{RELEASE_DIRECTORY}/geekbench_armv7"),
                    self.program)
                self.assertEqual(
                    self.dump(output, f"{RELEASE_DIRECTORY}/geekbench.plar"),
                    self.data)
                self.assertEqual(self.dump(output, "/etc/os-release"),
                                 b"ID=debian\n")
                program = self.stat(output,
                                    f"{RELEASE_DIRECTORY}/geekbench_armv7")
                self.assertIn("Mode:  0755", program)
                self.assertIn("User:     0   Group:     0", program)
                self.assertIn("Mode:  0644", self.stat(
                    output, f"{RELEASE_DIRECTORY}/geekbench.plar"))

    def test_a_release_needs_its_pin_and_a_pin_needs_its_release(self) -> None:
        disk, _ = self.starter_disk()
        output = self.root / "rootfs.ext4"
        for options, expected in (
            (["--release-dir", str(self.release)], "needs --program-sha256"),
            (["--program-sha256", self.digest], "needs --release-dir"),
        ):
            status, text = self.run_tool(
                "rootfs", "--starter-image", str(disk), *options,
                "--output", str(output))
            self.assertEqual(status, 1)
            self.assertIn(expected, text)
        self.assertFalse(output.exists())
        status, text = self.run_tool(
            "rootfs", "--starter-image", str(self.release / "geekbench.plar"),
            "--output", str(output))
        self.assertEqual(status, 1)
        self.assertIn("is not a starter image", text)


if __name__ == "__main__":
    unittest.main()
