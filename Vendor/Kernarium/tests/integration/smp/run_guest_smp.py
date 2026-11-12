#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify that Darwin UML runs guest work on multiple host cores."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import shlex
import sys
import time


SUPPORT = pathlib.Path(__file__).resolve().parents[1] / "support"
sys.path.insert(0, str(SUPPORT))
from guest import Guest  # noqa: E402
from host_metrics import cpu_time_seconds  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument(
        "--cpus",
        type=int,
        default=10,
        help="number of guest cores to start (default: 10)",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if not 1 <= arguments.cpus <= min(os.cpu_count() or 1, 64):
        raise ValueError("--cpus must be between 1 and the host CPU count")
    expected_cpus = arguments.cpus
    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (f"ncpus={expected_cpus}",),
        overlay_shell=True,
    )
    try:
        guest.boot_and_login()
        guest.check_command("nproc", expected=str(expected_cpus))
        guest.check_command(
            "cat /sys/devices/system/cpu/online",
            expected="0" if expected_cpus == 1 else f"0-{expected_cpus - 1}",
        )
        guest.check_command(
            "for cpu in /sys/devices/system/cpu/cpu[0-9]*; do "
            "id=${cpu##*cpu}; "
            "test \"$(cat \"$cpu/topology/physical_package_id\")\" = 0 || exit 1; "
            "test \"$(cat \"$cpu/topology/core_id\")\" = \"$id\" || exit 1; "
            "test \"$(cat \"$cpu/topology/thread_siblings_list\")\" = \"$id\" "
            "|| exit 1; "
            "done; echo TOPOLOGY_OK",
            expected="TOPOLOGY_OK",
        )
        guest.check_command(
            ": >/tmp/uml-smp-pids; i=0; "
            f"while [ \"$i\" -lt {expected_cpus} ]; do "
            "sh -c 'while :; do :; done' & "
            "echo \"$!\" >>/tmp/uml-smp-pids; "
            "i=$((i + 1)); "
            "done",
        )
        before = cpu_time_seconds(guest.process_id)
        started = time.monotonic()
        time.sleep(4)
        elapsed = time.monotonic() - started
        consumed = cpu_time_seconds(guest.process_id) - before
        guest.check_command(
            "pids=$(cat /tmp/uml-smp-pids); kill $pids; "
            "wait $pids 2>/dev/null; rm /tmp/uml-smp-pids",
        )
        utilization = consumed / elapsed
        print(
            f"guest SMP: {expected_cpus} vCPUs, host CPU time "
            f"{consumed:.2f}s over "
            f"{elapsed:.2f}s wall ({utilization:.2f} cores)",
            flush=True,
        )
        minimum_utilization = (
            0.75 if expected_cpus == 1 else max(1.25, expected_cpus * 0.5)
        )
        if utilization < minimum_utilization:
            raise RuntimeError(
                f"{expected_cpus} runnable guest processes used only "
                f"{utilization:.2f} host cores; expected at least "
                f"{minimum_utilization:.2f}"
            )
        wakeup_payload = (
            pathlib.Path(__file__).with_name("guest_ipi_wakeup.py").read_text()
        )
        wakeup_command = shlex.quote(f"exec({wakeup_payload!r})")
        guest.check_command(
            f"python3 -c {wakeup_command} {expected_cpus}",
            expected=f"IPI_WAKEUP_OK {expected_cpus * 2000}\n"
            f"IPI_WAKEUP_OK {expected_cpus * 8000}",
        )
        if expected_cpus > 1:
            guest.check_command(
                "i=0; while [ $i -lt 5 ]; do "
                "echo 0 >/sys/devices/system/cpu/cpu1/online || exit 1; "
                "echo 1 >/sys/devices/system/cpu/cpu1/online || exit 1; "
                "i=$((i + 1)); done; echo HOTPLUG_OK",
                expected="HOTPLUG_OK",
            )
            guest.check_command(
                "cat /sys/devices/system/cpu/online",
                expected=f"0-{expected_cpus - 1}",
            )
        guest.check_command("nproc", expected=str(expected_cpus))
        return 0
    finally:
        guest.close()


if __name__ == "__main__":
    raise SystemExit(main())
