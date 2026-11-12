# SPDX-License-Identifier: GPL-2.0-or-later
"""The KDE Plasma session launch: options, GPU-harness command, separator.

tools/boot_kde_vnc.py is the interactive command line over this module,
and the WebGL runners build their guests from the same options and command,
appending their own kernel arguments after the separator.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import ipaddress
import os
from pathlib import Path
import sys
from typing import Sequence


REPO_ROOT = Path(__file__).resolve().parents[3]
GUEST_HARNESS = REPO_ROOT / "tests/integration/gpu/run_gpu_guest.py"
DEFAULT_EMULATOR = REPO_ROOT / ".build/presets/macos-integration/bin/uml-darwin"
DEFAULT_ROOTFS = (
    REPO_ROOT
    / ".build/rootfs/starter-20260928-kde-setup/debian-13-plasma-wayland.img"
)
# A starter image is a partitioned ext4 disk that owns its whole session: the
# per-run initramfs starts its own /sbin/init, its getty@tty1 autologin starts
# Plasma, and it configures DHCP itself. A legacy image is the unified KDE
# filesystem, rootfs.erofs, or an ext4 derivative sealed from it: an
# unpartitioned filesystem whose SDDM autologin starts Plasma once the seed
# below has set it up, and whose gettys must stay off the display manager's
# VT. The filesystem type selects the usual profile; runners that accept
# legacy derivatives name the profile explicitly.
IMAGE_PROFILES = ("starter", "legacy")
DEFAULT_IMAGE_PROFILE_BY_TYPE = {
    "erofs": "legacy",
    "ext4": "starter",
}
ROOT_DEVICE_BY_IMAGE_PROFILE = {
    "legacy": "/dev/ubda",
    "starter": "/dev/ubda1",
}
CDROM_DEVICE = "/dev/ubdb"
CDROM_MOUNT_POINT = "/mnt/cdrom"
# The image's systemd mounts the ISO; both image kinds run systemd 257.
ISO_MOUNT_ARGUMENT = (
    f"systemd.mount-extra={CDROM_DEVICE}:{CDROM_MOUNT_POINT}:iso9660:ro"
)
# The legacy image's desktop setup, installed as a payload and run before the
# display manager starts. The desktop image derivation bakes the same file
# into the KDE images it produces.
LEGACY_KDE_SEED = REPO_ROOT / "tools/rootfs/desktop/setup/kde"
LEGACY_KDE_SEED_GUEST_PATH = "/usr/local/libexec/uml-unified-kde-setup"


def ipv4_address(value: str) -> str:
    try:
        return str(ipaddress.IPv4Address(value))
    except ipaddress.AddressValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from error


def positive_integer(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"not an integer: {value}") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def port_number(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("not an integer") from error
    if parsed < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    if parsed > 65535:
        raise argparse.ArgumentTypeError("must be at most 65535")
    return parsed


def vnc_password(value: str) -> str:
    length = len(value.encode("utf-8"))
    if length == 0 or length > 8:
        raise argparse.ArgumentTypeError(
            "must contain between 1 and 8 UTF-8 bytes"
        )
    return value


def iso_image(value: str) -> Path:
    candidate = Path(value).expanduser()
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as error:
        raise argparse.ArgumentTypeError(
            f"cannot access ISO image {candidate}: {error}"
        ) from error
    if not resolved.is_file() or not os.access(resolved, os.R_OK):
        raise argparse.ArgumentTypeError(
            f"not a readable regular file: {resolved}"
        )
    return resolved


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Boot KDE Plasma Wayland with Virgl in a native Metal window "
            "(or an external VNC server) until Ctrl-C"
        )
    )
    parser.add_argument(
        "--display", choices=("native", "vnc"), default="native",
        help="display backend (default: native)")
    parser.add_argument(
        "--bind",
        type=ipv4_address,
        default=None,
        help="VNC IPv4 bind address (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--port", type=port_number, default=None,
        help="VNC TCP port (0 selects a dynamically bound port)")
    parser.add_argument(
        "--password",
        type=vnc_password,
        help=(
            "enable VNC password authentication (the VNC protocol accepts "
            "at most 8 bytes)"
        ),
    )
    parser.add_argument("--width", type=positive_integer, default=1280)
    parser.add_argument("--height", type=positive_integer, default=720)
    parser.add_argument(
        "--dpi",
        type=positive_integer,
        help="advertise an effective display density through Virtio-GPU EDID",
    )
    parser.add_argument("--cpus", type=positive_integer, default=10)
    parser.add_argument(
        "--engine",
        choices=("jit", "gadgets"),
        default="jit",
    )
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--emulator", type=Path, default=DEFAULT_EMULATOR)
    parser.add_argument(
        "--rootfs",
        type=Path,
        default=DEFAULT_ROOTFS,
        help="read-only lower disk (default: codec-enabled Plasma starter)",
    )
    parser.add_argument(
        "--rootfs-type",
        choices=("erofs", "ext4"),
        default="ext4",
        help="lower filesystem type (default: ext4)",
    )
    parser.add_argument(
        "--image-profile",
        choices=IMAGE_PROFILES,
        help=(
            "session, getty, and network policy of the lower image "
            "(default: starter for ext4, legacy for erofs)"
        ),
    )
    parser.add_argument(
        "--root-device",
        help=(
            "guest root device (default: /dev/ubda1 for a starter image, "
            "/dev/ubda for a legacy image)"
        ),
    )
    parser.add_argument(
        "--iso",
        type=iso_image,
        help=(
            "attach an ISO image read-only and mount it at /mnt/cdrom in "
            "the guest"
        ),
    )
    parser.add_argument(
        "--serial-socket",
        type=Path,
        help=(
            "expose ttyS0 at this Unix-domain socket for an attachable "
            "interactive console"
        ),
    )
    parser.add_argument(
        "--virtio-console-socket",
        type=Path,
        help=(
            "attach guest hvc0 at this reconnectable Unix-domain socket"
        ),
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the emulator command without booting",
    )
    options = parser.parse_args(arguments)
    if options.display != "vnc" and any(
        value is not None
        for value in (options.bind, options.port, options.password)
    ):
        parser.error("--bind, --port, and --password require --display vnc")
    if options.bind is None:
        options.bind = "127.0.0.1"
    if options.port is None:
        options.port = 5900
    if options.image_profile is None:
        options.image_profile = DEFAULT_IMAGE_PROFILE_BY_TYPE[
            options.rootfs_type
        ]
    if options.image_profile == "starter" and options.rootfs_type != "ext4":
        parser.error("a starter image is an ext4 disk")
    if options.root_device is None:
        options.root_device = ROOT_DEVICE_BY_IMAGE_PROFILE[
            options.image_profile
        ]
    if (
        options.serial_socket is not None
        and options.virtio_console_socket is not None
        and options.serial_socket.resolve()
        == options.virtio_console_socket.resolve()
    ):
        parser.error("serial and Virtio console sockets must be different")
    return options


def build_harness_command(options: argparse.Namespace) -> list[str]:
    command = [
        sys.executable,
        str(GUEST_HARNESS),
        "--emulator",
        str(options.emulator),
        "--rootfs",
        str(options.rootfs),
        "--rootfs-type",
        options.rootfs_type,
        "--root-device",
        options.root_device,
        "--memory",
        options.memory,
        "--unit",
        "graphical.target",
        "--timeout",
        "0",
        "--no-auto-poweroff",
    ]
    if options.image_profile == "starter":
        command.append("--image-init")
    else:
        command.extend((
            "--guest-file",
            f"{LEGACY_KDE_SEED_GUEST_PATH}={LEGACY_KDE_SEED}",
            "--command",
            LEGACY_KDE_SEED_GUEST_PATH,
        ))
    if options.serial_socket is not None:
        command.extend(("--serial-socket", str(options.serial_socket)))
    if options.virtio_console_socket is not None:
        command.extend(
            ("--virtio-console-socket", str(options.virtio_console_socket))
        )
    if options.dry_run:
        command.append("--dry-run")
    command.extend((
        "--",
        f"ncpus={options.cpus}",
        f"--execution-engine={options.engine}",
    ))
    if options.image_profile == "legacy":
        # Legacy SDDM images must keep a getty off the display manager's VT
        # from the start of boot, as their seed explains. Their getty@tty0
        # autologin otherwise opens the VT that SDDM has just given the Plasma
        # session and hangs it up. A starter image's Plasma session starts
        # from its getty@tty1 autologin, so its getties stay unmasked.
        command.extend((
            "systemd.mask=getty@tty0.service",
            "systemd.mask=getty@tty1.service",
        ))
    if options.serial_socket is not None:
        command.append("systemd.wants=serial-getty@ttyS0.service")
    if options.virtio_console_socket is None:
        command.append("systemd.mask=serial-getty@hvc0.service")
    else:
        command.append("systemd.wants=serial-getty@hvc0.service")
    if options.iso is not None:
        command.extend((f"ubd1r={options.iso}", ISO_MOUNT_ARGUMENT))
    command.extend(("--audio", "--gpu", f"--gpu-display={options.display}"))
    if options.display == "vnc":
        command.extend((
            f"--gpu-vnc-bind={options.bind}",
            f"--gpu-vnc-port={options.port}",
        ))
        if options.password is not None:
            command.append(f"--gpu-vnc-password={options.password}")
    command.extend(
        (
            f"--gpu-width={options.width}",
            f"--gpu-height={options.height}",
        )
    )
    if options.dpi is not None:
        command.append(f"--gpu-dpi={options.dpi}")
    return command


def guest_argument_separator(command: Sequence[str]) -> int:
    """Locate the GPU harness kernel-argument separator."""
    separators = [index for index, argument in enumerate(command) if argument == "--"]
    if not separators:
        raise ValueError("GPU harness command has no kernel-argument separator")
    return separators[-1]
