#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium/scripts/stage-system-image.py stages only a checked, accepted release.

A fake release carries the files and manifest fields the script reads: the
system disk (any bytes), the partition layout that declares the disk's size
and sha256, the kernel command line and the manifest inventory.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "Andarium/scripts/stage-system-image.py"
PACKAGER = REPOSITORY / "Kernarium/scripts/package-ios-frameworks.sh"

RELEASE_ID = "20261031-los24-r4"
FAMILY = "lineage-24-mainline-6.18-4k"
DIRECTORY = f"lineage24-mainline6.18-4k-{RELEASE_ID}"
SYSTEM_DISK = b"GPT-and-partitions" * 1000


def make_release(root: pathlib.Path, **overrides) -> pathlib.Path:
    release = root / DIRECTORY
    (release / "guest").mkdir(parents=True)
    (release / "boot").mkdir()
    disk = overrides.get("disk", SYSTEM_DISK)
    (release / "guest/system.img").write_bytes(disk)
    (release / "boot/kernel-command-line.txt").write_text("init=/init console=ttyS0\n")
    layout = json.dumps({
        "schema_version": overrides.get("layout_schema", 2),
        "sector_size": 512,
        "system_disk": {
            "size_bytes": overrides.get("layout_bytes", len(disk)),
            "sha256": overrides.get("layout_sha256", hashlib.sha256(disk).hexdigest()),
        },
        "data_disk": {"filesystem": "ext4"},
    })
    (release / "guest/partition-layout.json").write_text(layout)
    inventory = [
        {"path": "boot/kernel-command-line.txt", "role": "kernel-command-line",
         "size": (release / "boot/kernel-command-line.txt").stat().st_size},
        {"path": "guest/partition-layout.json", "role": "partition-layout", "size": len(layout)},
        {"path": "guest/system.img", "role": overrides.get("disk_role", "system-disk"),
         "size": overrides.get("disk_size", len(disk))},
        {"path": "kernel/vmlinux", "role": "kernel-elf", "size": 1},
    ]
    manifest = {
        "acceptance": overrides.get("acceptance", "accepted"),
        "android": {"major": 17},
        "kernel": {"page_size": overrides.get("page_size", 4096), "release": "6.18.54-gabc"},
        "product_family": overrides.get("family", FAMILY),
        "release_id": overrides.get("release_id", RELEASE_ID),
        "schema_version": 6,
        "inventory": inventory,
    }
    (release / "release-manifest.json").write_text(json.dumps(manifest))
    return release


def make_acceptance(path: pathlib.Path, **overrides) -> pathlib.Path:
    values = {
        "LINEAGE24_ACCEPTED_RELEASE_ID": RELEASE_ID,
        "LINEAGE24_ACCEPTED_DIRECTORY_NAME": DIRECTORY,
        "LINEAGE24_ACCEPTED_PRODUCT_FAMILY": FAMILY,
        "LINEAGE24_ACCEPTED_ANDROID_MAJOR": "17",
    }
    values.update(overrides)
    lines = [f"{key}='{value}'" if not value.isdigit() else f"{key}={value}" for key, value in values.items()]
    path.write_text("\n".join(lines) + "\n")
    return path


def run(*arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(SCRIPT), *arguments],
        capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL,
    )


def tree(directory: pathlib.Path) -> list[str]:
    return sorted(str(path.relative_to(directory)) for path in directory.rglob("*") if path.is_file())


