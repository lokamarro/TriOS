#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure cold translation and exec throughput with repeated /bin/true."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import sys


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import Guest  # noqa: E402
from guest_bench import (  # noqa: E402
    add_guest_arguments,
    halt,
    measure,
    print_median,
    topology,
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    add_guest_arguments(parser, cpus=4, memory=None, engine="gadgets")
    parser.add_argument(
        "--workers",
        type=int,
        help="concurrent exec loops (default: --cpus)",
    )
    parser.add_argument("--executions", type=int, default=5000)
    parser.add_argument("--repetitions", type=int, default=5)
    arguments = parser.parse_args()
    if arguments.workers is None:
        arguments.workers = arguments.cpus
    return arguments


def workload_command(workers: int, executions: int) -> str:
    count = executions // workers
    return (
        "rm -f /tmp/exec-bench-pids; : >/tmp/exec-bench-pids; "
        "worker=0; "
        f"while [ \"$worker\" -lt {workers} ]; do "
        "(i=0; "
        f"while [ \"$i\" -lt {count} ]; do /bin/true || exit; "
        "i=$((i + 1)); done) & "
        "echo \"$!\" >>/tmp/exec-bench-pids; "
        "worker=$((worker + 1)); "
        "done; "
        "status=0; "
        "for pid in $(cat /tmp/exec-bench-pids); do "
        "wait \"$pid\" || status=1; done; "
        "rm -f /tmp/exec-bench-pids; "
        "test \"$status\" -eq 0"
    )


def main() -> int:
    arguments = parse_arguments()
    host_cpus = min(os.cpu_count() or 1, 64)
    if not 1 <= arguments.cpus <= host_cpus:
        raise ValueError("--cpus must be between 1 and the host CPU count")
    if not 1 <= arguments.workers <= arguments.cpus:
        raise ValueError("--workers must be between 1 and --cpus")
    if arguments.executions < arguments.workers or (
        arguments.executions % arguments.workers
    ):
        raise ValueError("--executions must be a positive multiple of --workers")
    if arguments.repetitions < 1:
        raise ValueError("--repetitions must be positive")

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (f"ncpus={arguments.cpus}",),
        overlay_shell=True,
        engine=arguments.engine,
        core_class=arguments.core_class,
    )
    try:
        guest.boot_and_login()
        elapsed, utilization = measure(
            guest,
            workload_command(arguments.workers, arguments.executions),
            arguments.repetitions,
            30,
        )
        halt(guest)
    finally:
        guest.close()

    print_median(
        elapsed,
        utilization,
        " " + topology(arguments, workers=arguments.workers),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
