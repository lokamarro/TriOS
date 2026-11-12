#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/check-run-location.py refuses a Mac build in a temporary directory.

The script runs in an Xcode run-script phase, where the destination is
TARGET_DEVICE_PLATFORM_NAME and the products directory TARGET_BUILD_DIR. The
fixtures give it those two variables and nothing else. The second group takes
each application's Xcode project, finds the first build phase of the
application's target and runs that phase's own script, so a project that drops
the phase, moves it behind compilation or names another path fails here.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
PHASE = "Check Mac run location"

SCRIPT: pathlib.Path
BOUND_SECONDS = 30
# A products directory below no temporary directory; it need not exist.
ELSEWHERE = "/opt/xcode-products/Build/Products/Debug-iphoneos"


def run(products: str, destination: str | None = "macosx") -> subprocess.CompletedProcess[str]:
    environment = {"TARGET_BUILD_DIR": products}
    if destination is not None:
        environment["TARGET_DEVICE_PLATFORM_NAME"] = destination
    return subprocess.run(
        [sys.executable, "-B", str(SCRIPT)],
        env=environment, capture_output=True, text=True, timeout=BOUND_SECONDS, check=False, stdin=subprocess.DEVNULL,
    )


def without_trailing_commas(text: str) -> str:
    """The text of a project.xcproj as JSON: a comma before a closing bracket, outside a string, is dropped."""
    out: list[str] = []
    in_string = escaped = False
    for character in text:
        if in_string:
            out.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character in "}]":
            while out and out[-1] in " \t\r\n":
                out.pop()
            if out and out[-1] == ",":
                out.pop()
            out.append(character)
            continue
        out.append(character)
    return "".join(out)


def first_phase_of_andarium() -> tuple[str, str, str]:
    """The name, shell and script of the Andarium target's first build phase."""
    project = json.loads(without_trailing_commas(
        (REPOSITORY / "Andarium/Andarium.xcodeproj/project.xcproj").read_text(encoding="utf-8")))
    target = next(target for target in project["targets"] if target["name"] == "Andarium")
    phase = target["build-phases"][0]
    script = phase["script"]
    return phase.get("name", ""), phase.get("shell", ""), "\n".join(script) if isinstance(script, list) else script


def first_phase_of_kernarium() -> tuple[str, str, str]:
    """The name, shell and script of the Kernarium target's first build phase."""
    converted = subprocess.run(
        ["plutil", "-convert", "json", "-o", "-", str(REPOSITORY / "Kernarium/Kernarium.xcodeproj/project.pbxproj")],
        capture_output=True, text=True, timeout=BOUND_SECONDS, check=True, stdin=subprocess.DEVNULL)
    objects = json.loads(converted.stdout)["objects"]
    target = next(o for o in objects.values() if o.get("isa") == "PBXNativeTarget" and o.get("name") == "Kernarium")
    phase = objects[target["buildPhases"][0]]
    return phase.get("name", ""), phase.get("shellPath", ""), phase.get("shellScript", "")


class RunLocationTests(unittest.TestCase):
    def test_a_mac_build_below_every_temporary_directory_is_refused(self):
        for root in ("/tmp", "/private/tmp", "/var/tmp", "/private/var/tmp", "/var/folders", "/private/var/folders"):
            with self.subTest(root=root):
                result = run(f"{root}/derived/Build/Products/Debug-iphoneos")
                self.assertEqual(result.returncode, 1)
                self.assertIn("error:", result.stderr)
                self.assertIn(root, result.stderr)

    def test_the_users_temporary_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as scratch:
            result = run(scratch)
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_a_path_that_leads_into_a_temporary_directory_is_refused(self):
        self.assertEqual(run("/opt/xcode-products/../../tmp/derived").returncode, 1)

    def test_a_mac_build_elsewhere_is_accepted(self):
        result = run(ELSEWHERE)
        self.assertEqual((result.returncode, result.stderr), (0, ""))

    def test_a_directory_that_only_starts_like_a_temporary_one_is_accepted(self):
        self.assertEqual(run("/tmpfiles/derived").returncode, 0)

    def test_other_destinations_may_use_a_temporary_directory(self):
        for destination in (None, "iphoneos", "iphonesimulator"):
            with self.subTest(destination=destination):
                self.assertEqual(run("/tmp/derived", destination).returncode, 0)

    def test_a_mac_build_without_a_products_directory_is_refused(self):
        self.assertEqual(run("").returncode, 1)


@unittest.skipUnless(shutil.which("plutil"), "plutil reads the Kernarium project")
class ProjectPhaseTests(unittest.TestCase):
    """Each application's first build phase, run as Xcode would, with the shared script."""

    PROJECTS = (
        ("Andarium", "Andarium", first_phase_of_andarium),
        ("Kernarium", "Kernarium", first_phase_of_kernarium),
    )

    def run_phase(self, directory: str, phase: tuple[str, str, str], products: str) -> subprocess.CompletedProcess[str]:
        name, shell, script = phase
        self.assertEqual(name, PHASE)
        environment = {
            "SRCROOT": str(REPOSITORY / directory),
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "TARGET_DEVICE_PLATFORM_NAME": "macosx",
            "TARGET_BUILD_DIR": products,
        }
        return subprocess.run(
            [shell, "-c", script], env=environment, capture_output=True, text=True,
            timeout=BOUND_SECONDS, check=False, stdin=subprocess.DEVNULL)

    def test_the_first_phase_of_each_application_refuses_a_mac_build_in_a_temporary_directory(self):
        for application, directory, first_phase in self.PROJECTS:
            with self.subTest(application=application):
                result = self.run_phase(directory, first_phase(), "/tmp/derived/Build/Products/Debug-iphoneos")
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("error:", result.stderr)

    def test_the_first_phase_of_each_application_accepts_a_mac_build_elsewhere(self):
        for application, directory, first_phase in self.PROJECTS:
            with self.subTest(application=application):
                result = self.run_phase(directory, first_phase(), ELSEWHERE)
                self.assertEqual((result.returncode, result.stderr), (0, ""))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", type=pathlib.Path, required=True)
    options, remaining = parser.parse_known_args()
    SCRIPT = options.checker
    unittest.main(argv=[sys.argv[0], *remaining])