class StageSystemImageTests(unittest.TestCase):
    def setUp(self):
        self._scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self._scratch.cleanup)
        self.root = pathlib.Path(self._scratch.name)
        self.output = self.root / "out/andarium-system"

    def test_stages_the_four_files_of_an_explicit_release_at_their_release_paths(self):
        release = make_release(self.root / "releases")
        result = run("--release", str(release), "--output", str(self.output))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            tree(self.output),
            ["boot/kernel-command-line.txt", "guest/partition-layout.json",
             "guest/system.img", "release-manifest.json"],
        )
        for relative in tree(self.output):
            self.assertEqual((self.output / relative).read_bytes(), (release / relative).read_bytes(), relative)
        self.assertIn(RELEASE_ID, result.stdout)

    def test_the_accepted_release_is_found_through_the_acceptance_file(self):
        make_release(self.root / "releases")
        acceptance = make_acceptance(self.root / "accepted-release.env")
        result = run("--acceptance", str(acceptance), "--releases", str(self.root / "releases"),
                     "--output", str(self.output))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.output / "release-manifest.json").is_file())

    def test_a_release_that_is_not_the_accepted_one_is_refused(self):
        make_release(self.root / "releases", release_id="20260101-other")
        acceptance = make_acceptance(self.root / "accepted-release.env")
        result = run("--acceptance", str(acceptance), "--releases", str(self.root / "releases"),
                     "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release_id", result.stderr)
        self.assertFalse(self.output.exists())

    def test_a_missing_release_is_named(self):
        acceptance = make_acceptance(self.root / "accepted-release.env")
        result = run("--acceptance", str(acceptance), "--releases", str(self.root / "none"),
                     "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not installed", result.stderr)

    def test_each_defect_of_a_release_is_refused_and_stages_nothing(self):
        cases = {
            "acceptance": ({"acceptance": "candidate"}, "not accepted"),
            "page size": ({"page_size": 16384}, "4096"),
            "family": ({"family": "aosp-17"}, "LineageOS"),
            "inventory size": ({"disk_size": 7}, "size in the manifest"),
            "no system disk role": ({"disk_role": "guest-disk-archive"}, "system-disk"),
            "layout size": ({"layout_bytes": len(SYSTEM_DISK) + 1}, "size in the partition layout"),
            "layout sha256": ({"layout_sha256": "0" * 64}, "sha256 in the partition layout"),
            "layout schema": ({"layout_schema": 1}, "schema 2"),
        }
        for name, (overrides, message) in cases.items():
            with self.subTest(name):
                release = make_release(self.root / name, **overrides)
                output = self.root / "out" / name
                result = run("--release", str(release), "--output", str(output))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertFalse(output.exists())

    def test_a_missing_file_is_refused(self):
        release = make_release(self.root / "releases")
        (release / "boot/kernel-command-line.txt").unlink()
        result = run("--release", str(release), "--output", str(self.output))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing or unsafe", result.stderr)

    def test_staging_again_replaces_the_output_and_a_failure_keeps_it(self):
        first = make_release(self.root / "first")
        self.assertEqual(run("--release", str(first), "--output", str(self.output)).returncode, 0)
        before = (self.output / "release-manifest.json").read_text()

        broken = make_release(self.root / "broken", page_size=16384)
        self.assertNotEqual(run("--release", str(broken), "--output", str(self.output)).returncode, 0)
        self.assertEqual((self.output / "release-manifest.json").read_text(), before)

        second = make_release(self.root / "second", release_id="20261101-los24-r5", disk=b"newer disk")
        self.assertEqual(run("--release", str(second), "--output", str(self.output)).returncode, 0)
        self.assertIn("20261101-los24-r5", (self.output / "release-manifest.json").read_text())
        self.assertEqual((self.output / "guest/system.img").read_bytes(), b"newer disk")
        # No scratch directory is left beside the output.
        self.assertEqual([entry.name for entry in self.output.parent.iterdir()], ["andarium-system"])


class PackageOptionTests(unittest.TestCase):
    """The packaging script checks its options before it touches any tool."""

    def package(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["/bin/sh", str(PACKAGER), *arguments],
            capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL,
        )

    def test_an_unknown_runtime_set_is_refused(self):
        result = self.package("--runtimes", "linux")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--runtimes takes all or android", result.stderr)

    def test_runtimes_needs_a_value(self):
        result = self.package("--runtimes")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("usage", result.stderr)

    def test_the_linux_automation_runtime_is_not_an_android_only_package(self):
        result = self.package("--runtimes", "android", "--automation")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--automation", result.stderr)

    def test_an_unknown_option_shows_the_usage_with_the_new_option(self):
        result = self.package("--bogus")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--runtimes", result.stderr)


if __name__ == "__main__":
    unittest.main()
