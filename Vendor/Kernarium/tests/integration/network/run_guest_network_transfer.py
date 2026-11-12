#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove guest TCP uploads through the SLiRP network reach the host promptly.

The guest connects to a listener on the host's loopback through the SLiRP
gateway alias 10.0.2.2 and sends bodies in the shapes applications produce: one
bulk stream, a request body written in paced chunks, several concurrent
streams, a stream whose host reader stops for a while, and many small
request/response exchanges. The host checks every byte by SHA-256 and replies
with its digest, so a report of success also proves the data arrived intact.

Every shape has a latency bound far above the healthy result and far below a
stall, which surfaces as retransmission timeouts, zero-window probes, or an
application-level timeout. The guest interface must also drop no frames.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import dataclasses
import hashlib
import json
import pathlib
import re
import socket
import sys
import threading
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402
from slirp_network import (  # noqa: E402
    NODE,
    start_guest_dhcp_client,
    verify_slirp_lease,
    wait_for_slirp_lease,
)


# A host reader that stops must push back through SLiRP to the guest's TCP
# window rather than disappear into an auto-tuned multi-megabyte socket buffer.
HOST_RECEIVE_BUFFER = 64 * 1024
STALL_AFTER_BYTES = 256 * 1024
STALL_SECONDS = 3.0
RESULT_MARKER = "TRANSFER-RESULT "

# Bounds, in milliseconds. Healthy results are one to two orders of magnitude
# lower; a stalled stream costs at least a retransmission timeout per loss and
# seconds per zero-window probe.
BULK_TOTAL_LIMIT_MS = 30_000
PACED_REPLY_LIMIT_MS = 1_000
PARALLEL_TOTAL_LIMIT_MS = 30_000
STALL_RESUME_LIMIT_MS = 5_000
EXCHANGE_LIMIT_MS = 1_000

GUEST_SCRIPT_PATH = "/usr/local/libexec/uml-test/transfer.js"
GUEST_SCRIPT = r"""
const net=require("net"),crypto=require("crypto"),fs=require("fs");
const HOST="10.0.2.2",PORT=Number(process.argv[2]);
const drops=()=>Number(fs.readFileSync("/sys/class/net/eth0/statistics/tx_dropped","utf8"));
async function measured(run){const before=drops(),result=await run();
result.tx_dropped=drops()-before;return result;}
function pattern(n,seed){const b=Buffer.alloc(n);let x=seed;
for(let i=0;i+4<=n;i+=4){x^=x<<13;x>>>=0;x^=x>>>17;x^=x<<5;x>>>=0;b.writeUInt32LE(x,i);}return b;}
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
function upload(name,body,chunk,gap){return new Promise((resolve,reject)=>{
const digest=crypto.createHash("sha256").update(body).digest("hex");
const s=net.connect(PORT,HOST);s.setNoDelay(true);let reply="",t0=0,tLast=0;
s.setTimeout(60000,()=>s.destroy(new Error(name+" timed out")));
s.on("connect",async()=>{t0=Date.now();s.write(name+" "+body.length+"\n");
for(let o=0;o<body.length;o+=chunk){if(o&&gap)await sleep(gap);tLast=Date.now();
if(!s.write(body.subarray(o,o+chunk)))await new Promise(r=>s.once("drain",r));}});
s.on("data",d=>reply+=d);s.on("error",reject);
s.on("end",()=>{const t=Date.now(),[hd,count]=reply.trim().split(" ");
resolve({name,bytes:body.length,ok:hd===digest&&Number(count)===body.length,
total_ms:t-t0,reply_ms:t-tLast});});});}
function exchanges(count,size){return new Promise((resolve,reject)=>{
const s=net.connect(PORT,HOST);s.setNoDelay(true);const message=pattern(size,7);
let pending=0,done=0,t=0;const rtts=[];
s.setTimeout(60000,()=>s.destroy(new Error("echo timed out")));
s.on("connect",()=>{s.write("echo "+count*size+"\n");t=Date.now();s.write(message);});
s.on("data",d=>{pending+=d.length;while(pending>=size){pending-=size;rtts.push(Date.now()-t);
if(++done===count){s.end();rtts.sort((a,b)=>a-b);
resolve({name:"echo",exchanges:count,ok:true,median_ms:rtts[count>>1],max_ms:rtts[count-1]});return;}
t=Date.now();s.write(message);}});s.on("error",reject);});}
(async()=>{const results=[];
results.push(await measured(()=>upload("bulk",pattern(16<<20,1),65536,0)));
results.push(await measured(()=>upload("paced",pattern(64<<10,2),4096,25)));
results.push(await measured(async()=>{const t=Date.now();
const parallel=await Promise.all([...Array(8).keys()].map(
i=>upload("parallel"+i,pattern(2<<20,10+i),65536,0)));
return {name:"parallel",ok:parallel.every(r=>r.ok),streams:parallel.length,
bytes:parallel.reduce((a,r)=>a+r.bytes,0),total_ms:Date.now()-t};}));
results.push(await measured(()=>upload("stall",pattern(4<<20,3),65536,0)));
results.push(await measured(()=>exchanges(200,1024)));
console.log("%MARKER%"+JSON.stringify(results));})().catch(e=>{console.error(e.message);process.exit(1);});
"""


