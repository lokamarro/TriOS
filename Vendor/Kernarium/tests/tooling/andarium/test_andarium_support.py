#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Andarium side of a device run: its settings, console lines and app."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import andarium  # noqa: E402
import ios_device  # noqa: E402
import kernarium  # noqa: E402

THERMAL = (
    "Andarium thermal state: phase=start state=nominal uptime=1.5\n"
    "Andarium thermal state: phase=change state=fair uptime=400.0\n"
    "Andarium thermal state: phase=change state=serious uptime=800.25\n"
    "Andarium thermal state: phase=finish state=fair uptime=900.0\n"
)


def settings(**keywords: object) -> dict[str, str]:
    return andarium.settings_for(
        **{"cpus": 2, "memory_mib": 1536, "engine": "gadgets",
           "host_port": 21100, **keywords}
    )


class SettingsTests(unittest.TestCase):
    def test_the_default_launch_starts_normally_on_a_loopback_forward(
        self
    ) -> None:
        self.assertEqual(settings(), {
            "ANDARIUM_START": "normal",
            "KERNARIUM_VCPU_COUNT": "2",
            "KERNARIUM_GUEST_MEMORY_MIB": "1536",
            "KERNARIUM_EXECUTION_ENGINE": "gadgets",
            "KERNARIUM_HOST_FORWARD": "tcp:127.0.0.1:21100-5555",
            "KERNARIUM_ANDROID_ADB_AUTH": "0",
        })

    def test_the_optional_knobs_are_set_only_when_asked_for(self) -> None:
        asked = settings(
            start="recovery", erase=True, disk_size_gib=8, extra={"KERNARIUM_FOOTPRINT_LOG_MS": "1000"},
        )
        self.assertEqual(asked["ANDARIUM_START"], "recovery")
        self.assertEqual(asked["ANDARIUM_ERASE"], "1")
        self.assertEqual(asked["KERNARIUM_ANDROID_DISK_SIZE_GIB"], "8")
        self.assertEqual(asked["KERNARIUM_FOOTPRINT_LOG_MS"], "1000")

    def test_a_setting_the_application_would_refuse_is_refused_here(
        self
    ) -> None:
        for keywords in (
            {"start": "auto"}, {"memory_mib": 512}, {"memory_mib": 8192},
            {"memory_mib": 1500}, {"cpus": 0}, {"host_port": 80},
            {"host_port": 65535},
            {"extra": {"PATH": "/usr/bin"}},
            {"extra": {"KERNARIUM_ANDROID_ADB_AUTH": "1"}},
            {"extra": {"ANDARIUM_START": "recovery"}},
        ):
            with self.subTest(keywords=keywords):
                with self.assertRaises(andarium.AndariumError):
                    settings(**keywords)

    def test_the_forward_port_comes_from_the_guest_slot(self) -> None:
        self.assertEqual(
            andarium.host_port_from_slot({"GUEST_SLOT_PORT_BASE": "21300"}),
            21300,
        )
        for environment in ({}, {"GUEST_SLOT_PORT_BASE": ""},
                            {"GUEST_SLOT_PORT_BASE": "x"}):
            with self.assertRaisesRegex(
                andarium.AndariumError, "GUEST_SLOT_PORT_BASE is not set"
            ):
                andarium.host_port_from_slot(environment)


class ConsoleTests(unittest.TestCase):
    def test_the_runtime_result_is_the_last_andarium_line(self) -> None:
        self.assertIsNone(andarium.runtime_result("Kernarium: boot\n"))
        self.assertEqual(
            andarium.runtime_result(
                "Andarium: runtime returned 3\nAndarium: runtime returned 0\n"
            ),
            0,
        )
        # Kernarium's line is not Andarium's.
        self.assertIsNone(
            andarium.runtime_result("Kernarium: runtime returned 0\n"))
        self.assertIsNone(
            kernarium.runtime_result("Andarium: runtime returned 0\n"))

    def test_the_thermal_record_has_the_start_the_finish_and_the_changes(
        self
    ) -> None:
        record = andarium.thermal_record("noise\n" + THERMAL)
        self.assertEqual(record["start"],
                         {"state": "nominal", "uptime_seconds": 1.5})
        self.assertEqual(record["finish"],
                         {"state": "fair", "uptime_seconds": 900.0})
        self.assertEqual([c["state"] for c in record["changes"]],
                         ["fair", "serious"])
        self.assertTrue(record["complete"])

    def test_a_run_missing_either_end_is_incomplete(self) -> None:
        start, change, _, finish = THERMAL.splitlines()
        for lines in ([start], [change, finish], []):
            with self.subTest(lines=lines):
                record = andarium.thermal_record("\n".join(lines))
                self.assertFalse(record["complete"])


