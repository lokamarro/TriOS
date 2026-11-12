#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin what the shared probe runner asks of a guest and when it fails.

tests/integration/support/run_guest_probe.py boots a guest, runs one Python or
C++ probe and fails on the probe's exit status or on a console line. This test
replaces the guest with a recorder and checks the files the runner installs,
the commands it sends in order, and which outcomes raise, so no emulator
starts.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

SUPPORT = pathlib.Path(__file__).resolve().parents[2] / "integration/support"
sys.path.insert(0, str(SUPPORT))
SPEC = importlib.util.spec_from_file_location(
    "run_guest_probe", SUPPORT / "run_guest_probe.py"
)
assert SPEC is not None and SPEC.loader is not None
run_guest_probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_guest_probe)

GUEST_DIRECTORY = "/usr/local/libexec/uml-test"
MOUNT_TMP = ("check", "mount -t tmpfs -o mode=1777 tmpfs /tmp", 20)
ALIVE = ("check", "echo alive", 20)
BUILD = f"c++ -std=c++20 -O2 {{}}-o /tmp/probe {GUEST_DIRECTORY}/probe.cpp"
KERNEL_LINE = "[    1.2] Darwin: tlb_sync failed (-14) ip=0x1 sp=0x2"


class FakeGuest:
    """Records the session; the probe's outcome is fixed by the test."""

    def __init__(self, return_code: int, console: str) -> None:
        self.return_code = return_code
        self.console = console
        self.events: list[tuple] = []
        self.constructed: dict = {}

    def __call__(self, *positional, **keywords) -> FakeGuest:
        self.constructed = {"positional": positional, **keywords}
        return self

    def __enter__(self) -> FakeGuest:
        return self

    def __exit__(self, *exception: object) -> None:
        pass

    def boot_to_shell(self, timeout: int) -> None:
        self.events.append(("boot", timeout))

    def check_command(self, command: str, timeout: int) -> None:
        self.events.append(("check", command, timeout))

    def run_command(self, command: str, timeout: int):
        self.events.append(("run", command, timeout))
        return mock.Mock(return_code=self.return_code, output="probe output\n")

    def output(self) -> str:
        return self.console


