#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Remove the Virtio video decoder and encoder while guest tasks use them.

UML removes its devices from an exitcall while the other vCPUs still run
tasks, and a management-console halt or reboot, an unbind, or a lost
vhost-user backend reaches the driver's remove without device_shutdown()
first. A video device's remove must reset the device and delete its queues
before it returns anything the device held, answer every pending command,
flush its event work, and keep its state until the last open file is
closed. An encoder also holds pictures in the driver until its coded queue
starts, and returns those too.

Every arm boots a guest with the decoder and the encoder (`--device` limits
it to one), opens and closes each node in a loop on every vCPU, holds each
open in the shell, and runs a client on each that has buffers queued which
the device or the driver keeps. Then it removes the devices:

- unbind: through sysfs, the encoder first and then the decoder. The
  client's next dequeue must fail with ENODEV and its close must return; the
  shell's descriptor then closes, the other device keeps working until its
  own removal, and the guest later powers off cleanly.
- halt: through the management console, which runs the exitcalls directly.
- poweroff: `poweroff -f`, through device_shutdown() and then the exitcalls.

The guest runs with slab_debug=FZPU, so freed memory is poisoned and checked
and a use after free faults instead of reading stale state. Nothing after the
removal may be a kernel warning, slab report, oops, panic or host fault
report, and each halt or power-off must end the emulator with status 0. The
contract is the driver-removal paragraph of kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import socket
import struct
import sys
import tempfile
from typing import NamedTuple


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


VCPUS = 4
OPENERS = VCPUS
EXIT_SECONDS = 60
CLIENT = "/opt/uml-test/video_teardown_probe.py"
PACKET = "/opt/uml-test/h264-baseline.bin"
DRIVER = "/sys/bus/virtio/drivers/virtio-video"
ENODEV = 19


class Device(NamedTuple):
    """One Virtio video device: its V4L2 card name, virtio device ID and
    the guest descriptor its shell holds open."""

    name: str
    virtio_id: int
    flag: str
    descriptor: int


DEVICES = {
    "decoder": Device("stateful-decoder", 31, "--video", 4),
    "encoder": Device("stateful-encoder", 30, "--video-encoder", 5),
}
FAILURE = re.compile(
    r"WARNING:|\bBUG\b|Oops|Call Trace:|Kernel panic|Darwin UML panic|"
    r"Darwin POSIX|Kernel mode fault|Poison overwritten|Redzone|"
    r"Object already free"
)
MCONSOLE_MAGIC = 0xCAFEBABE
MCONSOLE_VERSION = 2
MCONSOLE_DATA = 512


def mconsole(directory: pathlib.Path, command: str) -> str:
    """Send one management-console request and return its reply text.

    The guest answers at the address a request came from, so the request
    binds its own socket beside the console."""
    console = directory / "mconsole"
    if not console.is_socket():
        raise RuntimeError(f"the guest's management console is not {console}")
    reply = directory / "mc-video-teardown"
    reply.unlink(missing_ok=True)
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as endpoint:
        endpoint.bind(str(reply))
        try:
            data = command.encode()
            endpoint.sendto(
                struct.pack("<III", MCONSOLE_MAGIC, MCONSOLE_VERSION, len(data))
                + data.ljust(MCONSOLE_DATA, b"\0"),
                str(console),
            )
            endpoint.settimeout(EXIT_SECONDS)
            answer = endpoint.recv(12 + MCONSOLE_DATA)
        finally:
            reply.unlink(missing_ok=True)
    error, _, length = struct.unpack_from("<III", answer)
    if error:
        raise RuntimeError(
            f"mconsole {command} failed: {answer[12:12 + length]!r}"
        )
    return answer[12:12 + length].decode(errors="replace")


def fifo(key: str) -> str:
    return f"/tmp/video-teardown-{key}-trigger"


def client_log(key: str) -> str:
    return f"/tmp/video-teardown-{key}-client.log"


def stop_file(key: str) -> str:
    return f"/tmp/video-teardown-{key}-stop"


def client_job(key: str) -> str:
    return f"CLIENT_{key.upper()}"


def find_devices(guest: Guest, keys: tuple[str, ...]):
    """The device node and the virtio device name of each device in ``keys``."""
    guest.check_command(
        f"for i in $(seq 1 100); do "
        f"[ $(ls /sys/class/video4linux | wc -l) -ge {len(keys)} ] && break; "
        "sleep 0.1; done",
        30,
    )
    listing = guest.check_command(
        "for n in /sys/class/video4linux/video*; do "
        'echo "NODE $(cat $n/name) /dev/${n##*/}"; done; '
        f"for d in {DRIVER}/virtio*; do "
        'echo "VIRTIO $(basename $d) $(cat $d/device)"; done',
        30,
    )
    nodes = dict(re.findall(r"^NODE (\S+) (/dev/video\d+)$", listing, re.M))
    virtio = {
        int(device, 16): name
        for name, device in re.findall(
            r"^VIRTIO (\S+) (0x[0-9a-fA-F]+)$", listing, re.M
        )
    }
    found = {}
    for key in keys:
        device = DEVICES[key]
        if device.name not in nodes or device.virtio_id not in virtio:
            raise RuntimeError(f"the guest has no {key}:\n{listing}")
        found[key] = (nodes[device.name], virtio[device.virtio_id])
    return found


