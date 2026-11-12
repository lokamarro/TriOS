#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep a one-vCPU guest running while a vhost-user device's config churns.

An external test backend serves the guest a device with the ID of the Virtio
console. Every 2 ms it sends a config-change request on the vhost-user backend
channel and waits for the guest's acknowledgement; each change makes the
console driver's worker read the size from the config again over the main
socket. With one vCPU, that worker runs on the only CPU that services the
backend channel. A guest that waits for a reply on the main socket without
serving the backend channel stops for good: the backend answers queries such
as the config read meanwhile, but defers every other request until it has the
acknowledgement, so the boot or the command below never finishes.

The guest must keep running commands, and the backend must have raised and
had acknowledged many changes and answered many config reads, which shows that
the changes still reach the driver and that the two crossed.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import socket
import subprocess
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


# A driver that reads its config in a worker on every change binds a device
# of its type. The console's does when the device offers the size feature
# and not multiport, and it needs the input and output queues and struct
# virtio_console_config (cols, rows, max_nr_ports and emerg_wr) to bind.
CONSOLE_DEVICE_ID = 3
CONSOLE_QUEUES = 2
CONSOLE_CONFIG_BYTES = 12
CONSOLE_F_SIZE = 1 << 0
PERIOD_MILLISECONDS = 2
GUEST_MEMORY = "512M"
CHURN_SECONDS = 20
MINIMUM_CHANGES = 100
SERVER_EXIT_SECONDS = 20
SUMMARY = re.compile(
    rb"vhost-user churn: (\d+) config changes raised, "
    rb"(\d+) config reads answered"
)


def churn(guest: Guest) -> None:
    # The backend starts raising changes as soon as the guest connects, so
    # the boot can stop too.
    try:
        guest.boot_to_shell(180)
        churned = guest.run_command(
            f"sleep {CHURN_SECONDS}; echo CHURN-$((6*7))",
            CHURN_SECONDS + 60,
        )
    except TimeoutError as error:
        raise RuntimeError(
            "the guest stopped while its device's config churned: a "
            "config-change request and a config read waited for each other"
        ) from error
    if churned.return_code != 0 or "CHURN-42" not in churned.output:
        raise RuntimeError("the churn command failed:\n" + churned.output)
    if not guest.shutdown(30):
        raise RuntimeError("the guest did not power off")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--server", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    backend_end, guest_end = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    server = subprocess.Popen(
        [
            str(arguments.server.resolve()),
            "--fd", str(backend_end.fileno()),
            "--period-ms", str(PERIOD_MILLISECONDS),
            "--queues", str(CONSOLE_QUEUES),
            "--config-bytes", str(CONSOLE_CONFIG_BYTES),
            "--features", str(CONSOLE_F_SIZE),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        pass_fds=(backend_end.fileno(),),
    )
    backend_end.close()
    output = b""
    try:
        try:
            guest = Guest(
                arguments.emulator.resolve(),
                arguments.rootfs.resolve(),
                GUEST_MEMORY,
                "tmpfs",
                (
                    "ncpus=1",
                    f"virtio_uml.device=fd={guest_end.fileno()}:"
                    f"{CONSOLE_DEVICE_ID}",
                ),
                init_shell=True,
                pass_fds=(guest_end.fileno(),),
            )
        finally:
            guest_end.close()
        with guest:
            churn(guest)
        try:
            output, _ = server.communicate(timeout=SERVER_EXIT_SECONDS)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(
                "the churn backend did not exit with the guest"
            ) from error
        summary = SUMMARY.search(output)
        if server.returncode != 0 or summary is None:
            raise RuntimeError("the churn backend failed")
        changes, reads = (int(value) for value in summary.groups())
        if changes < MINIMUM_CHANGES or reads < MINIMUM_CHANGES:
            raise RuntimeError(
                "too few config changes reached the driver: "
                f"{changes} raised and {reads} config reads answered"
            )
    except BaseException:
        if not output:
            try:
                output, _ = server.communicate(timeout=SERVER_EXIT_SECONDS)
            except subprocess.TimeoutExpired:
                server.kill()
                output, _ = server.communicate()
        print(output.decode(errors="replace")[-4000:], file=sys.stderr)
        raise
    print(
        f"{changes} config changes acknowledged and {reads} config reads "
        "answered while the guest ran"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
