# SPDX-License-Identifier: GPL-2.0-or-later
"""Host side of a Kernarium automation run.

Only the automation build of the Kernarium Linux runtime boots a host payload
(Kernarium/README.md, "Automation runs"). ``KERNARIUM_BOOT_PAYLOAD`` names a
newc segment in the application's Documents directory, which the runtime
appends after its own segment, and ``KERNARIUM_SYSTEMD_UNIT`` names the
systemd target the guest boots. This module writes such segments with
``uml-initramfs`` and recognises the automation build; installing, copying
and launching on a connected device is tests/integration/support/ios_device.py,
which takes ``APPLICATION`` for Kernarium's bundle and console.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import hashlib
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from collections.abc import Mapping, Sequence

import guest
import ios_device


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
BUNDLE_ID = "com.imxieyi.Kernarium"
DEFAULT_INITRAMFS_TOOL = (
    REPOSITORY / ".build" / "presets" / "macos-integration" / "bin"
    / "uml-initramfs"
)
# The runtime refuses a larger payload: the kernel copies the whole initrd
# into boot memory before it unpacks it.
PAYLOAD_LIMIT = 64 << 20
NEWC_MAGIC = b"070701"
# Only the automation runtime reads the payload setting, so its name in a
# framework binary identifies that build.
AUTOMATION_SYMBOL = b"KERNARIUM_BOOT_PAYLOAD"
AUTOMATION_BOOT_LINE = "Kernarium: automation boot with a "
PAYLOAD_REMOVED_LINE = "Kernarium: removed the boot payload Documents/"
RUNTIME_RESULT = re.compile(r"Kernarium: runtime returned (-?[0-9]+)")
SESSION_EXIT = re.compile(r"UML_SESSION_EXIT:([0-9]+)")
SYSTEMD_TARGET = re.compile(r"[A-Za-z0-9@_.:-]{1,121}\.target")
LINUX_RUNTIME_FRAMEWORKS = ("KRuntime_6_18_4K",)
WORKLOAD_GUEST_PATH = "/usr/local/libexec/uml-workload"
# What the device plumbing needs to know about Kernarium: a launch clears the
# inherited KERNARIUM_ settings, and a run ends at the runtime-result line.
APPLICATION = ios_device.Application(
    BUNDLE_ID, ("KERNARIUM_",), RUNTIME_RESULT
)


class KernariumError(RuntimeError):
    pass


def root_type(image_name: str) -> str:
    """The root filesystem type the runtime infers from an image's name."""
    suffix = pathlib.PurePosixPath(image_name).suffix.lower()
    if suffix in (".erofs", ".ext4"):
        return suffix[1:]
    raise KernariumError(f"not an .erofs or .ext4 image name: {image_name}")


def boot_parameters(
    image_type: str, *, upper: str = "ext4", entry: str = "/sbin/init"
) -> str:
    """A ``/boot.env`` for the runtime's disks.

    The runtime's own boots the image read-only on ubd0 beneath a fresh ext4
    writable layer on ubd1, the overlay disk, and then the image's init. A
    payload that brings its own replaces that file; ``upper="tmpfs"`` leaves
    the overlay disk to the workload.
    """
    if image_type not in ("erofs", "ext4") or upper not in ("ext4", "tmpfs"):
        raise KernariumError("unsupported root type or writable layer")
    lines = [
        "UML_BOOT_ROOT=/dev/ubda",
        f"UML_BOOT_ROOT_TYPE={image_type}",
        f"UML_BOOT_UPPER={upper}",
        *(["UML_BOOT_UPPER_DEVICE=/dev/ubdb"] if upper == "ext4" else []),
        f"UML_BOOT_ENTRY={entry}",
    ]
    return "".join(f"{line}\n" for line in lines)


