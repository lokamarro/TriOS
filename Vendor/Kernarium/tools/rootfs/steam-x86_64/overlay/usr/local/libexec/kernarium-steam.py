#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Show real Steam bootstrap progress inside Gamescope until Big Picture maps."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
from pathlib import Path
import re
import subprocess
import time


def progress(line: str) -> tuple[str, float | None] | None:
    downloaded = re.search(r"Downloading update \(([\d,]+) of ([\d,]+) KB\)", line, re.I)
    if downloaded:
        current, total = (int(value.replace(",", "")) for value in downloaded.groups())
        return (f"Downloading Steam • {current / 1024:.1f} of {total / 1024:.1f} MiB",
                current / total if total else None)
    for token, caption in (
        ("Checking for available updates", "Checking for Steam updates"),
        ("Verifying installation", "Verifying Steam installation"),
        ("Extracting package", "Installing Steam • extracting update"),
        ("Installing update", "Installing Steam update"),
        ("Cleaning up", "Finishing Steam installation"),
        ("Update complete", "Steam is ready • starting Big Picture"),
        ("Fatal", "Steam could not update. Check your connection and restart."),
    ):
        if token.lower() in line.lower():
            return caption, None
    return None


def big_picture_visible() -> bool:
    tree = subprocess.run(["xwininfo", "-root", "-tree"], capture_output=True, text=True, timeout=5)
    for line in tree.stdout.splitlines():
        if "Steam Big Picture Mode" not in line:
            continue
        window = re.search(r"0x[0-9a-f]+", line)
        if window:
            state = subprocess.run(["xwininfo", "-id", window[0]], capture_output=True, text=True, timeout=5)
            if "Map State: IsViewable" in state.stdout:
                return True
    return False


def stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main() -> int:
    steam = Path.home() / ".local/share/Steam"
    state = Path(os.environ["XDG_RUNTIME_DIR"]) / "kernarium-install.status"

    def display(message: str, fraction: float | None = None) -> None:
        staged = state.with_suffix(".new")
        staged.write_text(f"{fraction if fraction is not None else -1}\n{message.replace(chr(10), chr(32))}\n")
        staged.replace(state)

    options = ["-gamepadui"]
    if os.environ.get("KERNARIUM_CEF_VAAPI") == "1":
        # Valve's own client option is what turns on the Chromium features that
        # let its web helper decode video through VA-API in an OpenGL session.
        options.append("-cef-enable-vaapi")

    display("Starting Steam")
    screen = subprocess.Popen(["/usr/local/libexec/kernarium-screen", "--status", str(state)])
    client = None
    log = steam / "logs/bootstrap_log.txt"
    try:
        for attempt in range(4):
            offset = log.stat().st_size if log.exists() else 0
            updated = False
            client = subprocess.Popen(["/usr/local/bin/steam", *options])
            deadline = time.monotonic() + 1800
            visible = False
            while True:
                if not visible and time.monotonic() >= deadline:
                    display("Steam startup timed out. Check your connection and restart.")
                    stop(client)
                    return 1
                if log.exists():
                    if log.stat().st_size < offset:
                        offset = 0
                    with log.open(errors="replace") as source:
                        source.seek(offset)
                        for line in source:
                            updated |= "Update complete, launching Steam" in line
                            change = progress(line)
                            if change:
                                display(*change)
                        offset = source.tell()
                if client.poll() is not None:
                    break
                if not visible and big_picture_visible():
                    visible = True
                    stop(screen)
                time.sleep(0.5)
            # Steam's bootstrap updater exits after replacing itself. Restart
            # that completed update, but do not override a user's normal Quit.
            # Valve's steam.sh defines 42 as MAGIC_RESTART_EXITCODE. Gamescope
            # does not propagate that child status as its own service status.
            restart = client.returncode == 42 or (client.returncode == 0 and updated and not visible)
            if not restart:
                return client.returncode
            display("Steam updated • starting Big Picture")
            if screen.poll() is not None:
                screen = subprocess.Popen(["/usr/local/libexec/kernarium-screen", "--status", str(state)])
        display("Steam repeatedly requested a restart. Please restart the guest.")
        return 1
    finally:
        if client is not None:
            stop(client)
        stop(screen)


if __name__ == "__main__":
    raise SystemExit(main())
