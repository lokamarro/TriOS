#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run Geekbench in a disposable overlay shell, from the image or a payload.

The program is Geekbench 6 on an arm64 or x86-64 image, or Geekbench 5 armv7
on a 32-bit Arm one; the guest script finds whichever the image or payload
carries."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import pathlib
import re
import sys


TESTS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TESTS / "integration" / "support"))
from guest import Guest, GuestFile  # noqa: E402
from guest_bench import add_guest_arguments, topology  # noqa: E402


# The workload script Kernarium runs as well; it prints the run's markers.
WORKLOAD = pathlib.Path(__file__).resolve().parent / "guest" / "geekbench.sh"
GUEST_WORKLOAD = "/usr/local/libexec/uml-test/geekbench"
GUEST_PARAMETERS = "/etc/uml-geekbench.env"
WORKLOAD_LIST = re.compile(r"[0-9]+(?:,[0-9]+)*")
# A payload image attaches as the second disk, which the workload mounts.
PAYLOAD_DISK = "ubd1rc"
PAYLOAD_DEVICE = "/dev/ubdb"
SHA256 = re.compile(r"[0-9a-f]{64}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    add_guest_arguments(parser, cpus=4, memory="8G")
    parser.add_argument("--console-log", required=True, type=pathlib.Path)
    parser.add_argument(
        "--workloads",
        metavar="LIST",
        help=(
            "run only these comma-separated single-core workload numbers "
            "instead of the full benchmark"
        ),
    )
    parser.add_argument(
        "--payload",
        metavar="IMAGE",
        type=pathlib.Path,
        help=(
            "attach this read-only EROFS image, which holds a Geekbench 6 "
            "or Geekbench 5 armv7 release at its root, and run that release "
            "instead of the rootfs's own payload; no x86-64 or armhf image "
            "carries one"
        ),
    )
    parser.add_argument(
        "--payload-sha256",
        metavar="DIGEST",
        help="the payload image's SHA-256, checked before the guest boots",
    )
    parser.add_argument(
        "--kernel-argument",
        action="append",
        default=[],
        metavar="ARGUMENT",
        help=(
            "append one extra guest kernel argument; repeatable. Isolating a "
            "kernel policy that the config turns on by default, such as "
            "'psi=0', needs a matched pair of runs that differ only here"
        ),
    )
    parser.add_argument(
        "--root-type",
        choices=("erofs", "ext4"),
        default="erofs",
        help=(
            "the rootfs's filesystem; ext4 is an unpartitioned filesystem "
            "image, such as the armhf image's root partition"
        ),
    )
    parser.add_argument(
        "--no-network",
        action="store_true",
        help=(
            "leave the guest without the user network's address and "
            "resolver, for an image that has no ip or a run that must not "
            "touch a network"
        ),
    )
    parser.add_argument("--timeout", type=int, default=7200)
    return parser.parse_args()


def verified_payload(
    image: pathlib.Path, expected: str | None
) -> pathlib.Path:
    """Return the payload image's path once its SHA-256 matches."""
    if expected is None or not SHA256.fullmatch(expected):
        raise ValueError("--payload needs --payload-sha256, in lowercase hex")
    path = image.resolve()
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    if digest.hexdigest() != expected:
        raise ValueError(f"{path} does not match --payload-sha256")
    return path


def main() -> int:
    arguments = parse_arguments()
    if arguments.cpus < 1 or arguments.cpus > 64:
        raise ValueError("--cpus must be between 1 and 64")
    if arguments.timeout < 1:
        raise ValueError("--timeout must be positive")
    payloads = {GUEST_WORKLOAD: GuestFile(WORKLOAD.read_bytes(), 0o755)}
    parameters = []
    if arguments.workloads is not None:
        if not WORKLOAD_LIST.fullmatch(arguments.workloads):
            raise ValueError("--workloads must be comma-separated numbers")
        parameters.append(f"UML_GEEKBENCH_WORKLOADS={arguments.workloads}\n")
    disks = ()
    if arguments.payload is not None:
        payload = verified_payload(arguments.payload, arguments.payload_sha256)
        disks = (f"{PAYLOAD_DISK}={payload}",)
        parameters.append(f"UML_GEEKBENCH_PAYLOAD={PAYLOAD_DEVICE}\n")
    elif arguments.payload_sha256 is not None:
        raise ValueError("--payload-sha256 needs --payload")
    if parameters:
        payloads[GUEST_PARAMETERS] = GuestFile("".join(parameters).encode())

    # Only a departure from the usual guest is passed on.
    boot_options: dict[str, str] = {}
    if arguments.root_type != "erofs":
        boot_options["root_type"] = arguments.root_type
    if arguments.no_network:
        boot_options["network"] = "image"

    arguments.console_log.parent.mkdir(parents=True, exist_ok=True)
    with arguments.console_log.open("wb") as console_log, Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        (
            f"ncpus={arguments.cpus}",
            *disks,
            *arguments.kernel_argument,
        ),
        overlay_shell=True,
        engine=arguments.engine,
        core_class=arguments.core_class,
        console_sink=console_log,
        payloads=payloads,
        **boot_options,
    ) as guest:
        guest.boot_and_login(180)
        print(f"geekbench guest {topology(arguments)}", flush=True)
        result = guest.run_command(GUEST_WORKLOAD, arguments.timeout)
        print(result.output, flush=True)
        shutdown_ok = guest.shutdown(60)
        if result.return_code != 0:
            raise RuntimeError(
                f"Geekbench exited with status {result.return_code}"
            )
        if not shutdown_ok:
            raise RuntimeError("guest did not halt cleanly after Geekbench")
        emulator_status = guest.return_code
        if emulator_status != 0:
            raise RuntimeError(
                f"emulator exited with status {emulator_status} after Geekbench"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