class Payload:
    """The files one automation run adds to the guest's new root."""

    def __init__(self) -> None:
        self._files: dict[str, guest.GuestFile] = {}

    def add(
        self, guest_path: str, contents: bytes | str, mode: int = 0o644
    ) -> None:
        guest.validate_payload_path(guest_path)
        data = contents.encode() if isinstance(contents, str) else contents
        archive_path = f"/payload{guest_path}"
        if archive_path in self._files:
            raise KernariumError(f"the payload already holds {guest_path}")
        self._files[archive_path] = guest.GuestFile(data, mode)

    def set_boot_parameters(self, parameters: str) -> None:
        """Replace the runtime's ``/boot.env``."""
        self._files["/boot.env"] = guest.GuestFile(parameters.encode())

    @property
    def files(self) -> Mapping[str, guest.GuestFile]:
        """Archive paths and contents, ``/payload/...`` and ``/boot.env``."""
        return dict(self._files)

    def write(
        self,
        output: pathlib.Path,
        tool: pathlib.Path = DEFAULT_INITRAMFS_TOOL,
    ) -> pathlib.Path:
        """Write the segment to the new file ``output`` and return it."""
        if not self._files:
            raise KernariumError("an empty payload changes nothing")
        if not tool.is_file() or not os.access(tool, os.X_OK):
            raise KernariumError(
                f"uml-initramfs is unavailable: {tool}; build the "
                "uml-initramfs target or pass its path"
            )
        with tempfile.TemporaryDirectory(prefix="kernarium-payload-") as name:
            directory = pathlib.Path(name)
            arguments = [str(tool), f"--output={output}"]
            for index, (archive_path, file) in enumerate(
                sorted(self._files.items())
            ):
                host = directory / f"{index:04d}"
                host.write_bytes(file.contents)
                host.chmod(file.mode)
                arguments.append(f"--file={archive_path}={host}")
            subprocess.run(arguments, check=True, stdin=subprocess.DEVNULL)
        size = output.stat().st_size
        with output.open("rb") as handle:
            magic = handle.read(len(NEWC_MAGIC))
        if magic != NEWC_MAGIC or size % 4 or size > PAYLOAD_LIMIT:
            raise KernariumError(
                f"{output} is not a padded newc segment of at most 64 MiB"
            )
        return output


def payload_name(path: pathlib.Path) -> str:
    """A Documents name that changes whenever the payload does.

    The device lanes never replace a file on a device, so equal names mean
    equal contents and a payload already there is reused.
    """
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return f"kernarium-payload-{digest[:24]}.cpio"


def workload_payload(
    image_type: str,
    workload: bytes,
    *,
    files: Mapping[str, guest.GuestFile] | None = None,
    upper: str = "ext4",
) -> Payload:
    """A payload that runs one guest script and powers the guest off.

    The harness session (tools/rootfs/boot-ramdisk/session) runs the script
    as a child of PID 1 with the runtime filesystems a shell needs, prints
    ``UML_SESSION_EXIT:<status>`` and powers off; nothing is typed, so every
    line the script prints is its own.
    """
    payload = Payload()
    payload.set_boot_parameters(
        boot_parameters(
            image_type, upper=upper, entry=guest.SESSION_GUEST_PATH
        )
    )
    payload.add(
        guest.SESSION_GUEST_PATH, guest.SESSION_SCRIPT.read_bytes(), 0o755
    )
    payload.add(
        guest.SESSION_PARAMETERS_GUEST_PATH,
        f"UML_SESSION_ENTRY={WORKLOAD_GUEST_PATH}\n"
        "UML_SESSION_RUNTIME_MOUNTS=1\n",
    )
    payload.add(WORKLOAD_GUEST_PATH, workload, 0o755)
    for guest_path, file in sorted((files or {}).items()):
        payload.add(guest_path, file.contents, file.mode)
    return payload


def validate_systemd_target(unit: str) -> str:
    if SYSTEMD_TARGET.fullmatch(unit) is None:
        raise KernariumError(f"not a systemd target the runtime boots: {unit}")
    return unit


