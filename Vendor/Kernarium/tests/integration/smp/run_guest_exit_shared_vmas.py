#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that a process with many dirty shared VMAs exits in linear time.

The guest runs exit_shared_vmas, whose child maps 20000 one-page shared
anonymous regions, dirties them and exits while its parent times the exit.
Every dirty shared page makes the mm teardown flush the TLB before it drops
the page-table lock, and a flush that walks every VMA on the host turns the
exit into work that grows with the square of the VMAs: about 47 s at 20000
once, a few tens of milliseconds when each flush is free. The probe fails an
exit that takes longer than its limit.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import sys


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "support"))
from guest import Guest, GuestFile  # noqa: E402


GUEST_PROBE = "/usr/local/libexec/uml-test/exit_shared_vmas"
RESULT_LINE = re.compile(r"^RESULT exit-shared-vmas (PASS|FAIL)\b(.*)$")
# The probe's own limit is far below this; a guest that runs the quadratic
# exit to its end still reports instead of timing the runner out.
PROBE_TIMEOUT_SECONDS = 300


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--vmas", type=int, default=20000)
    parser.add_argument("--limit-ms", type=int, default=10000)
    arguments = parser.parse_args()

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (f"ncpus={arguments.cpus}",),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={
            GUEST_PROBE: GuestFile(arguments.probe.read_bytes(), 0o755)
        },
    ) as guest:
        guest.boot_and_login()
        result = guest.run_command(
            f"{GUEST_PROBE} {arguments.vmas} {arguments.limit_ms}",
            PROBE_TIMEOUT_SECONDS,
        )
        print(result.output.strip(), flush=True)
        guest.check_command("echo alive", 30)

    lines = [
        match
        for match in map(RESULT_LINE.match, map(str.strip, result.output.splitlines()))
        if match
    ]
    if len(lines) != 1:
        raise RuntimeError(f"the probe printed {len(lines)} results")
    if lines[0].group(1) != "PASS" or result.return_code != 0:
        raise RuntimeError(
            f"exit of {arguments.vmas} shared VMAs: {lines[0].group(1)}"
            f"{lines[0].group(2)} (probe status {result.return_code})"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
