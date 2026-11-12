#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a matched command repeatedly in an EROFS overlay-root guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import functools
import os
import pathlib
import subprocess
import sys


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import Guest, GuestFile, validate_payload_path  # noqa: E402
from guest_bench import (  # noqa: E402
    add_guest_arguments,
    boot,
    halt,
    measure,
    print_median,
    topology,
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    add_guest_arguments(parser, cpus=4, memory="1024M")
    parser.add_argument("--command", required=True)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument(
        "--sample-output",
        type=pathlib.Path,
        help="capture a macOS sample profile during each command",
    )
    parser.add_argument("--sample-seconds", type=int, default=5)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument(
        "--upload",
        action="append",
        default=[],
        metavar="HOST_PATH:GUEST_PATH",
        help=(
            "install an executable host file at GUEST_PATH, outside /tmp "
            "and /run, before the guest starts"
        ),
    )
    parser.add_argument(
        "--shell-init",
        action="store_true",
        help=(
            "start a bare shell on the image's tmpfs overlay instead of "
            "the default shell with runtime mounts and the user network"
        ),
    )
    return parser.parse_args()


@contextlib.contextmanager
def sample_profile(
    arguments: argparse.Namespace, process_id: int, repetition: int
):
    """Capture a macOS sample profile of the emulator around one command."""
    path = arguments.sample_output
    if arguments.repetitions != 1:
        path = path.with_name(f"{path.stem}-{repetition}{path.suffix}")
    path.parent.mkdir(parents=True, exist_ok=True)
    sampler = subprocess.Popen(
        (
            "/usr/bin/sample",
            str(process_id),
            str(arguments.sample_seconds),
            "-file",
            str(path.resolve()),
        ),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    yield
    try:
        sampler.wait(timeout=arguments.sample_seconds + 5)
    except subprocess.TimeoutExpired:
        sampler.terminate()
        sampler.wait()


def main() -> int:
    arguments = parse_arguments()
    if not 1 <= arguments.cpus <= min(os.cpu_count() or 1, 64):
        raise ValueError("--cpus must be between 1 and the host CPU count")
    if arguments.repetitions < 1:
        raise ValueError("--repetitions must be positive")
    if arguments.sample_seconds < 1 or arguments.timeout < 1:
        raise ValueError("sample duration and timeout must be positive")

    payloads = {}
    for upload in arguments.upload:
        try:
            host_text, guest_path = upload.split(":", 1)
        except ValueError as error:
            raise ValueError("--upload must be HOST_PATH:GUEST_PATH") from error
        host_path = pathlib.Path(host_text).resolve()
        if not host_path.is_file():
            raise ValueError(f"upload source is not a file: {host_path}")
        validate_payload_path(guest_path)
        if guest_path in payloads:
            raise ValueError(f"--upload names {guest_path} twice")
        payloads[guest_path] = GuestFile(host_path.read_bytes(), 0o755)

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        (f"ncpus={arguments.cpus}",),
        init_shell=arguments.shell_init,
        overlay_shell=not arguments.shell_init,
        engine=arguments.engine,
        core_class=arguments.core_class,
        payloads=payloads,
    ) as guest:
        boot(guest, arguments.shell_init)
        sampler = None
        if arguments.sample_output is not None:
            sampler = functools.partial(
                sample_profile, arguments, guest.process_id
            )
        elapsed, utilization = measure(
            guest,
            arguments.command,
            arguments.repetitions,
            arguments.timeout,
            around=sampler,
            check=lambda _repetition, result: print(result.output, flush=True),
        )
        halt(guest)

    print_median(elapsed, utilization, " " + topology(arguments))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
