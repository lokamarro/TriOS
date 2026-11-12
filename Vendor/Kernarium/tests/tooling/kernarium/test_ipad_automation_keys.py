#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The iPad automation script sends hardware keys XCTest actually delivers.

On a device, XCTest synthesizes nothing that reaches the application for the
characters of XCUIKeyboardKey.return, .delete and .escape, or for a
backspace, and still reports success; "\\n" produces the real Return key. The
script resolves key names before a command reaches the host, so `key return`
must send "\\n" and a key XCTest would drop must fail with a working route
instead of silently succeeding.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[3] / "Kernarium/scripts/ipad-automation.py"
SPEC = importlib.util.spec_from_file_location("ipad_automation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
automation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(automation)

DEVICE = ["--device", "00000000-0000000000000000"]


def key_command(*arguments: str) -> dict:
    return automation.build_command(
        automation.parse([*DEVICE, "key", *arguments]))


class KeyCommandTests(unittest.TestCase):
    def test_return_is_sent_as_the_character_xctest_delivers(self) -> None:
        self.assertEqual(
            key_command("return"),
            {"op": "key", "key": "\n", "modifiers": []})
        self.assertEqual(
            key_command("return", "--modifiers", "shift,control")["key"], "\n")
        # A carriage return names the same key.
        self.assertEqual(key_command("\r")["key"], "\n")

    def test_named_keys_resolve_to_xcuikeyboardkey_characters(self) -> None:
        # The arrows are XCUIKeyboardKey's iOS values. Its macOS values,
        # U+F700 to U+F703, press nothing on a device.
        for name, character in (
            ("tab", "\t"), ("space", " "), ("up", "\u2191"),
            ("down", "\u2193"), ("left", "\u2190"), ("right", "\u2192"),
        ):
            with self.subTest(name=name):
                self.assertEqual(key_command(name)["key"], character)

    def test_characters_pass_through(self) -> None:
        self.assertEqual(
            key_command("c", "--modifiers", "control"),
            {"op": "key", "key": "c", "modifiers": ["control"]})
        self.assertEqual(key_command("\n")["key"], "\n")

    def test_keys_xctest_drops_are_refused_with_a_working_route(self) -> None:
        for key, route in (
            ("delete", "type $'\\x7f'"), ("\x7f", "type $'\\x7f'"),
            ("\b", "type $'\\x7f'"), ("escape", "tap --label Escape"),
            ("\x1b", "tap --label Escape"),
        ):
            with self.subTest(key=repr(key)):
                with self.assertRaises(ValueError) as refused:
                    key_command(key)
                self.assertIn(route, str(refused.exception))

    def test_unknown_names_are_refused(self) -> None:
        with self.assertRaises(ValueError):
            key_command("enter")

    def test_only_single_utf16_characters_reach_the_host(self) -> None:
        # XCTest's typeKey raises an Objective-C exception for anything but
        # one UTF-16 code unit, which a host cannot survive.
        for key in ("UIKeyInputUpArrow", "", "ab", "\U0001f600", "e\u0301"):
            with self.subTest(key=repr(key)):
                with self.assertRaises(ValueError):
                    key_command(key)
        self.assertEqual(key_command("\u00e9")["key"], "\u00e9")


if __name__ == "__main__":
    unittest.main()
