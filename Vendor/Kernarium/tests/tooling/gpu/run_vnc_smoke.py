#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

"""Serve the deterministic VNC fixture and connect the smoke client to it.

Every wait here is for an event of a process this script started: the
server's announcement of its port, or the exit of one client. Each wait ends
at that event, or when the process it waits on exits, so a loaded host makes
the run slower and never wrong. The one clock is the run's bound,
``--wedge-timeout``, which only a process that never answers reaches.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
from pathlib import Path
import re
import select
import subprocess
import time


VNC_PORT = re.compile(rb"^VNC display: [^:]+:(\d+) ")

# Stopping the server is not part of the verdict. The first only chooses
# between waiting longer for its exit and killing it; the second bounds the
# wait for a killed process to be gone.
TERMINATE_GRACE = 10
KILL_BOUND = 20


def output_text(output: bytes | bytearray) -> str:
    return bytes(output).decode("utf-8", errors="replace")


def time_left(deadline: float, what: str) -> float:
    left = deadline - time.monotonic()
    if left <= 0:
        raise TimeoutError(f"{what}: the run's wedge bound was reached")
    return left


def wait_for_vnc_port(server: subprocess.Popen[bytes], deadline: float) -> int:
    """Return the port the server announces, as soon as it does.

    The wait is on the server's output, so it ends at the announcement or at
    the output's end, which is the server exiting. The deadline is reached
    only by a server that stays alive and says nothing.
    """

    assert server.stdout is not None
    output = bytearray()
    pending = bytearray()
    descriptor = server.stdout.fileno()
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError(
                "VNC server did not announce a dynamically bound port:\n" +
                output_text(output))
        readable, _, _ = select.select([descriptor], [], [], left)
        if not readable:
            continue
        chunk = os.read(descriptor, 4096)
        if not chunk:
            try:
                status = f"status {server.wait(timeout=KILL_BOUND)}"
            except subprocess.TimeoutExpired:
                status = "still running"
            raise RuntimeError(
                f"VNC server exited early ({status}):\n" +
                output_text(output))
        output.extend(chunk)
        pending.extend(chunk)
        while b"\n" in pending:
            line, _, remainder = pending.partition(b"\n")
            pending = bytearray(remainder)
            match = VNC_PORT.match(line)
            if match is not None:
                port = int(match.group(1))
                if port != 0:
                    return port


def run_client(
    client: Path,
    deadline: float,
    what: str,
    *arguments: str,
    **run_options: object,
) -> subprocess.CompletedProcess[str]:
    """Run the client until it exits, which is the event of interest."""

    try:
        return subprocess.run(
            [str(client), *arguments],
            timeout=time_left(deadline, what),
            **run_options)
    except subprocess.TimeoutExpired:
        raise TimeoutError(
            f"{what}: the client did not exit before the run's wedge "
            "bound") from None


def stop_server(server: subprocess.Popen[bytes]) -> None:
    if server.poll() is None:
        server.terminate()
        try:
            server.wait(timeout=TERMINATE_GRACE)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=KILL_BOUND)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument(
        "--wedge-timeout", type=float, default=60,
        help="seconds the whole run may take before it counts as wedged; "
        "reaching it means a process never answered, not that the host "
        "was slow")
    options = parser.parse_args()
    if options.wedge_timeout <= 0:
        parser.error("--wedge-timeout must be positive")
    deadline = time.monotonic() + options.wedge_timeout
    password = "umltest"

    server = subprocess.Popen(
        [
            str(options.server),
            "--vnc-port",
            "0",
            "--vnc-password",
            password,
            "--width",
            "64",
            "--height",
            "48",
            "--self-test-pattern",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    try:
        port = wait_for_vnc_port(server, deadline)

        rejected = run_client(
            options.client,
            deadline,
            "wrong-password client",
            "--password",
            "wrong",
            str(port),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if rejected.returncode == 0:
            raise RuntimeError("VNC server accepted an incorrect password")

        run_client(
            options.client,
            deadline,
            "verifying client",
            "--password",
            password,
            str(port),
            check=True,
        )
        run_client(
            options.client,
            deadline,
            "input client",
            "--password",
            password,
            "--input",
            str(port),
            check=True,
        )
    finally:
        stop_server(server)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
