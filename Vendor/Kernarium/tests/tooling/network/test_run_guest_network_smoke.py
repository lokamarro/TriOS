#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import importlib.util
from pathlib import Path
import sys
import unittest
from unittest import mock


SCRIPT = (Path(__file__).resolve().parents[2] /
          "integration/network/run_guest_network_smoke.py")
SPEC = importlib.util.spec_from_file_location("run_guest_network_smoke", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
run_guest_network_smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_guest_network_smoke)


class FakeGuest:
    def __enter__(self) -> FakeGuest:
        return self

    def __exit__(self, *exception: object) -> None:
        self.close()

    def boot_to_shell(self, timeout: int) -> None:
        del timeout

    def output(self) -> str:
        return ""

    def close(self) -> None:
        pass


class NetworkSmokeTaxonomyTests(unittest.TestCase):
    def run_campaign(self, external: bool) -> list[str]:
        arguments = [
            "run_guest_network_smoke.py",
            "--emulator",
            "/tmp/uml-darwin",
            "--rootfs",
            "/tmp/rootfs.erofs",
        ]
        if external:
            arguments.extend(["--external", "--external-host", "example.test"])

        scripts: list[str] = []

        def run_node(_guest: FakeGuest, script: str, _timeout: int) -> str:
            scripts.append(script)
            if script in (
                run_guest_network_smoke.SLIRP_DNS_PROBE,
                run_guest_network_smoke.SLIRP_GATEWAY_DNS_PROBE,
            ):
                return "slirp-dns-ok"
            return "external-ok"

        with (
            mock.patch.object(sys, "argv", arguments),
            mock.patch.object(
                run_guest_network_smoke, "Guest", return_value=FakeGuest()
            ),
            mock.patch.object(
                run_guest_network_smoke, "start_guest_dhcp_client"
            ) as start_dhcp,
            mock.patch.object(
                run_guest_network_smoke,
                "wait_for_slirp_lease",
                return_value=("", "", ""),
            ) as wait_for_lease,
            mock.patch.object(
                run_guest_network_smoke, "verify_slirp_lease"
            ) as verify_lease,
            mock.patch.object(
                run_guest_network_smoke, "run_node", side_effect=run_node
            ),
            mock.patch.object(
                run_guest_network_smoke, "verify_concurrent_forward"
            ) as verify_forward,
        ):
            self.assertEqual(run_guest_network_smoke.main(), 0)
            # Guest userspace owns address configuration: every campaign has
            # to start a DHCP client and wait for the lease before probing.
            start_dhcp.assert_called_once()
            wait_for_lease.assert_called_once()
            verify_lease.assert_called_once_with("", "", "")
            # Both campaigns carry host connections through the forward the
            # guest was booted with.
            verify_forward.assert_called_once()
        return scripts

    def test_default_campaign_asks_only_for_localhost(self) -> None:
        # The system resolver answers localhost itself, through the name
        # server and through the gateway, so the default campaign still
        # needs no network.
        scripts = self.run_campaign(external=False)
        self.assertEqual(
            scripts,
            [
                run_guest_network_smoke.SLIRP_DNS_PROBE,
                run_guest_network_smoke.SLIRP_GATEWAY_DNS_PROBE,
            ],
        )
        for script in scripts:
            self.assertIn("6c6f63616c686f7374", script)  # "localhost"

    def test_external_campaign_issues_dns_before_https(self) -> None:
        scripts = self.run_campaign(external=True)
        self.assertEqual(
            scripts[:2],
            [
                run_guest_network_smoke.SLIRP_DNS_PROBE,
                run_guest_network_smoke.SLIRP_GATEWAY_DNS_PROBE,
            ],
        )
        self.assertIn('"10.0.2.2"', scripts[1])
        self.assertEqual(len(scripts), 3)
        self.assertIn("example.test", scripts[2])


if __name__ == "__main__":
    unittest.main()
