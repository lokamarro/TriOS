#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The macOS launcher refuses a host device option it cannot honour.

Every case ends before the guest boots, so the test needs no rootfs and holds
no guest slot. The launcher must stop with status 1 and one named reason when
a host device option collides with an explicit ``virtio_uml.device=``
argument of the same device, when an option is repeated, empty or
contradicted, and when the in-process service cannot start; it must leave its
scratch initrd behind in none of them. An informational run (``--version`` or
``--help``) parses every host device option and starts and refuses nothing.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import itertools
import os
import pathlib
import subprocess
import sys
import tempfile

TIMEOUT_SECONDS = 30
# Skips the rootfs lookup; no case boots far enough to open it.
NO_ROOTFS = "ubd0=/dev/null"
# The Virtio device identifiers of the explicit transport arguments.
BATTERY, CONSOLE, AUDIO, VIDEO, VIDEO_ENCODER, GPU, VSOCK = (
    65535, 3, 25, 31, 30, 16, 19)


def explicit(device_id: int) -> str:
    return f"virtio_uml.device=/explicit:{device_id}"


COLLISION = "{} cannot be combined with an explicit {} transport argument"
CONSOLE_COLLISION = COLLISION.format(
    "a host Virtio console or serial socket", "Virtio console"
)
CONFIGURE = "Unable to configure host devices: "

