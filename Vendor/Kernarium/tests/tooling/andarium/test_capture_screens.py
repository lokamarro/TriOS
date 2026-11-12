#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Andarium/scripts/capture-screens.py takes one run at a time into a directory.

Two runs into the same --out would share the screenshots' working files and
read and write manifest.json. The second run refuses at once and names the
first; the lock is released when a run ends, however it ends. No test here
needs a Simulator: the Simulator calls are replaced, and a refused run stops
before any is made.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import os
import pathlib
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPOSITORY / "Andarium/scripts/capture-screens.py"
BOUND_SECONDS = 30

HOLDER = """
import importlib.util, pathlib, sys, time
spec = importlib.util.spec_from_file_location("capture_screens", sys.argv[1])
module = importlib.util.module_from_spec(spec)
sys.modules["capture_screens"] = module
spec.loader.exec_module(module)
with module.output_lock(pathlib.Path(sys.argv[2])):
    print("held", flush=True)
    time.sleep(120)
"""


def load_script():
    spec = importlib.util.spec_from_file_location("capture_screens", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules["capture_screens"] = module
    spec.loader.exec_module(module)
    return module


class CaptureLockTests(unittest.TestCase):
    def setUp(self):
        self.script = load_script()
        self.holders: list[subprocess.Popen] = []
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.out = pathlib.Path(scratch.name) / "shots"

    def tearDown(self):
        for holder in self.holders:
            holder.kill()
            holder.wait(timeout=BOUND_SECONDS)
            holder.stdout.close()

    def run_main(self, *extra: str) -> tuple[object, str]:
        """The script's main() with the Simulator calls replaced, and what it printed to stderr."""
        stderr = io.StringIO()
        with mock.patch.object(self.script, "device_names", return_value={"UDID": "Fake iPad"}), \
                mock.patch.object(self.script, "simctl"), \
                mock.patch.object(self.script, "preflight", return_value="portrait"), \
                contextlib.redirect_stderr(stderr):
            try:
                result = self.script.main(["--out", str(self.out), "--device", "UDID", "--only", "^no-such-shot$", *extra])
            except SystemExit as exit:
                result = exit.code
        return result, stderr.getvalue()

    def hold_from_another_process(self) -> subprocess.Popen:
        self.out.mkdir(parents=True, exist_ok=True)
        holder = subprocess.Popen(
            [sys.executable, "-B", "-c", HOLDER, str(SCRIPT), str(self.out)],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        )
        self.holders.append(holder)
        ready, _, _ = select.select([holder.stdout], [], [], BOUND_SECONDS)
        self.assertTrue(ready, "the holder did not take the lock in time")
        self.assertEqual(holder.stdout.readline().strip(), "held")
        return holder

    def assertFree(self):
        with self.script.output_lock(self.out):
            pass

    def test_a_run_into_a_held_directory_refuses_at_once_and_names_the_holder(self):
        self.out.mkdir()
        with self.script.output_lock(self.out):
            started = time.monotonic()
            with mock.patch.object(self.script, "device_names", side_effect=AssertionError("a refused run reached the Simulator")):
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    result = self.script.main(["--out", str(self.out), "--device", "UDID"])
            self.assertLess(time.monotonic() - started, 5)
        self.assertEqual(result, 1)
        message = stderr.getvalue()
        self.assertIn("another capture is running", message)
        self.assertIn(str(self.out), message)
        self.assertIn(f"pid {os.getpid()}", message)
        self.assertFalse((self.out / "manifest.json").exists(), "a refused run wrote the manifest")

    def test_the_command_line_refuses_too_and_the_message_names_the_other_process(self):
        holder = self.hold_from_another_process()
        done = subprocess.run(
            [sys.executable, "-B", str(SCRIPT), "--out", str(self.out), "--device", "NO-SUCH-UDID"],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=BOUND_SECONDS, check=False,
        )
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn("another capture is running", done.stderr)
        self.assertIn(f"pid {holder.pid}", done.stderr)
        self.assertNotIn("no Simulator", done.stderr)
        self.assertFalse((self.out / "manifest.json").exists())

    def test_a_killed_holder_leaves_the_directory_free(self):
        holder = self.hold_from_another_process()
        with self.assertRaises(self.script.OutputBusy):
            with self.script.output_lock(self.out):
                pass
        holder.send_signal(signal.SIGKILL)
        holder.wait(timeout=BOUND_SECONDS)
        self.assertFree()

    def test_the_lock_is_released_when_a_run_ends(self):
        self.assertEqual(self.run_main()[0], 0)
        self.assertTrue((self.out / "manifest.json").is_file())
        self.assertFree()
        self.assertEqual(self.run_main()[0], 0, "a second run after the first")

    def test_the_lock_is_released_when_a_run_stops_on_an_unknown_device(self):
        stderr = io.StringIO()
        with mock.patch.object(self.script, "device_names", return_value={}), contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as stopped:
                self.script.main(["--out", str(self.out), "--device", "UDID"])
        self.assertIn("no Simulator UDID", str(stopped.exception.code))
        self.assertFree()

    def test_the_lock_is_released_when_a_run_raises(self):
        with mock.patch.object(self.script, "device_names", side_effect=RuntimeError("simctl failed")):
            with self.assertRaises(RuntimeError):
                self.script.main(["--out", str(self.out), "--device", "UDID"])
        self.assertFree()

    def test_the_lock_is_released_when_a_run_is_interrupted(self):
        with mock.patch.object(self.script, "device_names", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.script.main(["--out", str(self.out), "--device", "UDID"])
        self.assertFree()

    def test_other_directories_are_not_held(self):
        self.out.mkdir()
        other = self.out.parent / "other"
        other.mkdir()
        with self.script.output_lock(self.out), self.script.output_lock(other):
            pass

    def test_listing_the_shots_needs_no_directory_and_takes_no_lock(self):
        listing = io.StringIO()
        with self.script.output_lock(self.out.parent), contextlib.redirect_stdout(listing):
            self.assertEqual(self.script.main(["--list"]), 0)
        self.assertIn("home\tHome", listing.getvalue())


if __name__ == "__main__":
    unittest.main()
