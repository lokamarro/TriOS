#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The audio interrupter's script, project and sources agree.

The interrupter is test support that plays digital silence on a non-mixable
session so a run can interrupt Kernarium. It is built and driven by
Kernarium/scripts/audio-interrupter.py, which takes the signing team from the
environment or the local signing xcconfig, derives the bundle identifier from
the Kernarium project and hands the settings to the application through
DEVICECTL_CHILD_ environment variables. These tests keep
the three from drifting apart without a device: the script's settings are the
ones the Swift source reads, the project signs nothing by itself, the audio
background mode is declared, and the sources compile for the iOS SDK.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "Kernarium/scripts/audio-interrupter.py"
PROJECT = ROOT / "Kernarium/AudioInterrupter"
SOURCES = sorted((PROJECT / "AudioInterrupter").glob("*.swift"))
TEAM = "ABCDE12345"
SPEC = importlib.util.spec_from_file_location("audio_interrupter", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
interrupter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(interrupter)


def launch_arguments(*extra: str) -> argparse.Namespace:
    return interrupter.parser().parse_args(["--device", "UDID", "launch", *extra])


def swift_text() -> str:
    return "\n".join(path.read_text() for path in SOURCES)


class ScriptTests(unittest.TestCase):
    def test_identifier_comes_from_the_kernarium_project(self) -> None:
        self.assertEqual(interrupter.kernarium_identifier(), "com.imxieyi.Kernarium")

    def test_team_comes_from_the_environment_or_the_local_xcconfig(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "Signing.local.xcconfig"
            local.write_text("// the signer\nDEVELOPMENT_TEAM = " + TEAM + " // comment\n")
            self.assertEqual(interrupter.signing_team({}, local), TEAM)
            self.assertEqual(
                interrupter.signing_team({interrupter.TEAM_VARIABLE: "ZYXWV98765"}, local), "ZYXWV98765")

    def test_a_missing_team_says_where_to_set_it(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            local = Path(directory) / "Signing.local.xcconfig"
            for content in (None, "DEVELOPMENT_TEAM =\n", "DEVELOPMENT_TEAM = not-a-team\n"):
                if content is not None:
                    local.write_text(content)
                with self.assertRaises(SystemExit) as raised:
                    interrupter.signing_team({}, local)
                self.assertIn(interrupter.TEAM_VARIABLE, str(raised.exception))
                self.assertIn("Signing.local.xcconfig.example", str(raised.exception))

    def test_launch_is_foreground_unless_asked(self) -> None:
        command, _ = interrupter.launch_command(launch_arguments())
        self.assertIn("--activate", command)
        self.assertNotIn("--no-activate", command)
        self.assertIn("--terminate-existing", command)
        self.assertEqual(command[-1], "com.imxieyi.KernariumAudioInterrupter")
        background, _ = interrupter.launch_command(launch_arguments("--background", "--console"))
        self.assertIn("--no-activate", background)
        self.assertNotIn("--activate", background)
        self.assertIn("--console", background)

    def test_launch_settings_travel_as_child_environment(self) -> None:
        _, environment = interrupter.launch_command(
            launch_arguments("--mode", "switch", "--hold", "12", "--end", "notify", "--max", "99", "--retry", "2"))
        self.assertEqual(environment, {
            "DEVICECTL_CHILD_AUDIO_INTERRUPTER_MODE": "switch",
            "DEVICECTL_CHILD_AUDIO_INTERRUPTER_HOLD": "12.0",
            "DEVICECTL_CHILD_AUDIO_INTERRUPTER_END": "notify",
            "DEVICECTL_CHILD_AUDIO_INTERRUPTER_MAX": "99.0",
            "DEVICECTL_CHILD_AUDIO_INTERRUPTER_RETRY": "2.0",
        })

    def test_build_passes_team_identifier_and_version(self) -> None:
        arguments = interrupter.parser().parse_args(["build", "--build-version", "7"])
        with mock.patch.dict(os.environ, {interrupter.TEAM_VARIABLE: TEAM}):
            command = interrupter.build_command(arguments)
        self.assertIn("DEVELOPMENT_TEAM=" + TEAM, command)
        self.assertIn("KERNARIUM_BUNDLE_IDENTIFIER=com.imxieyi.Kernarium", command)
        self.assertIn("CURRENT_PROJECT_VERSION=7", command)
        self.assertEqual(command[command.index("-scheme") + 1], "AudioInterrupter")
        self.assertIn("-allowProvisioningUpdates", command)

    def test_dry_run_prints_every_command_and_runs_none(self) -> None:
        environment = dict(os.environ, **{interrupter.TEAM_VARIABLE: TEAM})
        for arguments in (["build"], ["install"], ["launch", "--console"], ["terminate", "--pid", "1"]):
            done = subprocess.run(
                [sys.executable, "-B", "-I", str(SCRIPT), "--device", "UDID", "--dry-run", *arguments],
                capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=30, check=False,
                env=environment)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertTrue(done.stdout.startswith("+ "), done.stdout)

    def test_a_command_that_needs_a_device_says_so(self) -> None:
        done = subprocess.run(
            [sys.executable, "-B", "-I", str(SCRIPT), "--dry-run", "launch"],
            capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=30, check=False)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("--device is required", done.stderr)


class SourceTests(unittest.TestCase):
    def test_the_script_sets_exactly_what_the_application_reads(self) -> None:
        read = set(re.findall(r'environment\["(AUDIO_INTERRUPTER_[A-Z]+)"\]', swift_text()))
        _, environment = interrupter.launch_command(launch_arguments())
        sent = {name.removeprefix("DEVICECTL_CHILD_") for name in environment}
        self.assertEqual(read, sent)

    def test_the_script_offers_the_modes_and_ends_the_application_has(self) -> None:
        text = swift_text()
        for enum, offered in (("Mode", interrupter.MODES), ("End", interrupter.ENDS)):
            block = re.search(rf"enum {enum}: String \{{(.*?)\n    \}}", text, re.S)
            self.assertIsNotNone(block, enum)
            cases = re.findall(r"^\s+case `?(\w+)`?$", block.group(1), re.M)
            self.assertEqual(tuple(cases), tuple(offered), enum)

    def test_the_sources_compile_for_the_ios_sdk(self) -> None:
        done = subprocess.run(
            ["xcrun", "--sdk", "iphonesimulator", "swiftc", "-typecheck", "-swift-version", "5",
             "-target", "arm64-apple-ios27.0-simulator",
             "-enable-upcoming-feature", "MemberImportVisibility",
             "-enable-upcoming-feature", "NonisolatedNonsendingByDefault",
             "-enable-upcoming-feature", "InferIsolatedConformances",
             *map(str, SOURCES)],
            capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=300, check=False)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertNotIn("warning:", done.stderr)


class ProjectTests(unittest.TestCase):
    def test_the_application_declares_the_audio_background_mode(self) -> None:
        info = plistlib.loads((PROJECT / "Info.plist").read_bytes())
        self.assertEqual(info["UIBackgroundModes"], ["audio"])

    def test_the_project_files_are_valid(self) -> None:
        for path in (PROJECT / "AudioInterrupter.xcodeproj/project.pbxproj",
                     PROJECT / "AudioInterrupter.xcodeproj/xcshareddata/xcschemes/AudioInterrupter.xcscheme"):
            self.assertTrue(path.is_file(), path)
        done = subprocess.run(
            ["plutil", "-lint", str(PROJECT / "AudioInterrupter.xcodeproj/project.pbxproj")],
            capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=30, check=False)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)

    def test_the_project_names_no_team_and_derives_the_identifier(self) -> None:
        project = (PROJECT / "AudioInterrupter.xcodeproj/project.pbxproj").read_text()
        self.assertNotIn("DEVELOPMENT_TEAM", project)
        identifiers = set(re.findall(r"PRODUCT_BUNDLE_IDENTIFIER = (.*?);", project))
        self.assertEqual(identifiers, {'"$(KERNARIUM_BUNDLE_IDENTIFIER)AudioInterrupter"'})
        self.assertIn("IPHONEOS_DEPLOYMENT_TARGET = 27.0;", project)

    def test_the_scheme_builds_the_application(self) -> None:
        scheme = (PROJECT / "AudioInterrupter.xcodeproj/xcshareddata/xcschemes/AudioInterrupter.xcscheme").read_text()
        self.assertIn('BuildableName = "AudioInterrupter.app"', scheme)
        project = (PROJECT / "AudioInterrupter.xcodeproj/project.pbxproj").read_text()
        blueprint = re.search(r'BlueprintIdentifier = "(\w+)"', scheme)
        self.assertIsNotNone(blueprint)
        self.assertIn(f"{blueprint.group(1)} /* AudioInterrupter */ = {{", project)


if __name__ == "__main__":
    unittest.main()
