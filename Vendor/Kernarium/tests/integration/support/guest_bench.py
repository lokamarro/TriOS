# SPDX-License-Identifier: GPL-2.0-or-later
"""The options, boot, measured loop and report the guest benchmarks share."""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import pathlib
import statistics
import time
from typing import Callable

from guest import CORE_CLASSES, CommandResult, Guest
from host_metrics import cpu_time_seconds


def add_guest_arguments(
    parser: argparse.ArgumentParser,
    *,
    cpus: int,
    memory: str | None,
    engine: str = "jit",
) -> None:
    """Add the emulator, image, guest size, engine and core-class options.

    A runner that fixes its guest memory passes ``memory=None``. ``cpus`` is
    the runner's default vCPU count (4 on the macOS host); the comparison
    policy for vCPU counts is in tests/perf/README.md.
    """
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--cpus", type=int, default=cpus)
    if memory is not None:
        parser.add_argument("--memory", default=memory)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default=engine)
    parser.add_argument(
        "--core-class",
        choices=CORE_CLASSES,
        default="performance",
        help=(
            "host core class to measure on; 'efficiency' clamps the emulator "
            "to background QoS so it runs on the E-cores that dominate iOS"
        ),
    )


def boot(guest: Guest, shell_init: bool, timeout: float = 180) -> None:
    """Reach the shell; a bare init shell also gets a writable /tmp."""
    if not shell_init:
        guest.boot_and_login(timeout)
        return
    guest.boot_to_shell(timeout)
    mounts = guest.run_command("mount -t tmpfs -o mode=1777 tmpfs /tmp", 30)
    if mounts.return_code != 0:
        raise RuntimeError(f"guest mount setup failed\n{mounts.output}")


def measure(
    guest: Guest,
    command: str,
    repetitions: int,
    timeout: float,
    *,
    around: Callable[[int], contextlib.AbstractContextManager] | None = None,
    check: Callable[[int, CommandResult], None] | None = None,
) -> tuple[list[float], list[float]]:
    """Time each repetition of a guest command and print its cost.

    ``around(repetition)``, such as a sampler, encloses only the timed
    command; ``check(repetition, result)`` sees each successful result
    before its run is recorded. Returns the wall times and host cores used.
    """
    elapsed_samples: list[float] = []
    utilization_samples: list[float] = []
    for repetition in range(1, repetitions + 1):
        with around(repetition) if around else contextlib.nullcontext():
            before_cpu = cpu_time_seconds(guest.process_id)
            before_wall = time.monotonic()
            result = guest.run_command(command, timeout)
            elapsed = time.monotonic() - before_wall
            consumed = cpu_time_seconds(guest.process_id) - before_cpu
        if result.return_code != 0:
            raise RuntimeError(
                f"repetition {repetition} failed "
                f"({result.return_code})\n{result.output}"
            )
        if check is not None:
            check(repetition, result)
        utilization = consumed / elapsed
        elapsed_samples.append(elapsed)
        utilization_samples.append(utilization)
        print(
            f"run {repetition}: {elapsed:.3f}s wall, "
            f"{consumed:.3f}s host CPU ({utilization:.2f} cores)",
            flush=True,
        )
    return elapsed_samples, utilization_samples


def halt(guest: Guest) -> None:
    """Power the guest off; fail unless the emulator exits zero in 60 s."""
    shutdown_ok = guest.shutdown(60)
    emulator_status = guest.return_code
    if not shutdown_ok:
        raise RuntimeError("emulator did not halt cleanly within 60 seconds")
    if emulator_status != 0:
        raise RuntimeError(f"emulator exited with status {emulator_status}")


def topology(arguments: argparse.Namespace, **extra: object) -> str:
    """The guest shape a result row records: engine, core class, vCPUs.

    ``extra`` appends runner-specific fields, such as the worker count.
    """
    fields = {
        "engine": arguments.engine,
        "cores": arguments.core_class,
        "cpus": arguments.cpus,
        **extra,
    }
    pairs = (f"{name}={value}" for name, value in fields.items())
    return "[" + " ".join(pairs) + "]"


def print_median(
    elapsed: list[float], utilization: list[float], suffix: str = ""
) -> None:
    print(
        f"median: {statistics.median(elapsed):.3f}s wall, "
        f"{statistics.median(utilization):.2f} host cores{suffix}",
        flush=True,
    )
