#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin how the guest-restart runner compares the process at two boots.

tests/integration/restart/run_guest_restart.py asks a process audit inside the
launcher for the process's descriptors, threads and Mach ports at every boot
and fails on what differs from the first boot. A thread's reply port is the
one right whose count follows how far each thread has got, so it may differ by
no more than the process has threads, and a decrease is never a leak. This test
feeds the comparison dumps in the audit's format and no guest starts.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
import pathlib
import sys
import unittest

RUNNER = pathlib.Path(__file__).resolve().parents[2] / "integration/restart"
SUPPORT = RUNNER.parent / "support"
sys.path.insert(0, str(SUPPORT))
SPEC = importlib.util.spec_from_file_location(
    "run_guest_restart", RUNNER / "run_guest_restart.py"
)
assert SPEC is not None and SPEC.loader is not None
run_guest_restart = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_guest_restart)

REPLY = run_guest_restart.REPLY_PORTS


def dump(*, threads: int = 9, reply: int = 10, send: int = 10,
         extra: tuple[str, ...] = ()) -> dict[str, list[str]]:
    """An audit dump with the lines the comparison reads."""
    lines = [
        f"threads {threads}",
        f"thread {threads} (unnamed)",
        f"ports {reply + send}",
        f"port {reply} {REPLY}",
        f"port {send} rights=0x10000 kobject=1",
        *extra,
    ]
    return run_guest_restart.parse_dump("\n".join(lines) + "\nend\n")


class AuditComparisonTests(unittest.TestCase):
    def differences(self, first, other):
        return run_guest_restart.differences(first, other)

    def test_a_process_that_looks_the_same_differs_in_nothing(self):
        self.assertEqual(self.differences(dump(), dump()), [])

    def test_fewer_reply_ports_with_the_same_threads_are_no_difference(self):
        # A loaded host showed 11 of them at the first boot and 8 at the
        # third, with the same threads: some had called the kernel by then and
        # some had not, and nothing was left behind.
        self.assertEqual(self.differences(dump(reply=11), dump(reply=8)), [])

    def test_more_reply_ports_are_no_difference_up_to_the_threads(self):
        self.assertEqual(self.differences(dump(reply=10), dump(reply=19)), [])

    def test_reply_ports_beyond_the_threads_are_a_difference(self):
        found = self.differences(dump(reply=10), dump(reply=20))
        self.assertEqual(found, [f"port {REPLY}: 10 at the first boot, 20 now"])

    def test_the_bound_is_what_the_process_has_not_a_constant(self):
        few = self.differences(dump(threads=2, reply=10), dump(threads=2, reply=13))
        self.assertEqual(few, [f"port {REPLY}: 10 at the first boot, 13 now"])
        many = self.differences(dump(threads=30, reply=10), dump(threads=30, reply=40))
        self.assertEqual(many, [])

    def test_every_other_kind_of_port_must_be_the_same(self):
        found = self.differences(dump(send=10), dump(send=11))
        self.assertEqual(
            found, ["port rights=0x10000 kobject=1: 10 at the first boot, 11 now"])

    def test_other_threads_are_still_a_difference(self):
        found = self.differences(dump(threads=9), dump(threads=10))
        self.assertTrue(any(line.startswith("thread ") for line in found), found)

    def test_a_dump_without_threads_allows_no_reply_port_difference(self):
        first = run_guest_restart.parse_dump(f"port 10 {REPLY}\nend\n")
        other = run_guest_restart.parse_dump(f"port 9 {REPLY}\nend\n")
        self.assertEqual(
            self.differences(first, other),
            [f"port {REPLY}: 10 at the first boot, 9 now"])


if __name__ == "__main__":
    unittest.main()
