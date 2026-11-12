#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The iPad automation script sends rotations, coordinate gestures and Siri
requests.

`orientation` sets XCUIDevice's orientation only to one of the four interface
orientations and otherwise just reports it. `tap --x --y` and `drag` name
points as offsets from the application's frame origin, and a drag carries its
press, hold and optional velocity. A malformed gesture is refused before it
reaches the host, where XCTest would raise or act on a point nobody meant.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[3] / "Kernarium/scripts/ipad-automation.py"
SPEC = importlib.util.spec_from_file_location("ipad_automation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
automation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(automation)

DEVICE = ["--device", "00000000-0000000000000000"]


def command(*arguments: str) -> dict:
    return automation.build_command(automation.parse([*DEVICE, *arguments]))


def refused_by_parser(*arguments: str) -> bool:
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            automation.parse([*DEVICE, *arguments])
    except SystemExit as error:
        return error.code != 0
    return False


class OrientationTests(unittest.TestCase):
    def test_each_interface_orientation_is_sent_by_name(self) -> None:
        for name in ("portrait", "portraitUpsideDown", "landscapeLeft",
                     "landscapeRight"):
            with self.subTest(name=name):
                self.assertEqual(command("orientation", name),
                                 {"op": "orientation", "orientation": name})

    def test_without_a_name_the_orientation_is_only_reported(self) -> None:
        self.assertEqual(command("orientation"), {"op": "orientation"})

    def test_other_orientations_are_refused(self) -> None:
        # faceUp and faceDown are device orientations no interface follows.
        for name in ("faceUp", "landscape", "LandscapeLeft"):
            with self.subTest(name=name):
                self.assertTrue(refused_by_parser("orientation", name))


class DragTests(unittest.TestCase):
    def test_a_drag_carries_both_points_and_its_timing(self) -> None:
        self.assertEqual(
            command("drag", "10", "20", "30.5", "40"),
            {"op": "drag", "x1": 10.0, "y1": 20.0, "x2": 30.5, "y2": 40.0,
             "press": 0.5, "hold": 0})
        self.assertEqual(
            command("drag", "1", "2", "3", "4", "2", "--hold", "0.25",
                    "--velocity", "300"),
            {"op": "drag", "x1": 1.0, "y1": 2.0, "x2": 3.0, "y2": 4.0,
             "press": 2.0, "hold": 0.25, "velocity": 300.0})

    def test_the_target_application_is_chosen_by_bundle(self) -> None:
        options = automation.parse(
            [*DEVICE, "--bundle", "com.apple.springboard",
             "drag", "0", "0", "1", "1"])
        self.assertEqual(options.bundle, "com.apple.springboard")

    def test_negative_offsets_are_points_not_options(self) -> None:
        # A point above or left of a window's origin is a negative offset.
        self.assertEqual(command("drag", "-5", "-6", "7", "8")["x1"], -5.0)

    def test_malformed_drags_are_refused(self) -> None:
        for arguments in (("drag", "1", "2", "3", "4", "-1"),
                          ("drag", "1", "2", "3", "4", "--hold", "-1"),
                          ("drag", "1", "2", "3", "4", "--velocity", "0")):
            with self.subTest(arguments=arguments):
                with self.assertRaises(ValueError):
                    command(*arguments)
        self.assertTrue(refused_by_parser("drag", "1", "2", "3"))
        self.assertTrue(refused_by_parser("drag", "1", "2", "3", "x"))


class CoordinateTapTests(unittest.TestCase):
    def test_a_coordinate_tap_sends_both_offsets(self) -> None:
        sent = command("tap", "--x", "12", "--y", "34")
        self.assertEqual((sent["op"], sent["x"], sent["y"]), ("tap", 12.0, 34.0))

    def test_a_coordinate_tap_needs_both_offsets(self) -> None:
        self.assertTrue(refused_by_parser("tap", "--x", "12"))



class SiriTests(unittest.TestCase):
    def test_a_siri_request_carries_its_typed_text(self) -> None:
        self.assertEqual(command("siri", "What time is it"),
                         {"op": "siri", "text": "What time is it"})

    def test_a_siri_request_needs_text(self) -> None:
        self.assertTrue(refused_by_parser("siri"))


if __name__ == "__main__":
    unittest.main()
