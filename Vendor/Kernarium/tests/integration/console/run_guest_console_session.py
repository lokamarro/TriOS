#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The emulator stays in its launcher's session and process group.

Boots a guest to a shell on the harness console, whose output is a PTY that
is not the emulator's controlling terminal, and requires the emulator's
session and process group to still be the launcher's: a signal to the group
that started the guest, such as a timeout's or a Ctrl-C, must reach it. A
console that made the emulator a session leader also made that PTY its
controlling terminal. Upstream's window-size helper does this to its own
process; a Darwin kernel's helper would be a thread and would move the whole
emulator. The contract is in kernel/common/README.md. macOS only.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        ("ncpus=1",),
        init_shell=True,
        engine=arguments.engine,
    ) as guest:
        # The kernel opens the console for init, so by the shell prompt the
        # line, and any helper it starts, is up.
        guest.boot_to_shell(180)
        emulator = guest.process_id
        launcher = (os.getsid(0), os.getpgid(0))
        found = (os.getsid(emulator), os.getpgid(emulator))
        print(
            f"launcher session {launcher[0]} group {launcher[1]}; "
            f"emulator {emulator} session {found[0]} group {found[1]}"
        )
        if found != launcher:
            raise RuntimeError(
                "the emulator left its launcher's session or process group "
                f"(session {found[0]}, group {found[1]}; the launcher's are "
                f"{launcher[0]} and {launcher[1]}): signals to the group that "
                "started it miss the guest"
            )
        if not guest.shutdown(60):
            raise RuntimeError("the guest did not power off within 60 s")
        print("the emulator kept its launcher's session and process group")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
