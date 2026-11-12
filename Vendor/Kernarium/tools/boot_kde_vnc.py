#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot KDE Plasma from a read-only lower image in a native or VNC display."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import os
from pathlib import Path
import sys
from typing import Sequence

sys.path.insert(
    0, str(Path(__file__).resolve().parents[1] / "tests/integration/support")
)
from kde_session import (  # noqa: E402
    CDROM_MOUNT_POINT,
    build_harness_command,
    parse_arguments,
)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    if options.display == "native":
        print("A native Metal display window will open.", file=sys.stderr)
    authentication = (
        "with password authentication"
        if options.password is not None
        else "without authentication"
    )
    if options.display == "vnc":
        print(
            f"VNC will listen {authentication} on "
            f"{options.bind}:{options.port}.",
            file=sys.stderr,
        )
    if options.display == "vnc" and options.bind == "0.0.0.0":
        print(
            "The server is reachable on every IPv4 interface; use "
            "--bind 127.0.0.1 for local-only access.",
            file=sys.stderr,
        )
    if options.serial_socket is not None:
        print(
            f"Serial console will listen on {options.serial_socket}.",
            file=sys.stderr,
        )
    if options.virtio_console_socket is not None:
        print(
            "Virtio console will expose hvc0 on "
            f"{options.virtio_console_socket}.",
            file=sys.stderr,
        )
    if options.iso is not None:
        print(
            f"ISO {options.iso} will be mounted read-only at "
            f"{CDROM_MOUNT_POINT}.",
            file=sys.stderr,
        )
    print("Press Ctrl-C in this terminal to stop the guest.", file=sys.stderr)
    command = build_harness_command(options)
    os.execv(command[0], command)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
