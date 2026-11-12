#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that x86-64 and i386 processes fork from busy threads intact.

Cross-builds the freestanding fork_cow_probe.cpp for x86-64 and i386, boots
an x86-64 image with four vCPUs on an emulator built with the macos-x86_64
preset, and runs both probes. Worker threads count under a futex mutex
while a sibling forks, holding three lock words across each fork and
releasing them in the parent with glibc's "mov; xchg", whose exchange
faults on the page the fork write-protected. Every fork must leave the
workers running their own code with their counts intact, and every release
must reach the parent's copy, as the parent and the kernel both read it.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import pathlib
import re
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
# python3 -I leaves the script's own directory off sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from guest import Guest  # noqa: E402
from probe_runner import (  # noqa: E402
    GUEST_DIRECTORY, I386, X86_64, build_probes, parse_arguments)


PROBE = pathlib.Path(__file__).with_name("fork_cow_probe.cpp")
PROBES = {
    "fork-cow-probe": ("x86-64", X86_64),
    "fork-cow-probe32": ("i386", I386),
}
RESULT = re.compile(
    r"fork-cow: abi=(\S+) forks=(\d+) increments=(\d+) counter=(\d+) "
    r"overlaps=(\d+) lost=(\d+) verdict=(\S+)"
)
CPUS = 4
PROBE_TIMEOUT_SECONDS = 120


def main() -> int:
    arguments = parse_arguments(__doc__, timeout_help="boot timeout")

    payloads = build_probes(arguments.compiler, PROBE, PROBES)
    failures = []
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "512M",
        "tmpfs",
        (f"ncpus={CPUS}",),
        init_shell=True,
        engine=arguments.engine,
        payloads=payloads,
    ) as guest:
        guest.boot_to_shell(arguments.timeout)
        for name, (abi, _) in PROBES.items():
            result = guest.run_command(f"{GUEST_DIRECTORY}/{name}", PROBE_TIMEOUT_SECONDS)
            output = result.output.strip()
            print(output, flush=True)
            match = RESULT.search(output)
            if result.return_code != 0 or match is None or match.group(1) != abi \
                    or match.group(7) != "ok":
                failures.append(f"{abi}: exit status {result.return_code}")
    for failure in failures:
        print(f"FAIL {failure}")
    if failures:
        raise RuntimeError(f"{len(failures)} probe(s) failed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
