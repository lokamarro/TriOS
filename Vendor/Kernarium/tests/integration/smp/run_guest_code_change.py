#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that an arm64 process runs the code it holds, not the code it held.

Boots the integration rootfs with several vCPUs on one engine and runs the
freestanding guest_code_change.cpp probe, which changes the code of functions
the engine has translated -- by a store from outside a function or from inside
the block that runs it, by mprotect round trips, by writing a page a fork
write-protected, by MAP_FIXED, munmap and mremap, from other vCPUs that keep
running it, and through a flood of code changes -- and requires every call
after a change to run the new instructions.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import sys


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "support"))
from guest import Guest, GuestFile  # noqa: E402


VERDICT = re.compile(
    r"^code-change: abi=a64 checks=(\d+) failed=(\d+) verdict=(\w+)$",
    re.MULTILINE)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--probe", required=True, type=pathlib.Path,
                        help="the freestanding arm64/Linux probe executable")
    parser.add_argument("--engine", choices=("gadgets", "jit"),
                        default="gadgets")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--rounds", type=int, default=3,
                        help="times to run the probe in one boot")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    probe = arguments.probe.resolve()
    if not probe.is_file():
        raise ValueError(f"probe is not a file: {probe}")
    path = f"/usr/local/libexec/uml-test/{probe.name}"
    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (f"ncpus={arguments.cpus}",),
        overlay_shell=True,
        payloads={path: GuestFile(probe.read_bytes(), 0o755)},
        engine=arguments.engine,
    )
    try:
        guest.boot_and_login()
        for round_index in range(arguments.rounds):
            result = guest.run_command(path, 120)
            for line in result.output.splitlines():
                if line.startswith("code-change: FAIL"):
                    print(line, flush=True)
            match = VERDICT.search(result.output)
            if match is None:
                raise RuntimeError(f"round {round_index}: exit "
                                   f"{result.return_code}, no verdict in "
                                   f"{result.output!r}")
            print(match.group(0), flush=True)
            if (result.return_code != 0 or match.group(3) != "ok" or
                    match.group(2) != "0"):
                return 1
        return 0
    finally:
        guest.close()


if __name__ == "__main__":
    raise SystemExit(main())
