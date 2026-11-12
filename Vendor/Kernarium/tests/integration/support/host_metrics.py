# SPDX-License-Identifier: GPL-2.0-or-later
"""Host-side metrics shared by guest integration and benchmark runners."""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import subprocess


def cpu_time_seconds(process_id: int) -> float:
    value = subprocess.check_output(
        ["ps", "-p", str(process_id), "-o", "time="],
        text=True,
    ).strip()
    if not value:
        raise RuntimeError("ps returned no CPU time for the emulator")
    days = 0
    if "-" in value:
        day_text, value = value.split("-", 1)
        days = int(day_text)
    fields = value.split(":")
    if len(fields) == 2:
        hours = 0
        minutes, seconds = fields
    elif len(fields) == 3:
        hours, minutes, seconds = fields
    else:
        raise RuntimeError(f"unrecognized ps CPU time: {value!r}")
    return (
        days * 86400
        + int(hours) * 3600
        + int(minutes) * 60
        + float(seconds)
    )
