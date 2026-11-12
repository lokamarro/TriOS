#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the deterministic SLiRP guest network without external traffic.

The guest kernel deliberately has no ``CONFIG_IP_PNP``, so nothing is
configured before userspace starts and guest userspace owns every address.
This init-shell boot has no init system to run a DHCP client for it, so the
runner starts the one the product rootfs ships, ``systemd-networkd``, and then
proves the emulator's user-mode network answered with the documented lease:
address 10.0.2.15/24 and a default route through the gateway 10.0.2.2.

Every campaign then carries a burst of concurrent host connections through a
loopback host forward into an echo server in the guest.  SLiRP accepts one
connection per event-loop pass, so the host kernel queues the rest of the
burst on the forward's listening socket, and every client must read back its
own line.

Every campaign also asks for ``localhost`` through both the name server
address and the gateway address.  The provider answers them through the host's
system resolver, which answers ``localhost`` itself, so this needs no network.

The default campaign reaches only the SLiRP host gateway.  An explicit
``--external`` switch is deliberately required before the guest resolves or
contacts an Internet host; CMake registers that variant only when the caller
has opted into external-network permission.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import pathlib
import shlex
import socket
import sys
import threading
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402
from slirp_network import (  # noqa: E402
    NODE,
    start_guest_dhcp_client,
    verify_slirp_lease,
    wait_for_slirp_lease,
)


def slirp_dns_probe(server: str) -> str:
    """A node script: one DNS query to ``server`` must be answered by it."""
    return (
        'const dgram=require("dgram");'
        'const socket=dgram.createSocket("udp4");'
        'const query=Buffer.from('
        '"c0de01000001000000000000096c6f63616c686f73740000010001","hex");'
        "let done=false;"
        "const finish=(message)=>{"
        "if(done)return;done=true;clearTimeout(timer);socket.close();"
        "if(message){console.error(message);process.exitCode=1;}"
        'else console.log("slirp-dns-ok");};'
        'const timer=setTimeout(()=>finish("SLiRP DNS request timed out"),5000);'
        'socket.on("error",error=>finish(error.message));'
        'socket.on("message",(response,remote)=>{'
        f'if(remote.address!=={json.dumps(server)}||response.length<12||'
        'response.readUInt16BE(0)!==0xc0de){'
        'finish("invalid SLiRP DNS response");return;}finish();});'
        f"socket.send(query,53,{json.dumps(server)});"
    )


# The name server address, and the gateway address, which the provider answers
# through the host's system resolver as well.
SLIRP_DNS_PROBE = slirp_dns_probe("10.0.2.3")
SLIRP_GATEWAY_DNS_PROBE = slirp_dns_probe("10.0.2.2")


FORWARD_GUEST_PORT = 7777
FORWARD_CLIENTS = 16
FORWARD_ECHO_SERVER = (
    'require("net").createServer(socket=>socket.pipe(socket))'
    f'.listen({FORWARD_GUEST_PORT},"0.0.0.0",'
    '()=>console.log("forward-echo-ready"));'
)


def run_node(guest: Guest, script: str, timeout: int = 30) -> str:
    return guest.check_command(f"{NODE} -e {shlex.quote(script)}", timeout)


def unused_loopback_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def verify_concurrent_forward(guest: Guest, host_port: int) -> None:
    """Connect a burst of host clients through one forward at once."""
    guest.check_command(
        f"({NODE} -e {shlex.quote(FORWARD_ECHO_SERVER)} "
        ">/run/forward-echo.log 2>&1 </dev/null &)"
    )
    deadline = time.monotonic() + 30
    while "forward-echo-ready" not in guest.check_command(
        "cat /run/forward-echo.log"
    ):
        if time.monotonic() > deadline:
            raise RuntimeError("the guest echo server never became ready")
        time.sleep(0.5)

    start = threading.Barrier(FORWARD_CLIENTS)
    failures: list[str] = []

    def client(index: int) -> None:
        line = f"forward-client-{index}\n".encode()
        try:
            start.wait(10)
            with socket.create_connection(
                ("127.0.0.1", host_port), timeout=30
            ) as connection:
                connection.sendall(line)
                reply = b""
                while not reply.endswith(b"\n"):
                    chunk = connection.recv(64)
                    if not chunk:
                        break
                    reply += chunk
            if reply != line:
                failures.append(f"client {index} read back {reply!r}")
        except (OSError, threading.BrokenBarrierError) as error:
            failures.append(f"client {index}: {error!r}")

    clients = [
        threading.Thread(target=client, args=(index,))
        for index in range(FORWARD_CLIENTS)
    ]
    for thread in clients:
        thread.start()
    for thread in clients:
        thread.join(60)
    if any(thread.is_alive() for thread in clients):
        raise RuntimeError("a host forward client did not finish")
    if failures:
        raise RuntimeError(
            f"{len(failures)} of {FORWARD_CLIENTS} concurrent host forward "
            "clients failed:\n" + "\n".join(failures)
        )
    print(f"{FORWARD_CLIENTS} concurrent host forward connections echoed")


def external_probe(host: str) -> str:
    hostname = json.dumps(host)
    return (
        'const dns=require("dns");const https=require("https");'
        'dns.setServers(["10.0.2.3"]);'
        f"const hostname={hostname};"
        "dns.resolve4(hostname,(error,addresses)=>{"
        "if(error)throw error;"
        'if(!addresses.length)throw new Error("DNS lookup returned no address");'
        "const request=https.get({host:addresses[0],servername:hostname,"
        'headers:{Host:hostname},path:"/"},response=>{'
        "if(response.statusCode<200||response.statusCode>=400)"
        "process.exitCode=2;"
        "response.resume();response.on('end',()=>console.log('external-ok'));"
        "});request.on('error',error=>{throw error});});"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--external", action="store_true")
    parser.add_argument("--external-host", default="example.com")
    arguments = parser.parse_args()
    forward_port = unused_loopback_port()

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        (
            "ncpus=10",
            f"--net-forward=tcp:127.0.0.1:{forward_port}-{FORWARD_GUEST_PORT}",
        ),
        # The production rootfs is raw EROFS and is intentionally entered
        # through a temporary init shell, which keeps this smoke to the
        # emulator's network device and the guest's own DHCP client.
        # guest-overlay-systemd-smoke separately proves the overlay pivot and
        # a real systemd boot.
        init_shell=True,
    ) as guest:
        guest.boot_to_shell(180)
        start_guest_dhcp_client(guest)
        verify_slirp_lease(*wait_for_slirp_lease(guest))
        verify_concurrent_forward(guest, forward_port)
        for probe in (SLIRP_DNS_PROBE, SLIRP_GATEWAY_DNS_PROBE):
            dns_output = run_node(guest, probe, 15)
            if "slirp-dns-ok" not in dns_output:
                raise RuntimeError(
                    f"SLiRP DNS probe did not report success:\n{dns_output}"
                )
        if arguments.external:
            output = run_node(guest, external_probe(arguments.external_host), 60)
            if "external-ok" not in output:
                raise RuntimeError(
                    f"external connectivity probe did not report success:\n{output}"
                )
            print(f"external connectivity verified for {arguments.external_host}")
        else:
            print("deterministic SLiRP gateway connectivity verified")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
