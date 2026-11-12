#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the pure x86-64 smoke rootfs on the 4 KiB x86-64 runtime."""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import os
import pathlib
import pty
import select
import signal
import subprocess
import sys
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
# python3 -I leaves the script's own directory off sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from guest import (  # noqa: E402
    emulator_arguments,
    remove_emulator_run_directories,
)
from probe_runner import parse_arguments  # noqa: E402


SUCCESS = b"x86_64 gadgets rootfs: syscall tls signal xstate ok"


def terminate(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        try:
            process.send_signal(signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
    remove_emulator_run_directories(process.pid)


def boot(
    emulator: pathlib.Path, rootfs: pathlib.Path, engine: str, timeout: int
) -> bytes:
    # The terminal end stays open until the console is read to its end, so
    # the emulator's exit is never its last close: Darwin discards a
    # terminal's unread output then, the success line among it.
    master, slave = pty.openpty()
    os.set_blocking(master, False)
    try:
        process = subprocess.Popen(
            emulator_arguments(
                emulator,
                rootfs,
                "512M",
                kernel_arguments=(
                    "ncpus=10",
                    "console=tty0",
                    "console=ttyS0",
                    "panic=1",
                ),
                init_path="/init",
                engine=engine,
            ),
            stdin=slave,
            stdout=slave,
            stderr=slave,
            close_fds=True,
        )
    except BaseException:
        os.close(master)
        os.close(slave)
        raise

    transcript = bytearray()
    deadline = time.monotonic() + timeout
    try:
        while process.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"x86-64 smoke guest did not stop within {timeout} seconds"
                )
            # The held terminal reports no end when the emulator exits.
            readable, _, _ = select.select(
                [master], [], [], min(remaining, 0.25)
            )
            if not readable:
                continue
            try:
                transcript.extend(os.read(master, 65536))
            except BlockingIOError:
                pass
            except OSError:
                if process.poll() is None:
                    raise
                break

        while True:
            try:
                chunk = os.read(master, 65536)
            except (BlockingIOError, OSError):
                break
            if not chunk:
                break
            transcript.extend(chunk)
        if process.returncode != 0:
            raise RuntimeError(
                f"x86-64 smoke guest exited with status {process.returncode}"
            )
        return bytes(transcript)
    except BaseException:
        terminate(process)
        if transcript:
            print(transcript.decode(errors="replace"), file=sys.stderr)
        raise
    finally:
        # The master first: a terminal end closed last with output unread
        # while its master is open waits 0.6 s for a reader.
        os.close(master)
        os.close(slave)


def main() -> int:
    arguments = parse_arguments(__doc__, compiler_help=None)

    try:
        transcript = boot(
            arguments.emulator, arguments.rootfs, arguments.engine, arguments.timeout
        )
        if SUCCESS not in transcript:
            raise RuntimeError("x86-64 smoke success marker is missing")
    except BaseException:
        if "transcript" in locals():
            print(transcript.decode(errors="replace"), file=sys.stderr)
        raise

    print(
        f"PASS engine={arguments.engine} userspace-slice=x86_64 "
        "kernel-mode=arm64 page-size=4k"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
