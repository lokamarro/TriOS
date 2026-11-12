# SPDX-License-Identifier: GPL-2.0-or-later
"""The host ports of the guest slot a runner holds, for its default ports.

A process that holds a guest slot has GUEST_SLOT_PORT_BASE set and owns the
ports base..base+99; no other slot's guest uses them. A runner's default port
comes from the slot, so two runners in different slots never clash; outside a
slot it is the runner's fixed fallback.

Layout inside a slot:

- base+0 is the adb forward of the guest the slot runs;
- base+50..base+99 hold private adb servers (adb_debugging's stranger-key step
  is the one that starts one);
- the rest is free for a runner's own extras.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

from collections.abc import Mapping
import os

SLOT_PORT_VARIABLE = "GUEST_SLOT_PORT_BASE"
SLOT_PORTS = 100
ADB_OFFSET = 0
PRIVATE_SERVER_FLOOR = 50


def slot_base(environ: Mapping[str, str] | None = None) -> int | None:
    """The slot's first port, or None when the process holds no slot."""
    value = (os.environ if environ is None else environ).get(SLOT_PORT_VARIABLE)
    if not value:
        return None
    if not value.isdecimal() or not 1024 <= int(value) <= 65535 - (SLOT_PORTS - 1):
        raise ValueError(f"{SLOT_PORT_VARIABLE} is not a usable port base: {value!r}")
    return int(value)


def slot_port(offset: int, fallback: int, environ: Mapping[str, str] | None = None) -> int:
    """The slot's port at the offset, or the fixed fallback outside a slot."""
    base = slot_base(environ)
    return fallback if base is None else base + offset