class ApplicationTests(unittest.TestCase):
    def test_a_launch_clears_both_families_and_keeps_the_rest(self) -> None:
        inherited = {
            "DEVICECTL_CHILD_KERNARIUM_ROOTFS": "x",
            "DEVICECTL_CHILD_ANDARIUM_ERASE": "1",
            "DEVICECTL_CHILD_UML_GPU_FRAME_STATS": "1",
            "UNRELATED_SETTING": "kept",
        }
        with mock.patch.dict(ios_device.os.environ, inherited):
            environment = ios_device.device_launch_environment(
                andarium.APPLICATION, settings())
            kernarium_environment = ios_device.device_launch_environment(
                kernarium.APPLICATION, {})
        self.assertNotIn("DEVICECTL_CHILD_KERNARIUM_ROOTFS", environment)
        self.assertNotIn("DEVICECTL_CHILD_ANDARIUM_ERASE", environment)
        self.assertEqual(environment["DEVICECTL_CHILD_ANDARIUM_START"],
                         "normal")
        self.assertEqual(environment["DEVICECTL_CHILD_UML_GPU_FRAME_STATS"],
                         "1")
        # Kernarium still clears only its own family.
        self.assertNotIn("DEVICECTL_CHILD_KERNARIUM_ROOTFS",
                         kernarium_environment)
        self.assertEqual(
            kernarium_environment["DEVICECTL_CHILD_ANDARIUM_ERASE"], "1")

    def test_the_focus_is_what_the_console_said_last(self) -> None:
        self.assertTrue(andarium.application_is_active(""))
        self.assertFalse(andarium.application_is_active(
            "x\nAndarium: application inactive\n"))
        self.assertTrue(andarium.application_is_active(
            "Andarium: application inactive\nAndarium: application active\n"))
        self.assertFalse(andarium.application_is_active(
            "Andarium: application active\nAndarium: application inactive\n"))

    def test_the_two_applications_are_told_apart(self) -> None:
        self.assertEqual(andarium.APPLICATION.bundle_id,
                         "com.imxieyi.andarium")
        self.assertEqual(kernarium.APPLICATION.bundle_id,
                         "com.imxieyi.Kernarium")

    def test_only_the_automation_android_runtime_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            app = pathlib.Path(name) / "Andarium.app"
            with self.assertRaisesRegex(andarium.AndariumError, "no KRuntime"):
                andarium.require_automation_app(app)
            for framework in andarium.ANDROID_RUNTIME_FRAMEWORKS:
                directory = app / "Frameworks" / f"{framework}.framework"
                directory.mkdir(parents=True)
                (directory / framework).write_bytes(b"release runtime")
            with self.assertRaisesRegex(andarium.AndariumError, "release"):
                andarium.require_automation_app(app)
            for framework in andarium.ANDROID_RUNTIME_FRAMEWORKS:
                directory = app / "Frameworks" / f"{framework}.framework"
                (directory / framework).write_bytes(
                    b"x\0KERNARIUM_ANDROID_ADB_AUTH\0y")
            andarium.require_automation_app(app)

    def test_the_console_must_report_authorization_off(self) -> None:
        andarium.require_adb_authorization_off(andarium.ADB_AUTH_OFF_LINE)
        with self.assertRaisesRegex(andarium.AndariumError, "release runtime"):
            andarium.require_adb_authorization_off("Linux version 6.18\n")


if __name__ == "__main__":
    unittest.main()
