#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the guest Geekbench script against stand-in programs and payloads."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import os
import pathlib
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "perf/guest/geekbench.sh"
GEEKBENCH_6 = "opt/geekbench/release/geekbench6"
GEEKBENCH_5 = "opt/geekbench/release/geekbench_armv7"
GEEKBENCH_5_AARCH64 = (
    "opt/geekbench/release/geekbench_aarch64")
# Prints the arguments it was run with, then exits with the status it is given
# in $STATUS, so the test sees exactly what the script asked Geekbench to do.
# The guest's /bin/sh is dash, so a host that has it runs both and compares.
SHELLS = [shell for shell in ("/bin/sh", "/bin/dash") if os.path.exists(shell)]
PROGRAM = '#!/bin/sh\necho "ran $0 $*"\nexit "${STATUS:-0}"\n'
# Stands in for mount(8): puts a Geekbench 5 payload where the script mounts.
MOUNT = (
    '#!/bin/sh\necho "mount $*" >&2\n'
    'for last; do :; done\n'
    'printf \'#!/bin/sh\\necho "ran $0 $*"\\n\' > "$last/geekbench_armv7"\n'
    'chmod 755 "$last/geekbench_armv7"\n'
)


class GeekbenchScriptTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="uml-geekbench-sh-")
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name).resolve()
        # The script names absolute guest paths; point them into a sandbox.
        text = SCRIPT.read_text(encoding="utf-8")
        for guest in ("/etc/uml-geekbench.env", "/opt/geekbench-payload",
                      "/opt/geekbench/"):
            self.assertIn(guest, text)
            text = text.replace(guest, f"{self.root}{guest}")
        self.script = self.root / "geekbench.sh"
        self.script.write_text(text, encoding="utf-8")
        self.bin = self.root / "bin"
        self.bin.mkdir()

    def program(self, relative: str, contents: str = PROGRAM) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
        path.chmod(0o755)

    def parameters(self, text: str) -> None:
        path = self.root / "etc/uml-geekbench.env"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def run_script(self, status: int = 0):
        results = [
            subprocess.run(
                [shell, str(self.script)],
                env={"PATH": f"{self.bin}:/usr/bin:/bin",
                     "STATUS": str(status)},
                cwd=self.root, capture_output=True, text=True, check=False,
            )
            for shell in SHELLS
        ]
        for result in results[1:]:
            self.assertEqual(
                (result.returncode, result.stdout, result.stderr),
                (results[0].returncode, results[0].stdout,
                 results[0].stderr))
        return results[0]

    def lines(self, result) -> list[str]:
        return [
            line.replace(str(self.root), "")
            for line in result.stdout.splitlines() if line
        ]

    def test_the_image_payload_of_geekbench_6_runs_as_it_always_has(self) -> None:
        self.program(GEEKBENCH_6)
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.lines(result), [
            "UML_GEEKBENCH_READY",
            f"UML_GEEKBENCH_PROGRAM:/{GEEKBENCH_6}",
            f"ran /{GEEKBENCH_6} --no-upload --iterations 1",
            "UML_GEEKBENCH_END:0",
        ])

    def test_targeted_workloads_run_single_core_section_one(self) -> None:
        self.program(GEEKBENCH_6)
        self.parameters("# comment\n\nUML_GEEKBENCH_WORKLOADS=102,201\n")
        result = self.run_script()
        self.assertEqual(self.lines(result)[2:], [
            "UML_GEEKBENCH_TARGETS:102,201",
            f"ran /{GEEKBENCH_6} --no-upload --iterations 1 --single-core "
            "--section 1 "
            "--workload 102,201",
            "UML_GEEKBENCH_END:0",
        ])

    def test_geekbench_5_armv7_runs_with_the_same_arguments(self) -> None:
        self.program(GEEKBENCH_5)
        result = self.run_script()
        self.assertEqual(self.lines(result), [
            "UML_GEEKBENCH_READY",
            f"UML_GEEKBENCH_PROGRAM:/{GEEKBENCH_5}",
            f"ran /{GEEKBENCH_5} --no-upload --iterations 1",
            "UML_GEEKBENCH_END:0",
        ])
        self.parameters("UML_GEEKBENCH_WORKLOADS=101\n")
        self.assertIn(
            f"ran /{GEEKBENCH_5} --no-upload --iterations 1 --single-core "
            "--section 1 "
            "--workload 101",
            self.lines(self.run_script()))

    def test_geekbench_5_aarch64_is_found_by_name(self) -> None:
        self.program(GEEKBENCH_5_AARCH64)
        self.assertEqual(self.lines(self.run_script()), [
            "UML_GEEKBENCH_READY",
            f"UML_GEEKBENCH_PROGRAM:/{GEEKBENCH_5_AARCH64}",
            f"ran /{GEEKBENCH_5_AARCH64} --no-upload --iterations 1",
            "UML_GEEKBENCH_END:0",
        ])

    def test_geekbench_6_wins_when_an_image_carries_both(self) -> None:
        self.program(GEEKBENCH_5)
        self.program(GEEKBENCH_6)
        self.assertIn(f"UML_GEEKBENCH_PROGRAM:/{GEEKBENCH_6}",
                      self.lines(self.run_script()))

    def test_the_benchmarks_status_ends_the_run(self) -> None:
        self.program(GEEKBENCH_5)
        result = self.run_script(status=7)
        self.assertEqual(result.returncode, 7)
        self.assertEqual(self.lines(result)[-1], "UML_GEEKBENCH_END:7")

    def test_an_image_without_a_payload_or_a_bad_parameter_is_an_error(self) -> None:
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.lines(result), [
            "UML_GEEKBENCH_ERROR:the image has no Geekbench payload"])
        self.program(GEEKBENCH_5)
        for text, message in (
            ("UML_GEEKBENCH_WORKLOADS=1;2\n", "malformed workload list: 1;2"),
            ("UML_GEEKBENCH_WORKLOADS=,1\n", "malformed workload list: ,1"),
            ("UNKNOWN=1\n", "unknown"),
        ):
            self.parameters(text)
            result = self.run_script()
            self.assertEqual(result.returncode, 1)
            self.assertIn(f"UML_GEEKBENCH_ERROR:{message}", result.stdout)

    def test_a_payload_disk_is_mounted_read_only_and_its_program_runs(self) -> None:
        mount = self.bin / "mount"
        mount.write_text(MOUNT)
        mount.chmod(0o755)
        self.parameters("UML_GEEKBENCH_PAYLOAD=/dev/ubdb\n")
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("mount -t erofs -o ro /dev/ubdb", result.stderr)
        self.assertEqual(self.lines(result)[:2], [
            "UML_GEEKBENCH_READY",
            "UML_GEEKBENCH_PROGRAM:/opt/geekbench-payload/geekbench_armv7"])

    def test_a_payload_disk_that_will_not_mount_is_an_error(self) -> None:
        mount = self.bin / "mount"
        mount.write_text("#!/bin/sh\nexit 32\n")
        mount.chmod(0o755)
        self.parameters("UML_GEEKBENCH_PAYLOAD=/dev/ubdb\n")
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("UML_GEEKBENCH_ERROR:cannot mount the Geekbench payload "
                      "/dev/ubdb", result.stdout)


if __name__ == "__main__":
    unittest.main()
