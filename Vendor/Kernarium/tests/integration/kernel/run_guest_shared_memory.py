#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that memory the kernel shares with user space stays coherent.

The guest runs shared_memory_probe, which shares BPF arrays and ring buffers,
a perf ring, PACKET_MMAP rings and io_uring rings between a kernel alias and
the application's mapping, and checks both directions after the mapping
exists. Every kernel alias outside the linear map is a host-private copy of
its frames unless the kernel maps it as whole aligned groups of four pages, so
a view that stops following the other names the kernel memory that lost its
sharing. An io_uring ring in the application's own scattered pages cannot be
shared and must be refused.
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


GUEST_PROBE = "/usr/local/libexec/uml-test/shared_memory_probe"
PROBE_TIMEOUT_SECONDS = 600
# The results of every ring size, including the rings below a 16 KiB group,
# where the kernel synchronizes a wrapped record itself.
EXPECTED_RESULTS = (
    "array",
    "ringbuf-16k",
    "ringbuf-64k",
    "user-ringbuf-16k",
    "user-ringbuf-64k",
    "perf",
    "packet-pages",
    "packet-vzalloc",
    "iouring-kernel-rings",
    "iouring-pbuf-ring-mmap",
    "iouring-pbuf-ring-user-page",
    "iouring-user-memory",
    "ringbuf-4k",
    "ringbuf-8k",
    "user-ringbuf-4k",
    "user-ringbuf-8k",
)
RESULT_LINE = re.compile(r"^RESULT (\S+) (PASS|FAIL) (.*)$")
# Kernel complaints that a passing probe must not leave behind: the warning
# that vmalloc memory mapped to the application is not whole groups, and a
# host mapping that failed.
VMAP_GROUP_WARNING = "remap_vmalloc_range: memory from"
HOST_FAILURE = "Darwin: "


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--cpus", type=int, default=4)
    arguments = parser.parse_args()

    expected = set(EXPECTED_RESULTS)

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (f"ncpus={arguments.cpus}",),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={
            GUEST_PROBE: GuestFile(arguments.probe.read_bytes(), 0o755)
        },
    ) as guest:
        guest.boot_and_login()
        # PACKET_MMAP receives frames that a UDP datagram sends to loopback.
        guest.check_command("ip link set lo up", 30)
        page_size = guest.check_command("getconf PAGESIZE", 30).strip()
        if page_size != "4096":
            raise RuntimeError(f"the guest page size is {page_size}, not 4096")
        result = guest.run_command(f"{GUEST_PROBE} all", PROBE_TIMEOUT_SECONDS)
        print(result.output.strip(), flush=True)
        console = guest.output()
        guest.check_command("echo alive", 30)

    results: dict[str, str] = {}
    for line in result.output.splitlines():
        match = RESULT_LINE.match(line.strip())
        if match:
            results[match.group(1)] = match.group(2)
    failed = sorted(name for name, state in results.items() if state != "PASS")
    missing = sorted(expected - set(results))
    unexpected = sorted(set(results) - expected)
    complaints = [
        line.strip()
        for line in console.splitlines()
        if VMAP_GROUP_WARNING in line
        or (HOST_FAILURE in line and "failed" in line)
    ]
    problems = []
    if failed:
        problems.append(f"failed: {', '.join(failed)}")
    if missing:
        problems.append(f"missing: {', '.join(missing)}")
    if unexpected:
        problems.append(f"unexpected: {', '.join(unexpected)}")
    if result.return_code != 0 and not failed:
        problems.append(f"the probe exited with {result.return_code}")
    for line in complaints:
        problems.append(f"kernel: {line}")
    if problems:
        raise RuntimeError("; ".join(problems))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