@dataclasses.dataclass
class HostStream:
    name: str
    expected: int
    received: int = 0
    digest: str = ""
    first_byte: float = 0.0
    last_byte: float = 0.0
    resumed: float = 0.0


class TransferServer:
    """Loopback listener implementing the guest script's host half.

    A stream starts with ``NAME LENGTH\\n``. ``echo`` streams are echoed back
    until LENGTH bytes have arrived; every other stream is read to LENGTH and
    answered with ``SHA256 COUNT\\n``. A ``stall`` stream stops reading for
    STALL_SECONDS once STALL_AFTER_BYTES have arrived.
    """

    def __init__(self, stall_seconds: float = STALL_SECONDS) -> None:
        self.stall_seconds = stall_seconds
        self.streams: list[HostStream] = []
        self.errors: list[str] = []
        self._lock = threading.Lock()
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.setsockopt(
            socket.SOL_SOCKET, socket.SO_RCVBUF, HOST_RECEIVE_BUFFER)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(16)
        self.port = self._listener.getsockname()[1]
        self._thread = threading.Thread(target=self._accept, daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._listener.close()

    def stream(self, name: str) -> HostStream | None:
        with self._lock:
            return next((s for s in self.streams if s.name == name), None)

    def _accept(self) -> None:
        while True:
            try:
                connection, _ = self._listener.accept()
            except OSError:
                return
            threading.Thread(
                target=self._serve, args=(connection,), daemon=True).start()

    def _serve(self, connection: socket.socket) -> None:
        try:
            with connection:
                connection.settimeout(90)
                header = b""
                while not header.endswith(b"\n"):
                    byte = connection.recv(1)
                    if not byte or len(header) > 64:
                        raise ValueError(f"malformed stream header {header!r}")
                    header += byte
                name, length = header.decode().split()
                stream = HostStream(name, int(length))
                with self._lock:
                    self.streams.append(stream)
                if name == "echo":
                    self._echo(connection, stream)
                else:
                    self._receive(connection, stream)
        except Exception as error:  # reported by the runner, never swallowed
            with self._lock:
                self.errors.append(repr(error))

    def _echo(self, connection: socket.socket, stream: HostStream) -> None:
        while stream.received < stream.expected:
            data = connection.recv(65536)
            if not data:
                raise EOFError(f"echo closed after {stream.received} bytes")
            stream.received += len(data)
            connection.sendall(data)

    def _receive(self, connection: socket.socket, stream: HostStream) -> None:
        digest = hashlib.sha256()
        pause_pending = stream.name == "stall"
        while stream.received < stream.expected:
            limit = stream.expected - stream.received
            if pause_pending:
                limit = min(limit, STALL_AFTER_BYTES - stream.received)
            data = connection.recv(min(65536, limit))
            if not data:
                raise EOFError(
                    f"{stream.name} closed after {stream.received} bytes")
            now = time.monotonic()
            stream.first_byte = stream.first_byte or now
            stream.last_byte = now
            stream.received += len(data)
            digest.update(data)
            if pause_pending and stream.received >= STALL_AFTER_BYTES:
                time.sleep(self.stall_seconds)
                stream.resumed = time.monotonic()
                pause_pending = False
        stream.digest = digest.hexdigest()
        connection.sendall(f"{stream.digest} {stream.received}\n".encode())


def guest_command(port: int) -> str:
    return f"{NODE} {GUEST_SCRIPT_PATH} {port}"


def interface_counters(guest: Guest) -> dict[str, int]:
    statistics = guest.check_command(
        "cat /sys/class/net/eth0/statistics/tx_dropped "
        "/sys/class/net/eth0/statistics/rx_dropped"
    )
    tcp = guest.check_command("grep '^Tcp:' /proc/net/snmp")
    tx_dropped, rx_dropped = (int(v) for v in statistics.split()[:2])
    header, values = (line.split() for line in tcp.strip().splitlines()[:2])
    return {
        "tx_dropped": tx_dropped,
        "rx_dropped": rx_dropped,
        "retransmitted_segments": int(values[header.index("RetransSegs")]),
    }


def check_results(results: list[dict], server: TransferServer,
                  before: dict[str, int], after: dict[str, int]) -> list[str]:
    failures = [f"host stream error: {error}" for error in server.errors]
    by_name = {result["name"]: result for result in results}
    for result in results:
        if not result.get("ok"):
            failures.append(f"{result['name']}: data did not arrive intact")
    limits = {
        "bulk": ("total_ms", BULK_TOTAL_LIMIT_MS),
        "paced": ("reply_ms", PACED_REPLY_LIMIT_MS),
        "parallel": ("total_ms", PARALLEL_TOTAL_LIMIT_MS),
        "echo": ("max_ms", EXCHANGE_LIMIT_MS),
    }
    for name, (field, limit) in limits.items():
        result = by_name.get(name)
        if result is None:
            failures.append(f"{name}: no result")
        elif result[field] > limit:
            failures.append(f"{name}: {field} {result[field]} > {limit}")
    stall = server.stream("stall")
    if stall is None or not stall.resumed:
        failures.append("stall: host reader never resumed")
    else:
        resume_ms = round((stall.last_byte - stall.resumed) * 1000)
        by_name.setdefault("stall", {})["host_resume_to_end_ms"] = resume_ms
        if resume_ms > STALL_RESUME_LIMIT_MS:
            failures.append(
                f"stall: {resume_ms} ms from resume to the last byte > "
                f"{STALL_RESUME_LIMIT_MS}")
    for counter in ("tx_dropped", "rx_dropped"):
        if after[counter] != before[counter]:
            failures.append(
                f"eth0 {counter} rose by {after[counter] - before[counter]}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    server = TransferServer()
    with contextlib.closing(server), Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1024M",
        "tmpfs",
        ("ncpus=10",),
        init_shell=True,
        payloads={
            GUEST_SCRIPT_PATH: GuestFile(
                GUEST_SCRIPT.replace("%MARKER%", RESULT_MARKER).encode()
            )
        },
    ) as guest:
        guest.boot_to_shell(180)
        start_guest_dhcp_client(guest)
        verify_slirp_lease(*wait_for_slirp_lease(guest))
        before = interface_counters(guest)
        output = guest.check_command(guest_command(server.port), 180)
        after = interface_counters(guest)
        match = re.search(re.escape(RESULT_MARKER) + r"(\[.*\])", output)
        if match is None:
            raise RuntimeError(f"guest transfer reported no result:\n{output}")
        results = json.loads(match.group(1))
        failures = check_results(results, server, before, after)
        counters = {key: after[key] - before[key] for key in after}
        print(json.dumps({"results": results, "counters": counters}))
        if failures:
            raise RuntimeError("guest upload checks failed:\n  " +
                               "\n  ".join(failures))
        print("guest uploads reached the host promptly and intact")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