def start_load(guest: Guest, found: dict) -> None:
    """Openers on every vCPU, a held descriptor and a streaming client on
    each device.

    Each opener's open creates a stream with synchronous commands and its
    close destroys it, so commands are in flight on every vCPU while the
    device goes. Opens are read-only: `<>` would create a file once the node
    has gone."""
    per_device = max(1, OPENERS // len(found))
    for key, (node, _) in found.items():
        guest.check_command(
            f"rm -f {stop_file(key)} {fifo(key)}; mkfifo {fifo(key)}", 30
        )
        opener = (
            f"while [ ! -e {stop_file(key)} ]; do "
            f"( exec 3<{node} ) 2>/dev/null; done"
        )
        guest.check_command(
            " ".join(f"( {opener} ) >/dev/null 2>&1 &" for _ in range(per_device))
            + " true",
            30,
        )
        guest.check_command(f"exec {DEVICES[key].descriptor}<{node}", 30)
        if key == "decoder":
            command = f"python3 {CLIENT} decoder {node} {PACKET} {fifo(key)}"
        else:
            command = f"python3 {CLIENT} encoder {node} {fifo(key)}"
        guest.check_command(
            f"{command} >{client_log(key)} 2>&1 & {client_job(key)}=$!", 30
        )
    for key in found:
        output = guest.check_command(
            "for i in $(seq 1 300); do "
            f"grep -q 'VIDEO_CLIENT_\\(STREAMING\\|ERROR\\)' {client_log(key)} "
            f"&& break; sleep 0.1; done; cat {client_log(key)}",
            60,
        )
        if "VIDEO_CLIENT_STREAMING" not in output:
            raise RuntimeError(f"the {key} client never streamed:\n{output}")
    guest.check_command("sleep 1", 30)


def require_clean(tail: str, arm: str) -> None:
    failure = FAILURE.search(tail)
    if failure is not None:
        raise RuntimeError(
            f"{arm}: the removal printed {failure[0]!r}:\n"
            + tail[max(0, failure.start() - 2000):][:8000]
        )


def require_exit(guest: Guest, arm: str, request: int) -> None:
    if not guest.wait_for_exit(EXIT_SECONDS):
        raise RuntimeError(f"{arm}: the guest did not exit in {EXIT_SECONDS} s")
    tail = guest.output()[request:]
    require_clean(tail, arm)
    if guest.return_code != 0:
        raise RuntimeError(
            f"{arm}: the emulator exited with status {guest.return_code}:\n"
            + tail[-4000:]
        )


def unbind(guest: Guest, found: dict) -> None:
    request = len(guest.output())
    # The encoder goes first, while the decoder keeps streaming.
    for key in reversed(tuple(found)):
        node, device = found[key]
        guest.check_command(f"echo {device} > {DRIVER}/unbind", 60)
        require_clean(guest.output()[request:], f"unbind {key}")
        guest.check_command(
            f"touch {stop_file(key)}; python3 {CLIENT} --gone {node}", 30
        )
        closed = guest.check_command(
            f"echo go > {fifo(key)}; wait ${client_job(key)}; "
            f"cat {client_log(key)}",
            60,
        )
        expected = f"VIDEO_CLIENT_CLOSED dqbuf_errno={ENODEV}"
        if key == "encoder":
            expected += f" held_errno={ENODEV}"
        if expected not in closed:
            raise RuntimeError(
                f"unbind {key}: the client did not see ENODEV:\n{closed}"
            )
        guest.check_command(f"exec {DEVICES[key].descriptor}>&-", 30)
    guest.check_command("wait; echo alive", 60)
    require_clean(guest.output()[request:], "unbind")
    request = len(guest.output())
    if not guest.shutdown(EXIT_SECONDS):
        raise RuntimeError("unbind: the guest did not power off")
    require_exit(guest, "unbind power-off", request)


def halt(guest: Guest, directory: pathlib.Path) -> None:
    request = len(guest.output())
    mconsole(directory / f"uml-kern{guest.process_id}", "halt")
    require_exit(guest, "halt", request)


def poweroff(guest: Guest) -> None:
    request = len(guest.output())
    if not guest.shutdown(EXIT_SECONDS):
        raise RuntimeError("poweroff: the guest did not power off")
    require_exit(guest, "poweroff", request)


def run_arm(arguments: argparse.Namespace, arm: str,
            private: pathlib.Path) -> None:
    keys = tuple(arguments.devices)
    payloads = {
        CLIENT: GuestFile(
            (REPOSITORY / "tests/integration/kernel/video_teardown_probe.py")
            .read_bytes(),
            0o755,
        ),
        PACKET: GuestFile(
            (REPOSITORY / "tests/support/video/h264-baseline.bin").read_bytes()
        ),
    }
    with Guest(
        arguments.emulator,
        arguments.rootfs,
        "1G",
        "tmpfs",
        (*(DEVICES[key].flag for key in keys), f"ncpus={VCPUS}",
         "slab_debug=FZPU"),
        init_shell=True,
        engine=arguments.engine,
        payloads=payloads,
    ) as guest:
        guest.boot_to_shell(180)
        found = find_devices(guest, keys)
        start_load(guest, found)
        if arm == "unbind":
            unbind(guest, found)
        elif arm == "halt":
            halt(guest, private)
        else:
            poweroff(guest)
    print(f"{arm}: the {' and the '.join(keys)} went away cleanly while in use")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument(
        "--arm", action="append", choices=("unbind", "halt", "poweroff")
    )
    parser.add_argument(
        "--device", choices=("decoder", "encoder", "both"), default="both",
        help="the device to run with (default: both)",
    )
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()
    arguments.devices = (
        tuple(DEVICES) if arguments.device == "both" else (arguments.device,)
    )

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-video-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        for arm in arguments.arm or ("unbind", "halt", "poweroff"):
            run_arm(arguments, arm, private)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