class ProbeRunnerTests(unittest.TestCase):
    def run_probe(
        self,
        probe: str,
        *options: str,
        return_code: int = 0,
        console: str = "",
        tool: str | None = None,
    ) -> tuple[FakeGuest, str, Exception | None]:
        guest = FakeGuest(return_code, console)
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / probe
            source.write_bytes(b"probe source")
            if tool is not None:
                executable = pathlib.Path(directory) / "input-tool"
                executable.write_text(tool)
                executable.chmod(0o755)
                options += ("--probe-input", str(executable))
            arguments = [
                "run_guest_probe.py",
                "--emulator", "/emulator/uml-darwin",
                "--rootfs", "/images/rootfs.erofs",
                "--probe", str(source),
                "--ncpus", "2",
                "--probe-timeout", "120",
                *options,
            ]
            output = io.StringIO()
            failure = None
            with (
                mock.patch.object(sys, "argv", arguments),
                mock.patch.object(run_guest_probe, "Guest", guest),
                contextlib.redirect_stdout(output),
            ):
                try:
                    self.assertEqual(run_guest_probe.main(), 0)
                except RuntimeError as error:
                    failure = error
        return guest, output.getvalue(), failure

    def test_python_probe_runs_under_python_and_a_final_command(self) -> None:
        guest, output, failure = self.run_probe("probe_a.py")
        self.assertIsNone(failure)
        self.assertEqual(output, "probe output\n")
        positional = guest.constructed["positional"]
        self.assertEqual(
            positional[2:5], ("1G", "tmpfs", ("ncpus=2",)))
        self.assertTrue(guest.constructed["init_shell"])
        self.assertEqual(guest.constructed["engine"], "jit")
        installed = guest.constructed["payloads"]
        self.assertEqual(list(installed), [f"{GUEST_DIRECTORY}/probe.py"])
        self.assertEqual(installed[f"{GUEST_DIRECTORY}/probe.py"].contents,
                         b"probe source")
        self.assertEqual(guest.events, [
            ("boot", 180),
            MOUNT_TMP,
            ("run", f"python3 -B {GUEST_DIRECTORY}/probe.py", 120),
            ALIVE,
        ])

    def test_a_failing_python_probe_fails_before_the_final_command(
            self) -> None:
        guest, output, failure = self.run_probe("probe_a.py", return_code=3)
        self.assertIn("probe_a.py reported 3 failure(s)", str(failure))
        self.assertEqual(output, "probe output\n")
        self.assertNotIn(ALIVE, guest.events)

    def test_cpp_probe_is_built_in_the_guest_with_its_flags(self) -> None:
        guest, _, failure = self.run_probe(
            "probe_b.cpp", "--cxxflags=-g -fno-omit-frame-pointer -pthread")
        self.assertIsNone(failure)
        installed = guest.constructed["payloads"]
        self.assertEqual(list(installed), [f"{GUEST_DIRECTORY}/probe.cpp"])
        self.assertEqual(guest.events, [
            ("boot", 180),
            MOUNT_TMP,
            ("check", BUILD.format("-g -fno-omit-frame-pointer -pthread "),
             180),
            ("run", "/tmp/probe", 120),
            ALIVE,
        ])

    def test_cpp_probe_takes_one_flag_or_none(self) -> None:
        for options, flags in (
            ((), ""),
            (("--cxxflags=-pthread",), "-pthread "),
        ):
            with self.subTest(options=options):
                guest, _, failure = self.run_probe("probe_b.cpp", *options)
                self.assertIsNone(failure)
                self.assertIn(
                    ("check", BUILD.format(flags), 180), guest.events)

    def test_a_failing_cpp_probe_fails_before_the_final_command(
            self) -> None:
        guest, _, failure = self.run_probe("probe_b.cpp", return_code=2)
        self.assertIn("probe_b.cpp reported 2 failure(s)", str(failure))
        self.assertNotIn(ALIVE, guest.events)

    def test_a_probe_input_tool_runs_first_and_its_output_is_installed(
            self) -> None:
        guest, _, failure = self.run_probe(
            "probe_a.py", tool="#!/bin/sh\nprintf 'device\\nreports'\n")
        self.assertIsNone(failure)
        installed = guest.constructed["payloads"]
        self.assertEqual(list(installed), [
            f"{GUEST_DIRECTORY}/probe.py",
            f"{GUEST_DIRECTORY}/probe-input",
        ])
        self.assertEqual(
            installed[f"{GUEST_DIRECTORY}/probe-input"].contents,
            b"device\nreports")
        self.assertEqual(
            installed[f"{GUEST_DIRECTORY}/probe.py"].contents,
            b"probe source")

    def test_a_failing_probe_input_tool_ends_the_run_before_a_boot(
            self) -> None:
        guest, _, failure = self.run_probe(
            "probe_a.py", tool="#!/bin/sh\necho 'no model' >&2\nexit 3\n")
        self.assertEqual(
            str(failure), "input-tool failed (3): no model")
        self.assertEqual(guest.constructed, {})
        self.assertEqual(guest.events, [])

    def test_flags_belong_to_a_cpp_probe(self) -> None:
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            with self.assertRaises(SystemExit) as stopped:
                self.run_probe("probe_a.py", "--cxxflags=-Wall -Werror")
        self.assertEqual(stopped.exception.code, 2)
        self.assertIn("--cxxflags applies to a .cpp probe", errors.getvalue())

    def test_a_console_line_fails_a_probe_that_exits_zero(self) -> None:
        console = f"boot\n{KERNEL_LINE}\nDarwin: ok\n"
        for probe in ("probe_a.py", "probe_b.cpp"):
            with self.subTest(probe=probe):
                guest, output, failure = self.run_probe(
                    probe, "--console-failure", "Darwin: .*failed",
                    console=console)
                self.assertEqual(str(failure), "1 console line(s) matched")
                self.assertEqual(
                    output, f"probe output\nFAIL console: {KERNEL_LINE}\n")
                self.assertNotIn(ALIVE, guest.events)

    def test_exit_status_and_console_line_are_both_reported(self) -> None:
        _, _, failure = self.run_probe(
            "probe_b.cpp", "--console-failure", "Darwin: .*failed",
            return_code=4, console=KERNEL_LINE)
        self.assertEqual(
            str(failure),
            "probe_b.cpp reported 4 failure(s); 1 console line(s) matched")

    def test_the_console_is_not_scanned_unless_asked(self) -> None:
        for options in ((), ("--console-failure", "no such line")):
            with self.subTest(options=options):
                _, _, failure = self.run_probe(
                    "probe_b.cpp", *options, console=KERNEL_LINE)
                self.assertIsNone(failure)


if __name__ == "__main__":
    unittest.main()
