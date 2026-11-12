#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Wait for the early splash to release DRM before starting Gamescope."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from pathlib import Path
import time

Path("/run/kernarium-stop-splash").touch()
pid_file = Path("/run/kernarium-splash.pid")
if pid_file.exists():
    pid = int(pid_file.read_text())
    command = Path(f"/proc/{pid}/cmdline")
    deadline = time.monotonic() + 10
    while command.exists():
        try:
            arguments = command.read_bytes().split(b"\0")
        except FileNotFoundError:
            break
        if b"/usr/local/libexec/kernarium-screen" not in arguments or b"--framebuffer" not in arguments:
            break
        if time.monotonic() >= deadline:
            raise SystemExit("Kernarium splash did not release the display within 10 seconds")
        time.sleep(0.05)
