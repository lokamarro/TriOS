# SPDX-License-Identifier: GPL-2.0-or-later
"""The guest launcher both Chromium WebGL runners build on.

run_chromium_webgl.py (Aquarium) and run_webgl_samples.py each install a shell
launcher that waits for the KDE session, starts Chromium as the session user
under a guest-uptime deadline, and names every failure on the console before
the runner gives up. This module owns what the two share: the deadline
arithmetic, the shell functions for the session wait, the Xauthority lookup,
the Chromium command and the reason Chromium stopped, the extraction of a
named failure from the console, and the VNC capture of what was rendered.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import math
from pathlib import Path
import subprocess
from typing import Sequence


# The launcher's own deadline precedes the runner's by this much, leaving room
# for its diagnostics and error line to reach the console log.
GUEST_REPORT_MARGIN_SECONDS = 45
# The boot and the KDE session must keep at least this much guest uptime.
MINIMUM_SESSION_SECONDS = 60
# The capture client waits up to 90 s for a rendered frame and names its own
# failure; the host bound lies beyond that wait.
CAPTURE_TIMEOUT_SECONDS = 120

# A launcher defines $session_deadline and $deadline (guest uptime, in
# seconds) before it calls these. fail() runs the launcher's diagnostics, if
# it has any, then prints its named error. run_chromium's own arguments follow
# the pinned flags, so a diagnostic flag given there overrides a pinned one.
# timeout(1) exits 124 at the deadline, and 137 when it had to kill Chromium.
LAUNCHER_FUNCTIONS = """uml_uid="$(id -u uml)"
runtime="/run/user/$uml_uid"
uptime_seconds()
{
    cut -d. -f1 /proc/uptime
}
fail()
{
    @FAIL_DIAGNOSTICS@
    echo "@ERROR_MARKER@ $*"
    exit 1
}
# $1: the display socket; $2: what it belongs to. The session bus must exist
# as well, and the session must have finished starting. Plasma's splash,
# ksplashqml, starts before plasmashell and exits once the session reports
# its start-up complete. Chromium started earlier shares the guest with the
# whole desktop start-up, and on a small guest its browser thread can then
# miss the 15 s its first child processes wait for their IPC connection.
wait_for_session()
{
    while [ ! -S "$1" ] || [ ! -S "$runtime/bus" ]; do
        if [ "$(uptime_seconds)" -ge "$session_deadline" ]; then
            fail "$2 did not become ready by guest uptime" \\
                "${session_deadline}s"
        fi
        sleep 1
    done
    while pgrep -x ksplashqml >/dev/null ||
        ! pgrep -x plasmashell >/dev/null; do
        if [ "$(uptime_seconds)" -ge "$session_deadline" ]; then
            fail "$2 did not finish starting by guest uptime" \\
                "${session_deadline}s"
        fi
        sleep 1
    done
    printf 'UML_SESSION_READY|UPTIME=%s\\n' "$(cut -d' ' -f1 /proc/uptime)"
}
find_xauthority()
{
    xauthority="$(find "$runtime" -maxdepth 1 -type f -name 'xauth_*' -print |
        head -n 1)"
    if [ -z "$xauthority" ]; then
        fail "Xauthority cookie was not found"
    fi
}
# $1: the seconds Chromium may run; the rest: its further arguments.
run_chromium()
{
    chromium_bound=$1
    shift
    timeout -k 10 "$chromium_bound" runuser -u uml -- env \\
        HOME=/home/uml \\
        DISPLAY=:0 \\
        WAYLAND_DISPLAY=wayland-0 \\
        XAUTHORITY="$xauthority" \\
        XDG_RUNTIME_DIR="$runtime" \\
        DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \\
        chromium \\
        --use-gl=angle \\
        --use-angle=gles-egl \\
        --ignore-gpu-blocklist \\
        --no-first-run \\
        --disable-crash-reporter \\
        --enable-logging=stderr \\
        --start-maximized \\
        "$@"
}
# $1: Chromium's exit status; $2: the workload whose report it was running for.
chromium_exit_reason()
{
    if [ "$1" = 124 ] || [ "$1" = 137 ]; then
        echo "$2 did not report by guest uptime ${deadline}s"
    else
        echo "Chromium exited with status $1 before $2 reported"
    fi
}"""


def launcher_functions(error_marker: str, diagnostics: str = ":") -> str:
    """Return the shared shell functions, naming failures ``error_marker``.

    ``diagnostics`` is a shell command fail() runs before its error line.
    """

    return LAUNCHER_FUNCTIONS.replace("@ERROR_MARKER@", error_marker).replace(
        "@FAIL_DIAGNOSTICS@", diagnostics
    )


def guest_deadline(timeout: float) -> int:
    """Return the launcher's guest-uptime deadline for a runner deadline.

    Guest uptime starts with the emulator, so a deadline expressed in uptime
    also absorbs however long the boot took.
    """

    return math.floor(timeout) - GUEST_REPORT_MARGIN_SECONDS


def require_session_budget(session_deadline: int, budget: str) -> None:
    """Refuse a ``budget`` that leaves the session too little uptime."""

    if session_deadline < MINIMUM_SESSION_SECONDS:
        raise ValueError(
            f"{budget} leaves the boot and KDE session less than "
            f"{MINIMUM_SESSION_SECONDS} s"
        )


def capture_rendered_frame(
    client: Path, password: str, port: int, path: Path
) -> None:
    """Save the display's first rendered frame to ``path``, in one attempt.

    The client connects to the port the display announced once it was bound,
    and waits for a rendered frame itself. A capture that fails anyway is the
    result: another attempt would pass over a blank or stale first frame after
    a zero-client interval, which is the display regression a capture exists
    to catch.
    """

    try:
        subprocess.run(
            [
                str(client.resolve()),
                "--password",
                password,
                "--capture",
                str(path),
                str(port),
            ],
            check=True,
            timeout=CAPTURE_TIMEOUT_SECONDS,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
    except subprocess.CalledProcessError as error:
        reason = (error.stderr or "").strip() or f"status {error.returncode}"
        raise RuntimeError(f"VNC capture of {path} failed: {reason}") from error


def guest_failure(text: str, markers: Sequence[str]) -> str:
    """Return the guest's first named failure, without console decoration."""

    for line in text.splitlines():
        for marker in markers:
            index = line.find(marker)
            if index >= 0:
                return line[index:].split('", source:', 1)[0].strip()
    return "the guest reported no failure"
