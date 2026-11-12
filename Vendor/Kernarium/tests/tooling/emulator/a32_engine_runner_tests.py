#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the AArch32 engine runner's verdict on fixed console and host
output: a kernel line that reaches the host inside a probe line splits no
line and reports no crash, and every real crash, unexpected SIGILL and unclean
end still fails the boot."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import pathlib
import unittest

RUNNER = (pathlib.Path(__file__).resolve().parents[2]
          / "integration/aarch32/run_guest_a32_engine.py")
_spec = importlib.util.spec_from_file_location("run_guest_a32_engine", RUNNER)
runner = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(runner)

SWP = '[    1.822599] "init" (130) uses obsolete SWP{B} instruction at 0x2bd64'
SUMMARY = ("a32-engine-probe: 0 failure(s) in 3 check(s), "
           "sigill expected 1 unexpected 0")
CLEAN = "\n".join((
    "[    0.309539] Run /init as init process",
    "PASS swp/swp with condition 0xf: raises SIGILL",
    "PASS swp/swp with condition 0xf: si_code is ILL_ILLOPC",
    SWP,
    "PASS swp/abi.swp accepts 1",
    SUMMARY,
    "[    9.026908] reboot: Power down",
))
HOST = "\n".join((
    "UML: enabling 4 virtual CPUs",
    "guest powered off",
    "Darwin UML stopped: result=0 exitcode=0",
))


def split(console: str, line: str, at: str, inserted: str) -> str:
    """`console` with `inserted` and a line end placed inside `line`, just
    before `at`, as the host receives a kernel line that overtook the rest of
    a probe line's write."""
    head, tail = line.split(at, 1)
    return console.replace(line, f"{head}{inserted}\n{at}{tail}")


def judge(console: str, host: str = HOST, status: int | None = 0) -> int:
    with contextlib.redirect_stdout(io.StringIO()):
        return runner.check_boot("4 KiB layout", console, host, status)


class VerdictTests(unittest.TestCase):
    def test_a_clean_boot_passes(self) -> None:
        self.assertEqual(judge(CLEAN), 0)

    def test_a_kernel_line_inside_a_probe_line_is_no_crash(self) -> None:
        # The CI failure: the line's tail, "aises SIGILL", reached the host
        # on a line of its own after the kernel's message.
        line = "PASS swp/swp with condition 0xf: raises SIGILL"
        console = split(CLEAN, line, "aises SIGILL", SWP)
        self.assertIn("\naises SIGILL\n", console)
        self.assertEqual(judge(console), 0)
        _, lines = runner.console_parts(console)
        self.assertIn(line, lines)

    def test_a_kernel_line_before_a_line_end_is_no_crash(self) -> None:
        line = "PASS swp/swp with condition 0xf: raises SIGILL"
        console = CLEAN.replace(line, f"{line}{SWP}\n")
        self.assertEqual(judge(console), 0)

    def test_a_kernel_line_inside_the_summary_keeps_its_counts(self) -> None:
        console = split(CLEAN, SUMMARY, "unexpected 0", SWP)
        self.assertEqual(judge(console), 0)
        unexpected = SUMMARY.replace("unexpected 0", "unexpected 2")
        console = split(CLEAN.replace(SUMMARY, unexpected), unexpected,
                        "unexpected 2", SWP)
        self.assertEqual(judge(console), 2)

    def test_an_unexpected_sigill_fails(self) -> None:
        console = CLEAN.replace("unexpected 0", "unexpected 1")
        self.assertEqual(judge(console), 1)

    def test_a_host_fault_fails(self) -> None:
        for host in (
            HOST + "\nDarwin host fault: signal=4 esr=0x2000000 far=0x0 "
            "pc=0x1000 sp=0x2000 lr=0x3000",
            HOST + "\nDarwin host fault: taken on a host thread which owns no "
            "virtual CPU; the guest is not the faulting agent",
        ):
            with self.subTest(host=host.splitlines()[-1]):
                with self.assertRaisesRegex(RuntimeError, "crashed"):
                    judge(CLEAN, host)

    def test_a_host_fault_on_the_console_fails(self) -> None:
        console = CLEAN + "\nDarwin host fault: signal=4 address=0x1000"
        with self.assertRaisesRegex(RuntimeError, "crashed"):
            judge(console)

    def test_a_kernel_panic_inside_a_probe_line_fails(self) -> None:
        panic = ("[    5.000000] Kernel panic - not syncing: Attempted to kill "
                 "init! exitcode=0x00000004")
        line = "PASS swp/abi.swp accepts 1"
        for console in (split(CLEAN, line, "accepts 1", panic),
                        CLEAN.replace(line, f"{line}{panic}\n")):
            with self.subTest(console=console):
                with self.assertRaisesRegex(RuntimeError, "crashed"):
                    judge(console)

    def test_an_illegal_instruction_on_the_console_fails(self) -> None:
        console = CLEAN.replace(SUMMARY, "Illegal instruction\n" + SUMMARY)
        with self.assertRaisesRegex(RuntimeError, "crashed"):
            judge(console)

    def test_an_emulator_that_ends_badly_fails(self) -> None:
        # A host SIGILL whose report the console never carried still fails.
        for status in (-4, 1, None):
            with self.subTest(status=status):
                with self.assertRaisesRegex(RuntimeError, "power off cleanly"):
                    judge(CLEAN, status=status)

    def test_a_boot_without_a_summary_fails(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "no summary"):
            judge(CLEAN.replace(SUMMARY + "\n", ""))

    def test_a_boot_that_ran_no_checks_fails(self) -> None:
        console = CLEAN.replace("in 3 check(s)", "in 0 check(s)")
        with self.assertRaisesRegex(RuntimeError, "no checks"):
            judge(console)

    def test_a_disabled_host_fast_path_fails(self) -> None:
        host = (HOST + "\numl: the carrier's thread-specific data is not where "
                "expected; reading the CPU number through pthread")
        with self.assertRaisesRegex(RuntimeError, "fast path"):
            judge(CLEAN, host)


if __name__ == "__main__":
    unittest.main()
