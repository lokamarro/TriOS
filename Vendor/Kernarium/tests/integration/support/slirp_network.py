# SPDX-License-Identifier: GPL-2.0-or-later
"""Bring up the SLiRP guest network from a bare init shell.

The guest kernel deliberately has no ``CONFIG_IP_PNP``, so nothing is
configured before userspace starts and guest userspace owns every address.
An init-shell boot has no init system to run a DHCP client, so these helpers
start the one the product rootfs ships, ``systemd-networkd``, and wait for the
emulator's user-mode network to answer with the documented lease: address
10.0.2.15/24 and a default route through the gateway 10.0.2.2.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import re
import time

from guest import Guest


NODE = "/opt/node/bin/node"
NETWORKD = "/lib/systemd/systemd-networkd"
NETWORKD_LOG = "/run/uml-networkd.log"
NETWORKD_RUNTIME = "/run/systemd/netif"
NETWORK_DIRECTORY = "/run/systemd/network"
NETWORK_UNIT = f"{NETWORK_DIRECTORY}/10-eth0.network"
LEASE_TIMEOUT_SECONDS = 30
LEASE_POLL_SECONDS = 0.25
# 0202000A is the little-endian /proc/net/route spelling of 10.0.2.2.
SLIRP_DEFAULT_ROUTE = re.compile(r"^eth0\s+00000000\s+0202000A\s+", re.MULTILINE)
SLIRP_LEASE_ADDRESS = re.compile(r"\binet 10\.0\.2\.15/24\b")


def start_guest_dhcp_client(guest: Guest) -> None:
    """Start the DHCP client the product rootfs ships on a bare init shell.

    ``systemd-networkd`` is the rootfs's only DHCP client, and here it runs
    without the rest of systemd, so the runner supplies the three things its
    unit would have provided.
    """
    # systemd reads a writable /sys as "udev runs here" and then parks every
    # link in "pending udev initialization" indefinitely.  A read-only /sys is
    # its documented convention for a userspace that has no udev, which is
    # exactly this boot.
    guest.check_command("mount -t sysfs -o ro sysfs /sys")
    # An init would have created /run, so the drop-in and the daemon's
    # runtime state both live on a disposable tmpfs there.
    guest.check_command("mount -t tmpfs tmpfs /run")
    guest.check_command(f"mkdir -p {NETWORK_DIRECTORY}")
    # networkd drops to the systemd-network user and refuses a runtime
    # directory owned by anyone else, which is what its unit's
    # RuntimeDirectory= would have created for it.
    guest.check_command(
        "install -d -o systemd-network -g systemd-network -m 0755 "
        f"{NETWORKD_RUNTIME}",
    )
    guest.check_command(
        "printf '%s\\n' '[Match]' 'Name=eth0' '[Network]' 'DHCP=ipv4' "
        f"> {NETWORK_UNIT}",
    )
    # The console harness appends "; rc=$?" to every command, so a trailing
    # ampersand would be a shell syntax error.  The subshell detaches the
    # daemon and keeps that wrapper valid.
    guest.check_command(f"({NETWORKD} >{NETWORKD_LOG} 2>&1 </dev/null &)")


def wait_for_slirp_lease(
    guest: Guest, timeout_seconds: int = LEASE_TIMEOUT_SECONDS
) -> tuple[str, str, str]:
    """Return the guest routing, interface and address tables once DHCP lands.

    The lease normally arrives well inside a second; the wait exists so a
    loaded host reports the daemon's own diagnosis instead of an empty table.
    """
    deadline = time.monotonic() + timeout_seconds
    while True:
        route_table = guest.check_command("cat /proc/net/route")
        if SLIRP_DEFAULT_ROUTE.search(route_table) is not None:
            return (
                route_table,
                guest.check_command("cat /proc/net/dev"),
                guest.check_command("ip -4 -oneline addr show eth0"),
            )
        if time.monotonic() >= deadline:
            raise RuntimeError(
                "guest userspace did not acquire the SLiRP DHCP lease within "
                f"{timeout_seconds}s:\n{route_table}\n"
                f"{guest.check_command(f'cat {NETWORKD_LOG}')}"
            )
        time.sleep(LEASE_POLL_SECONDS)


def verify_slirp_lease(
    route_table: str, interfaces: str, addresses: str
) -> None:
    """Check the tables that wait_for_slirp_lease returned.

    That wait returns only once route_table holds the default route through
    the SLiRP gateway, so only the interface and the lease remain to check.
    """
    if re.search(r"^\s*eth0:", interfaces, re.MULTILINE) is None:
        raise RuntimeError(f"SLiRP eth0 interface is missing:\n{interfaces}")
    # The single SLiRP lease is also the default target of a host forward, so
    # the address the guest is handed is part of the network's contract.
    if SLIRP_LEASE_ADDRESS.search(addresses) is None:
        raise RuntimeError(
            f"eth0 did not receive the SLiRP lease 10.0.2.15/24:\n{addresses}"
        )