def is_automation_binary(binary: pathlib.Path) -> bool:
    return AUTOMATION_SYMBOL in binary.read_bytes()


def require_automation_app(app: pathlib.Path) -> None:
    """Refuse an application whose Linux runtimes ignore payloads."""
    for name in LINUX_RUNTIME_FRAMEWORKS:
        binary = app / "Frameworks" / f"{name}.framework" / name
        if not binary.is_file():
            raise KernariumError(f"{app} carries no {name} runtime")
        if not is_automation_binary(binary):
            raise KernariumError(
                f"{binary} is the release runtime; package the "
                "kernarium-ios-device-automation presets with "
                "Kernarium/scripts/package-ios-frameworks.sh --automation"
            )


def require_automation_boot(transcript: str) -> None:
    """Refuse a run whose application booted without the payload, or whose
    kernel turned a host fast path off."""
    if AUTOMATION_BOOT_LINE not in transcript:
        raise KernariumError(
            "the application did not report an automation boot; the "
            "installed build is the release runtime, which ignores payloads"
        )
    try:
        guest.require_host_fast_paths(transcript)
    except RuntimeError as error:
        raise KernariumError(str(error)) from None


def report_payload_removal(transcript: str, name: str) -> None:
    """Warn when a launch that asked for the payload's removal left it.

    A run is not judged by its cleanup, but a runtime that predates the
    setting, or a refused unlink, leaves one payload per run on the device.
    """
    if PAYLOAD_REMOVED_LINE + name not in transcript:
        print(
            f"warning: the runtime did not remove Documents/{name}; see "
            "the console for why",
            file=sys.stderr,
            flush=True,
        )


def runtime_result(transcript: str) -> int | None:
    return ios_device.runtime_result(APPLICATION, transcript)


def session_exit(transcript: str) -> int | None:
    results = SESSION_EXIT.findall(transcript)
    return int(results[-1]) if results else None


def settings_for(
    *,
    rootfs: str,
    payload: str | None,
    unit: str | None = None,
    cpus: int,
    memory_mib: int,
    engine: str,
    remove_payload: bool = False,
    extra: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """The application settings for one run, without the child prefix.

    ``remove_payload`` makes the runtime delete the payload once it has read
    it, so only a launch that is the payload's one use may ask: a device
    cannot remove a file for the host, and a second launch would find none.
    """
    settings = {
        "KERNARIUM_ROOTFS": rootfs,
        "KERNARIUM_VCPU_COUNT": str(cpus),
        "KERNARIUM_GUEST_MEMORY_MIB": str(memory_mib),
        "KERNARIUM_EXECUTION_ENGINE": engine,
    }
    if remove_payload and payload is None:
        raise KernariumError("only a boot payload can be removed")
    if payload is not None:
        settings["KERNARIUM_BOOT_PAYLOAD"] = payload
    if remove_payload:
        settings["KERNARIUM_PAYLOAD_REMOVE"] = "1"
    if unit is not None:
        settings["KERNARIUM_SYSTEMD_UNIT"] = validate_systemd_target(unit)
    settings.update(extra or {})
    return settings


def parse_guest_file(value: str) -> tuple[str, guest.GuestFile]:
    """``GUEST=HOST``: a host file installed at a guest path, mode kept."""
    guest_path, separator, host = value.partition("=")
    if not separator or not host:
        raise KernariumError(f"expected GUEST=HOST, not {value!r}")
    source = pathlib.Path(host)
    mode = source.stat().st_mode & 0o777
    return guest_path, guest.GuestFile(source.read_bytes(), mode)


def guest_files(values: Sequence[str]) -> dict[str, guest.GuestFile]:
    files: dict[str, guest.GuestFile] = {}
    for value in values:
        guest_path, file = parse_guest_file(value)
        if guest_path in files:
            raise KernariumError(f"{guest_path} is named twice")
        files[guest_path] = file
    return files
