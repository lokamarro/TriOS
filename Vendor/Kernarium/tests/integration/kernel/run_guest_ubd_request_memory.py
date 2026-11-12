#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive every kind of UBD request with every allocation traced.

A UBD request carries its host I/O state in the data blk-mq allocates with
it, so neither ->queue_rq() nor the work item that runs the request
allocates memory: a driver that allocated there could find the atomic
reserve empty during writeback and print a page allocation failure, and it
would need memory to write back the dirty pages that reclaim waits for.

The guest traces slab and non-sleeping page allocations with their stacks
while it drives a scratch disk with direct writes and reads of 1 to 64
segments, a transfer the block layer splits, buffered writes that writeback
submits and a flush ends, a discard and a write-zeroes
(ubd_request_memory_probe.py). Every kind of request must reach the disk,
the traced submission path must show allocations, so the trace saw it,
none may be the driver's, nothing may be lost, everything must read back as
written, and the guest must print no kernel warning and power off cleanly.
The contract is the UBD paragraph of kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import sys
import tempfile

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


VCPUS = 2
DISK_BYTES = 64 << 20
PROBE = "/opt/uml-test/ubd_request_memory_probe.py"
PROBE_SECONDS = 300
FAILURE = re.compile(
    r"WARNING:|BUG:|Oops|Call Trace:|page allocation failure|Kernel panic|"
    r"Darwin UML panic|Darwin POSIX|Kernel mode fault"
)
RESULT = re.compile(r"^UBD_(REQUESTS|ALLOCATIONS|DATA) (.*)$", re.MULTILINE)


def parse(output: str) -> dict[str, dict[str, int]]:
    results: dict[str, dict[str, int]] = {}
    for match in RESULT.finditer(output):
        results[match[1]] = {
            key: int(value)
            for key, value in (field.split("=") for field in match[2].split())
        }
    return results


def check(emulator: pathlib.Path, rootfs: pathlib.Path, engine: str) -> None:
    probe = (
        REPOSITORY / "tests/integration/kernel/ubd_request_memory_probe.py"
    ).read_bytes()
    with tempfile.TemporaryDirectory(prefix="uml-ubd-memory-") as scratch:
        image = pathlib.Path(scratch) / "disk.img"
        with open(image, "wb") as handle:
            handle.truncate(DISK_BYTES)
        with Guest(
            emulator,
            rootfs,
            "512M",
            "tmpfs",
            (f"ncpus={VCPUS}", f"ubd1={image}"),
            init_shell=True,
            engine=engine,
            payloads={PROBE: GuestFile(probe, 0o755)},
        ) as guest:
            guest.boot_to_shell(180)
            mark = len(guest.output())
            output = guest.check_command(
                f"python3 {PROBE} /dev/ubdb", PROBE_SECONDS
            )
            results = parse(output)
            for name in ("REQUESTS", "ALLOCATIONS", "DATA"):
                if name not in results:
                    raise RuntimeError(
                        f"the probe printed no UBD_{name} line:\n" + output[-4000:]
                    )
            requests = results["REQUESTS"]
            idle = [kind for kind, count in requests.items() if count == 0]
            if idle:
                raise RuntimeError(
                    f"the probe sent the disk no {', '.join(idle)}:\n"
                    + output[-4000:]
                )
            allocations = results["ALLOCATIONS"]
            if allocations["submissions"] == 0:
                raise RuntimeError(
                    "the trace shows no allocation on the block submission "
                    "path, so it cannot show the driver's:\n" + output[-4000:]
                )
            if allocations["lost"] != 0:
                raise RuntimeError(
                    f"the trace lost {allocations['lost']} events:\n"
                    + output[-4000:]
                )
            if allocations["driver"] != 0:
                raise RuntimeError(
                    f"the UBD driver allocated memory {allocations['driver']} "
                    "times while it submitted or ran requests:\n"
                    + output[-4000:]
                )
            if results["DATA"]["mismatches"] != 0:
                raise RuntimeError(
                    f"{results['DATA']['mismatches']} transfers read back "
                    "wrong:\n" + output[-4000:]
                )
            tail = guest.output()[mark:]
            failure = FAILURE.search(tail)
            if failure is not None:
                raise RuntimeError(
                    f"the guest printed {failure[0]!r}:\n"
                    + tail[max(0, failure.start() - 2000):][:8000]
                )
            if not guest.shutdown(60) or guest.return_code != 0:
                raise RuntimeError("the guest did not power off cleanly")
            print(
                "UBD requests "
                + " ".join(f"{kind}={count}" for kind, count in requests.items())
                + f"; {allocations['events']} allocations traced, "
                f"{allocations['submissions']} on the submission path, "
                "none by the driver; data intact"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()
    check(arguments.emulator.resolve(), arguments.rootfs.resolve(), arguments.engine)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
