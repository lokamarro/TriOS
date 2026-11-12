#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin how the fixture probe runner runs a probe and how it reports a failure.

tests/integration/support/fixture_transfer.py boots a guest with a static probe
installed, runs it and reads back what every step printed. This test replaces
the guest with a recorder and checks the command the guest is given and the
message of each way a run can fail, so no emulator starts. A failure has to
name the probe, its exit status, the step and the last thing it said, and it
has to reach the runner as an exception rather than end the guest first.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

SUPPORT = pathlib.Path(__file__).resolve().parents[2] / "integration/support"
sys.path.insert(0, str(SUPPORT))
import fixture_transfer  # noqa: E402

PASS_MARKER = "UML_PROBE_TEST: PASS"


class FakeGuest:
    """Records the session; what the probe printed and its status are fixed."""

    def __init__(self, probe_status: int, report: str) -> None:
        self.probe_status = probe_status
        self.report = report
        self.commands: list[tuple[str, int]] = []
        self.constructed: dict = {}
        self.shut_down = False
        self.return_code = 0

    def __call__(self, *positional, **keywords) -> FakeGuest:
        self.constructed = {"positional": positional, **keywords}
        return self

    def __enter__(self) -> FakeGuest:
        return self

    def __exit__(self, *exception: object) -> None:
        pass

    def boot_and_login(self, timeout: int) -> None:
        self.commands.append(("boot", timeout))

    def run_command(self, command: str, timeout: int):
        self.commands.append((command, timeout))
        if command.startswith("cat "):
            return mock.Mock(return_code=0, output=self.report)
        return mock.Mock(return_code=self.probe_status, output="")

    def shutdown(self, timeout: int) -> bool:
        self.shut_down = True
        return True

    def output(self) -> str:
        return ""


class FixtureTransferTests(unittest.TestCase):
    def run_probe(
        self, status: int, report: str, **keywords
    ) -> tuple[FakeGuest, str, Exception | None]:
        guest = FakeGuest(status, report)
        with tempfile.TemporaryDirectory() as directory:
            probe = pathlib.Path(directory) / "demo_probe.arm64"
            probe.write_bytes(b"\x7fELF probe")
            arguments = argparse.Namespace(
                emulator=pathlib.Path("/emulator/uml-darwin"),
                rootfs=pathlib.Path("/images/rootfs.erofs"),
                probe=probe,
                log=None,
            )
            output = io.StringIO()
            failure = None
            with (
                mock.patch.object(fixture_transfer, "Guest", guest),
                contextlib.redirect_stdout(output),
            ):
                try:
                    fixture_transfer.run_probe(
                        arguments,
                        subject="demo regression",
                        kernel_arguments=("ncpus=2",),
                        probe_timeout_seconds=30,
                        command_timeout_seconds=60,
                        pass_marker=PASS_MARKER,
                        probe_arguments=("--flag", "value"),
                        **keywords,
                    )
                except RuntimeError as error:
                    failure = error
        return guest, output.getvalue(), failure

    def test_a_passing_probe_runs_every_step_in_a_subshell(self) -> None:
        guest, output, failure = self.run_probe(
            0, f"UML_PROBE_STEP: probe\n{PASS_MARKER}\n", precondition="test -c /dev/x"
        )
        self.assertIsNone(failure)
        self.assertEqual(output, f"UML_PROBE_STEP: probe\n{PASS_MARKER}\n\n")
        self.assertTrue(guest.shut_down)
        boot, run, report = guest.commands
        self.assertEqual(boot, ("boot", fixture_transfer.BOOT_TIMEOUT_SECONDS))
        command, timeout = run
        self.assertEqual(timeout, 60)
        # The session shell is the guest's PID 1: a bare `set -e` would end it.
        self.assertTrue(command.startswith("( set -e; "), command)
        self.assertTrue(
            command.endswith(f") >{fixture_transfer.RESULT_LOG} 2>&1"), command
        )
        steps = [
            "precondition", "guest page size", "payload directory",
            "payload checksums", "probe",
        ]
        positions = [
            command.index(f"echo '{fixture_transfer.STEP_MARKER}{step}'")
            for step in steps
        ]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("test -c /dev/x", command)
        self.assertIn("timeout 30 ./probe --flag value", command)
        self.assertEqual(
            report[0], f"cat {fixture_transfer.RESULT_LOG}")

    def test_a_failing_probe_is_named_with_its_status_step_and_message(
        self,
    ) -> None:
        guest, output, failure = self.run_probe(
            1,
            "UML_PROBE_STEP: payload checksums\nUML_PROBE_STEP: probe\n"
            "UML_PROBE_TEST: FAIL the device is gone\n",
        )
        self.assertEqual(
            str(failure),
            "guest demo regression failed: probe demo_probe.arm64 exited 1 in "
            "step 'probe': UML_PROBE_TEST: FAIL the device is gone",
        )
        self.assertIn("FAIL the device is gone", output)
        self.assertFalse(guest.shut_down)

    def test_a_step_that_says_nothing_is_still_named(self) -> None:
        _, _, failure = self.run_probe(
            1, "UML_PROBE_STEP: precondition\n", precondition="false"
        )
        self.assertEqual(
            str(failure),
            "guest demo regression failed: probe demo_probe.arm64 exited 1 in "
            "step 'precondition': it printed nothing",
        )

    def test_a_probe_that_exits_zero_without_the_marker_fails(self) -> None:
        _, _, failure = self.run_probe(
            0, "UML_PROBE_STEP: probe\nall good, probably\n"
        )
        self.assertEqual(
            str(failure),
            f"guest demo regression failed: probe demo_probe.arm64 did not "
            f"print {PASS_MARKER!r}: all good, probably",
        )

    def test_a_kernel_failure_in_the_transcript_fails_a_passing_probe(
        self,
    ) -> None:
        guest = FakeGuest(0, f"UML_PROBE_STEP: probe\n{PASS_MARKER}\n")
        guest.output = lambda: "Kernel panic - not syncing"  # type: ignore
        with tempfile.TemporaryDirectory() as directory:
            probe = pathlib.Path(directory) / "demo_probe.arm64"
            probe.write_bytes(b"\x7fELF probe")
            arguments = argparse.Namespace(
                emulator=pathlib.Path("/e"), rootfs=pathlib.Path("/r"),
                probe=probe, log=None,
            )
            with (
                mock.patch.object(fixture_transfer, "Guest", guest),
                contextlib.redirect_stdout(io.StringIO()),
                self.assertRaisesRegex(RuntimeError, "kernel failure"),
            ):
                fixture_transfer.run_probe(
                    arguments,
                    subject="demo regression",
                    kernel_arguments=(),
                    probe_timeout_seconds=30,
                    command_timeout_seconds=60,
                    pass_marker=PASS_MARKER,
                )


if __name__ == "__main__":
    unittest.main()
