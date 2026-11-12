#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run shell commands in a guest without starting systemd.

Binary analysis, instruction sanity checks and one-off inspection do not need
a full systemd userspace - they need a root shell and the rootfs. Booting
systemd for them costs about ninety seconds of service startup, and the
benchmark runners' quiescence phase then has to stop most of it again.

This boots the per-run ramdisk's harness session straight to a shell instead,
runs each command, and prints the output.

    tools/guest-shell.py --emulator .build/presets/macos-integration/bin/uml-darwin \\
        -c 'objdump -d /opt/geekbench/... | head'

The runner maps the EROFS image directly; writable state remains in its tmpfs
OverlayFS upper layer.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys


TESTS = pathlib.Path(__file__).resolve().parents[1] / "tests"
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import CORE_CLASSES, Guest  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="run shell commands in a guest without systemd",
    )
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument(
        "--rootfs", type=pathlib.Path, default=pathlib.Path("rootfs.erofs")
    )
    parser.add_argument(
        "-c",
        "--command",
        action="append",
        required=True,
        metavar="COMMAND",
        help="shell command to run; repeatable, executed in order",
    )
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument(
        "--audio",
        action="store_true",
        help="attach the macOS Virtio Sound playback provider",
    )
    parser.add_argument(
        "--engine", choices=("jit", "gadgets"), default="gadgets"
    )
    parser.add_argument("--core-class", choices=CORE_CLASSES, default="performance")
    parser.add_argument("--boot-timeout", type=int, default=300)
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="suppress the live console stream and print only command output",
    )
    parser.add_argument("--timeout", type=int, default=1800)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if arguments.cpus < 1 or arguments.cpus > 64:
        raise ValueError("--cpus must be between 1 and 64")
    if arguments.timeout < 1 or arguments.boot_timeout < 1:
        raise ValueError("timeouts must be positive")

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        (
            f"ncpus={arguments.cpus}",
            *(("--audio",) if arguments.audio else ()),
        ),
        overlay_shell=True,
        engine=arguments.engine,
        core_class=arguments.core_class,
        # run_command returns only at its end marker, so without the live
        # console a long command such as a full benchmark would print nothing
        # for its whole run, and progress would look like a wedge.
        console_sink=None if arguments.quiet else sys.stdout.buffer,
    )
    status = 0
    try:
        guest.boot_and_login(arguments.boot_timeout)
        for command in arguments.command:
            result = guest.run_command(command, arguments.timeout)
            if arguments.quiet:
                print(result.output, flush=True)
            if result.return_code != 0:
                print(
                    f"command exited {result.return_code}: {command}",
                    file=sys.stderr,
                    flush=True,
                )
                status = result.return_code
        shutdown_ok = guest.shutdown(60)
        if not shutdown_ok:
            print(
                "guest did not halt cleanly within 60 seconds",
                file=sys.stderr,
                flush=True,
            )
            if status == 0:
                status = 1
        else:
            emulator_status = guest.return_code
            if emulator_status != 0:
                print(
                    f"emulator exited with status {emulator_status} "
                    "during shutdown",
                    file=sys.stderr,
                    flush=True,
                )
                if status == 0:
                    status = 1
    finally:
        guest.close()
    return status


if __name__ == "__main__":
    raise SystemExit(main())