# (arguments, the stderr line the launcher must print)
REFUSALS: list[tuple[list[str], str]] = [
    (["--battery", explicit(BATTERY)], COLLISION.format("--battery", "Virtio battery")),
    (["--video", explicit(VIDEO)], COLLISION.format("--video", "Virtio video")),
    (["--video-encoder", explicit(VIDEO_ENCODER)],
     COLLISION.format("--video-encoder", "Virtio video encoder")),
    (["--audio", explicit(AUDIO)], COLLISION.format("--audio", "Virtio Sound")),
    (["--gpu", explicit(GPU)], COLLISION.format("--gpu", "Virtio-GPU")),
    (["--vsock-forward=0-5555", explicit(VSOCK)],
     COLLISION.format("--vsock-forward", "Virtio vsock")),
    (["--vsock-forward="],
     CONFIGURE + "invalid vsock forward '': expected "
     "[127.0.0.1:]HOSTPORT-GUESTPORT"),
    (["--vsock-forward=0.0.0.0:0-5555"],
     CONFIGURE + "invalid vsock forward '0.0.0.0:0-5555': only a loopback "
     "address is accepted"),
    (["--vsock-forward=198.51.100.7:0-5555"],
     CONFIGURE + "invalid vsock forward '198.51.100.7:0-5555': only a "
     "loopback address is accepted"),
    (["--vsock-forward=0-0"],
     CONFIGURE + "invalid vsock forward '0-0': the guest port must be a "
     "positive port number"),
    (["--vsock-forward=99999-5555"],
     CONFIGURE + "invalid vsock forward '99999-5555': the host port must be "
     "0 to 65535"),
    (["--virtio-console-socket={tmp}/console.sock", explicit(CONSOLE)],
     CONSOLE_COLLISION),
    (["--virtio-serial-socket={tmp}/serial.sock", explicit(CONSOLE)],
     CONSOLE_COLLISION),
    (["--virtio-console-socket=/a", "--virtio-console-socket=/b"],
     CONFIGURE + "--virtio-console-socket may be specified only once"),
    (["--virtio-console-socket="],
     CONFIGURE + "--virtio-console-socket requires a path"),
    (["--virtio-serial-socket=/a", "--virtio-serial-socket=/b"],
     CONFIGURE + "--virtio-serial-socket may be specified only once"),
    (["--virtio-serial-socket="],
     CONFIGURE + "--virtio-serial-socket requires a path"),
    (["--virtio-console-socket=/a", "--virtio-serial-socket=/b"],
     CONFIGURE + "--virtio-console-socket and --virtio-serial-socket are "
     "mutually exclusive"),
    (["--gpu-display=bogus"],
     CONFIGURE + "--gpu-display must be native, offscreen or vnc"),
    (["--audio-output=bogus"], CONFIGURE + "--audio-output must be host or null"),
    (["--audio-output="], CONFIGURE + "--audio-output must be host or null"),
    (["--audio-output=null", explicit(AUDIO)],
     COLLISION.format("--audio", "Virtio Sound")),
    (["--audio-input=bogus"], CONFIGURE + "--audio-input must be silent or host"),
    (["--audio-input="], CONFIGURE + "--audio-input must be silent or host"),
    (["--audio-input=silent", explicit(AUDIO)],
     COLLISION.format("--audio", "Virtio Sound")),
    (["--gpu-vnc-bind="], CONFIGURE + "invalid host option: --gpu-vnc-bind="),
    (["--gpu-vnc-password="],
     CONFIGURE + "VNC password must contain between 1 and 8 bytes"),
    (["--gpu-vnc-password=123456789"],
     CONFIGURE + "VNC password must contain between 1 and 8 bytes"),
    (["--gpu-vnc-port=x"], CONFIGURE + "invalid host option: --gpu-vnc-port=x"),
    (["--gpu-width=0"], CONFIGURE + "invalid host option: --gpu-width=0"),
    (["--gpu-auto-resize=maybe"],
     CONFIGURE + "--gpu-auto-resize must be on or off"),
    (["--net-forward="],
     CONFIGURE + "--net-forward requires "
     "[PROTO:][HOSTADDR:]HOSTPORT-[GUESTADDR:]GUESTPORT"),
    (["--gpu-display=native", "--gpu-vnc-port=1"],
     CONFIGURE + "--gpu-vnc-* options require --gpu-display=vnc"),
    (["--gpu-display=offscreen", "--gpu-vnc-port=1"],
     CONFIGURE + "--gpu-vnc-* options require --gpu-display=vnc"),
    (["--gpu-offscreen-size=800x600"],
     CONFIGURE + "--gpu-offscreen-* options require --gpu-display=offscreen"),
    (["--gpu-display=vnc", "--gpu-offscreen-mirror=on"],
     CONFIGURE + "--gpu-offscreen-* options require --gpu-display=offscreen"),
    (["--gpu-display=offscreen", "--gpu-offscreen-size=800"],
     CONFIGURE + "invalid host option: --gpu-offscreen-size=800"),
    (["--gpu-display=offscreen", "--gpu-offscreen-size=800x0"],
     CONFIGURE + "invalid host option: --gpu-offscreen-size=800x0"),
    (["--gpu-display=offscreen", "--gpu-offscreen-size=ax600"],
     CONFIGURE + "invalid host option: --gpu-offscreen-size=ax600"),
    (["--gpu-display=offscreen", "--gpu-offscreen-mirror=800"],
     CONFIGURE + "invalid host option: --gpu-offscreen-mirror=800"),
    (["--gpu-display=offscreen", "--gpu-offscreen-mirror=800@0"],
     CONFIGURE + "invalid host option: --gpu-offscreen-mirror=800@0"),
    (["--gpu-display=offscreen", "--gpu-auto-resize=on"],
     CONFIGURE + "--gpu-auto-resize requires --gpu-display=native"),
    (["--gpu-auto-resize=on"],
     CONFIGURE + "--gpu-auto-resize requires --gpu-display=native"),
    (["--gpu-dpi=96", "--gpu-width=5000"],
     CONFIGURE + "--gpu-dpi requires display dimensions no larger than 4095"),
]

# (arguments, the start of the stderr line): the service cannot bind its
# socket below a directory that does not exist.
START_FAILURES: list[tuple[list[str], str]] = [
    (["--virtio-console-socket=/nonexistent-directory/console.sock"],
     "Unable to start in-process Virtio console: "),
    (["--virtio-serial-socket=/nonexistent-directory/serial.sock"],
     "Unable to start in-process Virtio serial: "),
]


