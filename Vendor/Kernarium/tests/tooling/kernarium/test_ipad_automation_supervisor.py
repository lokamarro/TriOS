#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The iPad automation script signals only the supervisor it started.

`start` records the supervisor's pid, and nothing clears that record when the
host ends. By the time `stop --force` or `status` reads it, the pid can name
any process: another agent's process-group leader, another user's process, or
nothing at all. The script must treat such a record as stale rather than
signal an unrelated process group or end in a traceback. Nothing here talks to
a device: the device channel is replaced with one that has no host.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


SCRIPT = Path(__file__).resolve().parents[3] / "Kernarium/scripts/ipad-automation.py"
SPEC = importlib.util.spec_from_file_location("ipad_automation", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
automation = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(automation)

DEVICE = "00000000-0000000000000000"
NONCE = "0123456789abcdef0123456789abcdef"


def no_host_send(options, command, timeout):
    raise automation.NoHost("the host is not running: test channel")


class SupervisorRecordTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.processes: list[subprocess.Popen] = []
        self.addCleanup(self.reap)
        self.saved = (automation.host_state, automation.send,
                      automation.SUPERVISOR_EXIT_WAIT)
        automation.host_state = lambda options: None
        automation.send = no_host_send
        automation.SUPERVISOR_EXIT_WAIT = 1
        self.addCleanup(self.restore)

    def restore(self) -> None:
        (automation.host_state, automation.send,
         automation.SUPERVISOR_EXIT_WAIT) = self.saved

    def reap(self) -> None:
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)

    def spawn(self, *arguments: str) -> subprocess.Popen:
        """A process-group leader of this test's own, as `start` makes one.

        Returns once `ps` shows its arguments. A framework Python's launcher
        re-execs the interpreter, and until that exec completes `ps` shows
        only "(python3.14)". `start` returns only after the host serves, so
        `stop` and `status` never meet a supervisor that young."""
        process = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(60)", *arguments],
            stdin=subprocess.DEVNULL, start_new_session=True)
        self.processes.append(process)
        expected = ["time.sleep(60)", *arguments]
        deadline = time.monotonic() + 10
        while True:
            shown = subprocess.run(
                ["ps", "-o", "command=", "-p", str(process.pid)],
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, timeout=10).stdout
            if shown.split()[-len(expected):] == expected:
                return process
            if time.monotonic() > deadline:
                self.fail(f"ps never showed the arguments of {process.pid}: {shown!r}")
            time.sleep(0.01)

    def options(self, *command: str):
        return automation.parse(
            ["--device", DEVICE, "--state-dir", self.scratch.name, *command])

    def record(self, pid: int) -> None:
        directory = Path(self.scratch.name) / DEVICE
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "host.json").write_text(json.dumps({
            "supervisor": pid, "log": str(directory / "host.log"),
            "nonce": NONCE, "started": 0, "seconds": 3600}))

    def status(self) -> dict:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(automation.command_status(self.options("status")),
                             automation.EXIT_NO_HOST)
        return json.loads(output.getvalue())

    def stop(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(
                automation.command_stop(self.options("stop", "--force")), 0)

    def test_a_reused_pid_leading_another_group_is_left_alone(self) -> None:
        unrelated = self.spawn()
        self.record(unrelated.pid)
        self.assertFalse(self.status()["supervisor_running"])
        self.stop()
        self.assertIsNone(unrelated.poll())

    def test_a_supervisor_of_another_host_is_left_alone(self) -> None:
        other = self.spawn("_supervise", "--nonce", "f" * 32)
        self.record(other.pid)
        self.assertFalse(self.status()["supervisor_running"])
        self.stop()
        self.assertIsNone(other.poll())

    def test_another_users_process_is_not_signalled(self) -> None:
        # launchd: signalling it raises PermissionError.
        self.record(1)
        self.assertFalse(self.status()["supervisor_running"])
        self.stop()

    def test_an_exited_supervisor_is_stale(self) -> None:
        gone = subprocess.Popen([sys.executable, "-c", "pass"],
                                stdin=subprocess.DEVNULL)
        gone.wait(timeout=30)
        self.record(gone.pid)
        self.assertFalse(self.status()["supervisor_running"])
        self.stop()

    def test_the_recorded_supervisor_is_stopped(self) -> None:
        supervisor = self.spawn("_supervise", "--nonce", NONCE)
        self.record(supervisor.pid)
        self.assertTrue(self.status()["supervisor_running"])
        self.stop()
        self.assertEqual(supervisor.wait(timeout=30), -15)


if __name__ == "__main__":
    unittest.main()
