#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure cached gadget-block link throughput in a standalone guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import functools
import os
import pathlib
import shlex
import sys


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import CommandResult, Guest, GuestFile  # noqa: E402
from guest_bench import (  # noqa: E402
    add_guest_arguments,
    boot,
    halt,
    measure,
    print_median,
    topology,
)


GUEST_LINK_LOOP = "/usr/local/libexec/uml-test/guest-link-loop"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    add_guest_arguments(parser, cpus=4, memory="1024M")
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    parser.add_argument(
        "--workers",
        type=int,
        help="concurrent probe processes (default: --cpus)",
    )
    parser.add_argument(
        "--probe-argument",
        action="append",
        default=[],
        help=(
            "argument passed to every probe process (repeatable); the "
            "guest_tagged_*_loop.S probes run their untagged control when "
            "given one"
        ),
    )
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--boot-timeout", type=int, default=180)
    parser.add_argument(
        "--command-timeout",
        type=int,
        default=30,
        help="seconds allowed for each measured guest repetition",
    )
    parser.add_argument(
        "--guest-top-byte-ignore",
        action="store_true",
        help="run the guest with the top address byte ignored, which is the "
        "default and how Android runs",
    )
    parser.add_argument(
        "--guest-no-top-byte-ignore",
        action="store_true",
        help="run the guest with the top address byte not ignored, the "
        "control of a tagged-pointer probe",
    )
    boot_mode = parser.add_mutually_exclusive_group()
    boot_mode.add_argument(
        "--shell-init",
        action="store_true",
        help="start a bare shell on the image's tmpfs overlay",
    )
    boot_mode.add_argument(
        "--overlay-shell",
        dest="overlay_shell",
        action="store_true",
        help=(
            "start a shell with runtime mounts and the user network on the "
            "image's tmpfs overlay (default)"
        ),
    )
    arguments = parser.parse_args()
    if arguments.workers is None:
        arguments.workers = arguments.cpus
    return arguments


def workload_command(workers: int, probe_arguments: list[str]) -> str:
    probe = shlex.join([GUEST_LINK_LOOP, *probe_arguments])
    return (
        "rm -f /tmp/link-loop-output.*; "
        ": >/tmp/link-loop-pids; "
        "i=0; "
        f"while [ \"$i\" -lt {workers} ]; do "
        f"{probe} >\"/tmp/link-loop-output.$i\" 2>&1 & "
        "echo \"$!\" >>/tmp/link-loop-pids; "
        "i=$((i + 1)); "
        "done; "
        "status=0; "
        "for pid in $(cat /tmp/link-loop-pids); do "
        "wait \"$pid\" || status=1; "
        "done; "
        "cat /tmp/link-loop-output.*; "
        "rm -f /tmp/link-loop-output.* /tmp/link-loop-pids; "
        "test \"$status\" -eq 0"
    )


def check_probe_output(
    workers: int, repetition: int, result: CommandResult
) -> None:
    """Print each probe report; require one OK line from every worker."""
    output_lines = result.output.splitlines()
    for line in output_lines:
        if line.startswith("UML_PERF_PROBE "):
            print(f"guest run {repetition}: {line}", flush=True)
    if [line for line in output_lines if line == "OK"] != ["OK"] * workers:
        raise RuntimeError(
            f"repetition {repetition} returned "
            f"unexpected output {result.output!r}"
        )


def main() -> int:
    arguments = parse_arguments()
    host_cpus = min(os.cpu_count() or 1, 64)
    if not 1 <= arguments.cpus <= host_cpus:
        raise ValueError("--cpus must be between 1 and the host CPU count")
    if not 1 <= arguments.workers <= arguments.cpus:
        raise ValueError("--workers must be between 1 and --cpus")
    if arguments.repetitions < 1:
        raise ValueError("--repetitions must be positive")
    if arguments.command_timeout < 1:
        raise ValueError("--command-timeout must be positive")

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        (
            f"ncpus={arguments.cpus}",
            *(("--guest-top-byte-ignore",)
              if arguments.guest_top_byte_ignore else ()),
            *(("--guest-no-top-byte-ignore",)
              if arguments.guest_no_top_byte_ignore else ()),
        ),
        init_shell=arguments.shell_init,
        overlay_shell=not arguments.shell_init,
        engine=arguments.engine,
        core_class=arguments.core_class,
        payloads={
            GUEST_LINK_LOOP: GuestFile(
                arguments.binary.resolve().read_bytes(), 0o755
            )
        },
    ) as guest:
        boot(guest, arguments.shell_init, arguments.boot_timeout)
        elapsed, utilization = measure(
            guest,
            workload_command(arguments.workers, arguments.probe_argument),
            arguments.repetitions,
            arguments.command_timeout,
            check=functools.partial(check_probe_output, arguments.workers),
        )
        halt(guest)

    print_median(
        elapsed,
        utilization,
        " " + topology(arguments, workers=arguments.workers),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