# Informational runs (--version) that every host device option may join: the
# launcher parses them all and starts nothing.
ACCEPTED: list[list[str]] = [
    ["--gpu-display=vnc", "--gpu-vnc-bind=127.0.0.1", "--gpu-vnc-password=secret",
     "--gpu-vnc-port=0", "--gpu-width=800", "--gpu-height=600", "--gpu-dpi=96",
     "--net-forward=tcp:127.0.0.1:55555-80",
     "--virtio-console-socket={tmp}/console.sock"],
    ["--gpu-display=native", "--gpu-auto-resize=off",
     "--virtio-serial-socket={tmp}/serial.sock"],
    ["--gpu-display=offscreen", "--gpu-offscreen-size=2266x1374",
     "--gpu-offscreen-mirror=800@15", "--gpu-width=2266", "--gpu-height=1374"],
    ["--gpu-display=offscreen", "--gpu-offscreen-mirror=on"],
    ["--gpu-display=offscreen", "--gpu-offscreen-mirror=off"],
    ["--gpu", "--battery", "--audio", "--video", "--video-encoder",
     explicit(BATTERY), explicit(GPU)],
    ["--audio-output=null"],
    ["--audio", "--audio-output=host"],
    ["--audio-input=silent"],
    ["--vsock-forward=127.0.0.1:0-5555", "--vsock-forward=6000-7"],
]


class CaseFailure(RuntimeError):
    pass


def run(
    emulator: pathlib.Path, scratch: pathlib.Path, arguments: list[str]
) -> subprocess.CompletedProcess[str]:
    """One launch, in a $TMPDIR of its own, which it must leave empty."""
    environment = dict(os.environ, TMPDIR=str(scratch))
    environment.pop("UML_DISABLE_NETWORK", None)
    result = subprocess.run(
        [str(emulator), NO_ROOTFS, *arguments],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        env=environment,
        timeout=TIMEOUT_SECONDS,
    )
    leftovers = sorted(path.name for path in scratch.iterdir())
    if leftovers:
        raise CaseFailure(f"{arguments}: the launcher left {leftovers} behind")
    return result


def check(emulator: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="uml-refusals-") as root:
        scratch = pathlib.Path(root) / "tmp"
        scratch.mkdir()
        for arguments, message in REFUSALS:
            arguments = [argument.format(tmp=root) for argument in arguments]
            result = run(emulator, scratch, arguments)
            lines = result.stderr.splitlines()
            if result.returncode != 1 or message not in lines:
                raise CaseFailure(
                    f"{arguments}: status {result.returncode}, expected 1 "
                    f"with {message!r}; stderr:\n{result.stderr}"
                )
        print(f"ok: {len(REFUSALS)} refusals name their reason and exit 1")
        for arguments, start in START_FAILURES:
            result = run(emulator, scratch, arguments)
            if result.returncode != 1 or not any(
                line.startswith(start) for line in result.stderr.splitlines()
            ):
                raise CaseFailure(
                    f"{arguments}: status {result.returncode}, expected 1 "
                    f"with {start!r}; stderr:\n{result.stderr}"
                )
        print("ok: a service that cannot start names its device and exits 1")
        for flag, arguments in itertools.product(("--version", "--help"), ACCEPTED):
            arguments = [argument.format(tmp=root) for argument in arguments]
            result = run(emulator, scratch, [flag, *arguments])
            if result.returncode != 0 or "cannot be combined" in result.stderr:
                raise CaseFailure(
                    f"{[flag, *arguments]}: an informational run failed "
                    f"(status {result.returncode}):\n{result.stderr}"
                )
        print("ok: an informational run parses every host option, starts and refuses no device")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--emulator", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    try:
        check(arguments.emulator.resolve())
    except (CaseFailure, subprocess.TimeoutExpired) as error:
        print(f"launcher refusal check failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
