#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Android runners' default adb forward port follows the guest slot.

Inside a guest slot (GUEST_SLOT_PORT_BASE) the default is the slot's first
port, so two runners in different slots never share a forward; outside one
each runner keeps its fixed port. The slot layout leaves the forward clear of
the range that private adb servers take.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import importlib.util
import os
from pathlib import Path
import sys
import unittest
from unittest import mock

TESTS = Path(__file__).resolve().parents[2]
SUPPORT = TESTS / "integration" / "support"
RUNNERS = {
    "run_android_adb_debugging": (TESTS / "integration/android/run_android_adb_debugging.py", 5575),
    "run_android_oobe": (TESTS / "integration/android/run_android_oobe.py", 5576),
    "android_launch_suite": (TESTS / "perf/android_launch_suite.py", 5601),
}
SLOT = "21300"


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    # Dataclasses resolve their annotations through sys.modules.
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


sys.path.insert(0, str(SUPPORT))
import slot_ports  # noqa: E402

PARSERS = {name: load(name, path).build_parser for name, (path, _) in RUNNERS.items()}


class SlotPortTests(unittest.TestCase):
    def test_no_slot_is_the_fallback(self) -> None:
        for environ in ({}, {slot_ports.SLOT_PORT_VARIABLE: ""}):
            self.assertIsNone(slot_ports.slot_base(environ))
            self.assertEqual(slot_ports.slot_port(slot_ports.ADB_OFFSET, 5575, environ), 5575)

    def test_a_slot_gives_its_port_at_the_offset(self) -> None:
        environ = {slot_ports.SLOT_PORT_VARIABLE: SLOT}
        self.assertEqual(slot_ports.slot_base(environ), 21300)
        self.assertEqual(slot_ports.slot_port(0, 5575, environ), 21300)
        self.assertEqual(slot_ports.slot_port(7, 5575, environ), 21307)

    def test_an_unusable_base_is_refused(self) -> None:
        for value in ("x", "-1", "21300.5", "80", "65500"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                slot_ports.slot_base({slot_ports.SLOT_PORT_VARIABLE: value})

    def test_the_forward_is_clear_of_the_private_server_range(self) -> None:
        self.assertEqual(slot_ports.ADB_OFFSET, 0)
        self.assertLess(slot_ports.ADB_OFFSET, slot_ports.PRIVATE_SERVER_FLOOR)
        self.assertLess(slot_ports.PRIVATE_SERVER_FLOOR, slot_ports.SLOT_PORTS)


class RunnerDefaultTests(unittest.TestCase):
    def default_port(self, name: str, *arguments: str, **variables: str) -> int:
        with mock.patch.dict(os.environ):
            os.environ.pop(slot_ports.SLOT_PORT_VARIABLE, None)
            os.environ.update(variables)
            return PARSERS[name]().parse_args(list(arguments)).adb_port

    def test_outside_a_slot_each_runner_keeps_its_fixed_port(self) -> None:
        for name, (_, fixed) in RUNNERS.items():
            with self.subTest(runner=name):
                self.assertEqual(self.default_port(name), fixed)

    def test_inside_a_slot_the_default_is_the_slots_first_port(self) -> None:
        for name in RUNNERS:
            with self.subTest(runner=name):
                self.assertEqual(
                    self.default_port(name, **{slot_ports.SLOT_PORT_VARIABLE: SLOT}), 21300)

    def test_an_explicit_port_wins(self) -> None:
        for name in RUNNERS:
            with self.subTest(runner=name):
                self.assertEqual(
                    self.default_port(name, "--adb-port", "6001",
                                      **{slot_ports.SLOT_PORT_VARIABLE: SLOT}), 6001)


if __name__ == "__main__":
    unittest.main()
