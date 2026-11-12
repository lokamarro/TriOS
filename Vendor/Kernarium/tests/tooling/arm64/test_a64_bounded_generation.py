#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the bounded generator runner's process-group rules.

run_bounded() runs each generator command in its own session and fails the
command when its leader exits while descendants remain. Darwin keeps
reporting the group of a leader that has just been reaped for a few
milliseconds, which must not count as a surviving descendant.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import a64_bounded_generation as bounded  # noqa: E402

MIB = 1024 * 1024
LIMITS = bounded.GenerationLimits(
    free_space_reserve_bytes=MIB,
    max_workspace_bytes=64 * MIB,
    max_artifact_bytes=16 * MIB,
    max_log_bytes=MIB,
    max_workspace_files=1000,
)


class BoundedCommandTest(unittest.TestCase):
    def setUp(self) -> None:
        self.source = tempfile.TemporaryDirectory()
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.source.cleanup)
        self.addCleanup(self.work.cleanup)
        # The temporary root is reached through a symlink on macOS.
        self.work_path = Path(self.work.name).resolve()
        self.workspace = bounded.BoundedWorkspace(
            source_root=Path(self.source.name).resolve(),
            publication_targets=[],
            work_root=self.work_path,
            limits=LIMITS,
            prefix="bounded-test-",
        )

    def run_command(self, script: str):
        with self.workspace:
            return bounded.run_bounded(
                ["/bin/sh", "-c", script],
                workspace=self.workspace,
                timeout_seconds=30,
                check=True,
            )

    def test_a_clean_exit_returns_its_output(self) -> None:
        self.assertEqual(self.run_command("echo ready").stdout, "ready\n")

    def test_a_surviving_descendant_is_a_violation(self) -> None:
        marker = self.work_path / "descendant.pid"
        with self.assertRaisesRegex(
            bounded.GenerationLimitError, "descendants remained"
        ):
            self.run_command(
                f"sleep 60 >/dev/null 2>&1 & echo $! > {marker}; exit 0"
            )
        pid = int(marker.read_text())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_the_teardown_window_of_the_group_is_not_a_violation(self) -> None:
        reports = iter([True, True, True])
        with mock.patch.object(
            bounded, "_group_exists", side_effect=lambda _group: next(reports, False)
        ):
            self.assertEqual(self.run_command("exit 0").returncode, 0)


if __name__ == "__main__":
    unittest.main()
