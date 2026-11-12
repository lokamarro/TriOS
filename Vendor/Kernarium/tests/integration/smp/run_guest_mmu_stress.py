#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise shared-mm data, permissions, fork COW, and mapping churn."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import sys
import time


SKIP = 77
TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "support"))
from guest import Guest, GuestFile  # noqa: E402
from host_metrics import cpu_time_seconds  # noqa: E402


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument(
        "--shared-mmap-binary",
        required=True,
        type=pathlib.Path,
        help="freestanding arm64/Linux shared-mm stress executable",
    )
    parser.add_argument(
        "--fork-cow-binary",
        required=True,
        type=pathlib.Path,
        help="freestanding arm64/Linux fork copy-on-write stress executable",
    )
    return parser.parse_args()


def run_timed(guest: Guest, name: str, command: str) -> str:
    before_cpu = cpu_time_seconds(guest.process_id)
    before_wall = time.monotonic()
    output = guest.check_command(command)
    elapsed = time.monotonic() - before_wall
    consumed = cpu_time_seconds(guest.process_id) - before_cpu
    print(
        f"{name}: {elapsed:.3f}s wall, {consumed:.3f}s host CPU "
        f"({consumed / elapsed:.2f} cores)",
        flush=True,
    )
    return output


def guest_executable(binary: pathlib.Path) -> tuple[str, GuestFile]:
    binary = binary.resolve()
    if not binary.is_file():
        raise ValueError(f"guest stress binary is not a file: {binary}")
    return (
        f"/usr/local/libexec/uml-test/{binary.name}",
        GuestFile(binary.read_bytes(), 0o755),
    )


def require_marker(name: str, output: str, accepts) -> None:
    # Kernel diagnostics share the serial console. run_timed() already
    # requires exit status zero; require the payload's standalone marker.
    lines = output.splitlines()
    if sum(1 for line in lines if accepts(line)) != 1:
        raise RuntimeError(f"{name} returned unexpected output {output!r}")
    for line in lines:
        if line != "OK":
            print(line, flush=True)


def main() -> int:
    arguments = parse_arguments()
    host_cpus = min(os.cpu_count() or 1, 64)
    if arguments.cpus < 1:
        raise ValueError("--cpus must be at least 1")
    if arguments.cpus > host_cpus:
        print(
            f"guest-mmu-stress: SKIP: needs {arguments.cpus} host CPUs, "
            f"host has {host_cpus}",
            flush=True,
        )
        return SKIP

    shared_mmap, shared_mmap_file = guest_executable(
        arguments.shared_mmap_binary
    )
    fork_cow, fork_cow_file = guest_executable(arguments.fork_cow_binary)
    if shared_mmap == fork_cow:
        raise ValueError("the stress binaries need distinct names")
    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (f"ncpus={arguments.cpus}",),
        overlay_shell=True,
        payloads={shared_mmap: shared_mmap_file, fork_cow: fork_cow_file},
        engine=arguments.engine,
    )
    try:
        guest.boot_and_login()
        output = run_timed(guest, "shared-mm-distant-mmap", shared_mmap)
        require_marker(
            "shared-mm workload", output, lambda line: line == "OK"
        )
        # Threads that store to and futex-wait on a page which a sibling's
        # forks keep write-protecting: a store or an unlock lost to the page
        # Linux replaced fails the payload, which reports what it counted.
        output = run_timed(guest, "fork-cow-coherence", fork_cow)
        require_marker(
            "fork copy-on-write workload",
            output,
            lambda line: line.startswith("FORK-COW OK "),
        )
        return 0
    finally:
        guest.close()


if __name__ == "__main__":
    raise SystemExit(main())
