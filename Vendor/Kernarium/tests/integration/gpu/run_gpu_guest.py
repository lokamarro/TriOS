#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the GPU test guest from a read-only lower filesystem plus tmpfs.

The launcher's per-run initramfs mounts the image beneath a tmpfs overlay,
installs this run's files and hands PID 1 to the harness session, which
prepares the resolver and container state and execs the image's systemd.
systemd starts the test unit through ``systemd.wants=``; nothing is typed.
With ``--image-init`` the initramfs starts the image's own ``/sbin/init``
instead, for an image that owns its whole session.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
from dataclasses import dataclass, field
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import pty
import select
import shlex
import signal
import socket
import stat
import subprocess
import sys
import tarfile
import time
import tty
from typing import Any, BinaryIO, Mapping, Sequence

import guest_process

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
from guest import (  # noqa: E402
    SESSION_GUEST_PATH,
    SESSION_PARAMETERS_GUEST_PATH,
    SESSION_SCRIPT,
    remove_emulator_run_directories,
    validate_payload_path,
)


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_EMULATOR = REPO_ROOT / ".build/presets/macos-integration/bin/uml-darwin"
DEFAULT_ROOTFS = REPO_ROOT / "rootfs.erofs"
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/gpu-test-artifacts"
PASS_MARKER = "UML_GPU_OVERLAY_TEST: PASS"
FAIL_MARKER = "UML_GPU_OVERLAY_TEST: FAIL"
DOCKER_HELLO_WORLD_IMAGE = "uml/hello-world:local"
MAX_DOCKER_HELLO_WORLD_SIZE = 1024 * 1024
MAX_SERIAL_CLIENT_BACKLOG = 1024 * 1024
MAX_SERIAL_INPUT_BACKLOG = 1024 * 1024
TEST_SCRIPT_GUEST_PATH = "/usr/local/libexec/uml-gpu-overlay-test"
TEST_UNIT = "uml-gpu-overlay-test.service"
TEST_UNIT_GUEST_PATH = f"/etc/systemd/system/{TEST_UNIT}"
DOCKER_ARCHIVE_GUEST_PATH = "/usr/local/libexec/uml-docker-hello-world.tar"
BOOT_PARAMETERS_GUEST_PATH = "/boot.env"
IMAGE_INIT = "/sbin/init"
# Where a caller stages the files it generates, beside its own artifacts.
GUEST_FILE_DIRECTORY = "guest-files"
# Every arm64 Linux guest has 4 KiB pages; the CTS launchers refuse any other.
GUEST_PAGE_SIZE = 4096
# Guest paths this runner writes itself; a caller's file may not replace one.
RUNNER_GUEST_PATHS = (
    BOOT_PARAMETERS_GUEST_PATH,
    SESSION_GUEST_PATH,
    SESSION_PARAMETERS_GUEST_PATH,
    TEST_SCRIPT_GUEST_PATH,
    TEST_UNIT_GUEST_PATH,
    DOCKER_ARCHIVE_GUEST_PATH,
)
TEST_UNIT_TEXT = f"""[Unit]
Description=Darwin UML read-only-root GPU test guard
After=local-fs.target
Before=display-manager.service

[Service]
Type=oneshot
ExecStart={TEST_SCRIPT_GUEST_PATH}
RemainAfterExit=yes
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console
"""
PROTECTED_ARGUMENTS = (
    "ubd0",
    "root=",
    "rootfstype=",
    "init=",
    "initrd=",
    "mem=",
    "console=",
    "systemd.unit=",
    "--guest-file=",
    "--guest-init=",
    "--virtio-console-socket=",
    "--virtio-serial-socket=",
)
ROOTFS_TYPES = ("erofs", "ext4")


class SafetyError(RuntimeError):
    """Raised when a boot could risk modifying the base image."""


@dataclass
class StagedGuestFiles:
    """Generated files a caller hands this runner through ``--guest-file``.

    Each is named by its SHA-256 and mode, so the command that names it is
    known before anything is written and changes whenever the file does.
    ``write`` puts the files in place just before that command runs.
    """

    directory: Path
    files: dict[Path, tuple[bytes, int]] = field(default_factory=dict)
    arguments: list[str] = field(default_factory=list)

    def add(self, guest_path: str, content: str | bytes, mode: int = 0o644) -> None:
        try:
            validate_payload_path(guest_path)
        except ValueError as error:
            raise SafetyError(str(error)) from error
        if guest_path in RUNNER_GUEST_PATHS:
            raise SafetyError(f"guest file replaces the runner's own {guest_path}")
        data = content.encode("utf-8") if isinstance(content, str) else content
        digest = hashlib.sha256(data).hexdigest()
        host = self.directory / (
            f"{digest}-{mode:04o}-{PurePosixPath(guest_path).name}"
        )
        self.files[host] = (data, mode)
        self.arguments.append(f"--guest-file={guest_path}={host}")

    def write(self) -> None:
        for host, (data, mode) in self.files.items():
            host.parent.mkdir(parents=True, exist_ok=True)
            temporary = host.with_name(f".{host.name}.{os.getpid()}")
            temporary.write_bytes(data)
            temporary.chmod(mode)
            os.replace(temporary, host)


class SerialConsoleSocket:
    """One reconnectable Unix client for the runner-owned serial stream."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.client: socket.socket | None = None
        self.output = bytearray()
        self._identity: tuple[int, int] | None = None
        if path.exists() or path.is_symlink():
            self.listener.close()
            raise SafetyError(f"serial socket path already exists: {path}")
        if not path.parent.is_dir():
            self.listener.close()
            raise SafetyError(
                f"serial socket parent is not a directory: {path.parent}"
            )
        try:
            self.listener.bind(str(path))
            details = path.lstat()
            self._identity = (details.st_dev, details.st_ino)
            path.chmod(0o600)
            self.listener.listen(1)
            self.listener.setblocking(False)
        except OSError as error:
            self.close()
            raise SafetyError(f"cannot create serial socket {path}: {error}") from error

    def close_client(self) -> None:
        if self.client is not None:
            self.client.close()
            self.client = None
        self.output.clear()

    def accept(self, history: bytes) -> None:
        try:
            client, _ = self.listener.accept()
        except BlockingIOError:
            return
        client.setblocking(False)
        self.close_client()
        self.client = client
        self.output.extend(history)

    def read_input(self) -> bytes:
        if self.client is None:
            return b""
        try:
            data = self.client.recv(65536)
        except BlockingIOError:
            return b""
        except OSError:
            self.close_client()
            return b""
        if not data:
            self.close_client()
            return b""
        return data

    def queue_output(self, data: bytes) -> None:
        if self.client is None:
            return
        if len(self.output) + len(data) > MAX_SERIAL_CLIENT_BACKLOG:
            self.close_client()
            return
        self.output.extend(data)

    def flush_output(self) -> None:
        if self.client is None or not self.output:
            return
        try:
            sent = self.client.send(self.output)
        except BlockingIOError:
            return
        except OSError:
            self.close_client()
            return
        if sent > 0:
            del self.output[:sent]

    def close(self) -> None:
        self.close_client()
        self.listener.close()
        if self._identity is None:
            return
        try:
            details = self.path.lstat()
        except FileNotFoundError:
            return
        if (
            stat.S_ISSOCK(details.st_mode)
            and (details.st_dev, details.st_ino) == self._identity
        ):
            self.path.unlink()

    def __enter__(self) -> SerialConsoleSocket:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def validate_extra_arguments(arguments: Sequence[str]) -> None:
    for argument in arguments:
        if any(argument.startswith(prefix) for prefix in PROTECTED_ARGUMENTS):
            raise SafetyError(f"kernel argument is controlled by the harness: {argument}")


def validate_rootfs_path(path: Path, rootfs_type: str) -> None:
    if rootfs_type not in ROOTFS_TYPES:
        raise SafetyError(f"unsupported root filesystem type: {rootfs_type}")
    if not path.is_file():
        raise SafetyError(f"rootfs is not a regular file: {path}")
    if rootfs_type == "ext4" and path.stat().st_mode & (
        stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH
    ):
        raise SafetyError(
            "derivative ext4 rootfs still has host write permission"
        )


def validate_console_socket_paths(
    serial_socket: Path | None,
    virtio_console_socket: Path | None,
    virtio_serial_socket: Path | None = None,
) -> None:
    paths = [
        path
        for path in (serial_socket, virtio_console_socket, virtio_serial_socket)
        if path is not None
    ]
    if len(set(paths)) != len(paths):
        raise SafetyError("serial and Virtio console sockets must be different")


def load_docker_hello_world(path: Path) -> bytes:
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise SafetyError(
            f"cannot read Docker hello-world payload {path}: {error}"
        ) from error
    if not payload.startswith(b"\x7fELF"):
        raise SafetyError(f"Docker hello-world payload is not an ELF file: {path}")
    if not payload or len(payload) > MAX_DOCKER_HELLO_WORLD_SIZE:
        raise SafetyError(
            "Docker hello-world payload has an invalid size: "
            f"{len(payload)} bytes"
        )
    return payload


def make_docker_rootfs_archive(payload: bytes) -> bytes:
    archive_stream = io.BytesIO()
    member = tarfile.TarInfo("hello")
    member.mode = 0o755
    member.uid = 0
    member.gid = 0
    member.uname = "root"
    member.gname = "root"
    member.mtime = int(time.time())
    member.size = len(payload)
    with tarfile.open(
        fileobj=archive_stream,
        mode="w",
        format=tarfile.USTAR_FORMAT,
    ) as archive:
        archive.addfile(member, io.BytesIO(payload))
    return archive_stream.getvalue()


def make_docker_hello_world_smoke() -> str:
    """The offline Docker smoke over the archive installed as a guest file."""
    return f"""docker_hello_world_image='{DOCKER_HELLO_WORLD_IMAGE}'
docker_hello_world_container='uml-hello-world-smoke'
docker_hello_world_archive='{DOCKER_ARCHIVE_GUEST_PATH}'
docker_hello_world_output='/run/uml-docker-hello-world.out'
docker_hello_world_cleanup()
{{
    timeout 10 docker rm -f \\
        "$docker_hello_world_container" >/dev/null 2>&1 || true
    timeout 10 docker image rm -f \\
        "$docker_hello_world_image" >/dev/null 2>&1 || true
    rm -f "$docker_hello_world_output"
}}
trap docker_hello_world_cleanup EXIT HUP INT TERM
timeout 30 docker import \\
    "$docker_hello_world_archive" "$docker_hello_world_image" >/dev/null
docker image inspect "$docker_hello_world_image" >/dev/null
if ! timeout 30 docker run \\
    --name "$docker_hello_world_container" \\
    --rm --pull=never --network none \\
    "$docker_hello_world_image" /hello > "$docker_hello_world_output" 2>&1
then
    cat "$docker_hello_world_output"
    exit 1
fi
grep -Fxq 'Hello from Docker!' "$docker_hello_world_output"
cat "$docker_hello_world_output"
trap - EXIT HUP INT TERM
docker_hello_world_cleanup"""


def parse_guest_file(value: str) -> tuple[str, Path]:
    """Read a caller's ``GUEST_PATH=HOST_PATH`` file for the guest."""
    guest_path, separator, host = value.partition("=")
    if not separator or not host:
        raise SafetyError(f"guest file must be GUEST_PATH=HOST_PATH: {value}")
    try:
        validate_payload_path(guest_path)
    except ValueError as error:
        raise SafetyError(str(error)) from error
    if guest_path in RUNNER_GUEST_PATHS:
        raise SafetyError(f"guest file replaces the runner's own {guest_path}")
    host_path = Path(host).resolve()
    if not host_path.is_file():
        raise SafetyError(f"guest file source is not a regular file: {host_path}")
    return guest_path, host_path


def boot_parameters(
    root_device: str, rootfs_type: str, entry: str = SESSION_GUEST_PATH
) -> str:
    """The per-run /boot.env: the image beneath tmpfs, then ``entry``."""
    if rootfs_type not in ROOTFS_TYPES:
        raise SafetyError(f"unsupported root filesystem type: {rootfs_type}")
    if not root_device.startswith("/dev/") or any(
        character.isspace() for character in root_device
    ):
        raise SafetyError(f"root device must be a /dev path: {root_device}")
    return (
        f"UML_BOOT_ROOT={root_device}\n"
        f"UML_BOOT_ROOT_TYPE={rootfs_type}\n"
        "UML_BOOT_UPPER=tmpfs\n"
        f"UML_BOOT_ENTRY={entry}\n"
    )


# The session hands PID 1 to the image's systemd, keeping container state out
# of the overlay's upper layer and the SLiRP resolver in /etc/resolv.conf.
SESSION_PARAMETERS = (
    "UML_SESSION_ENTRY=systemd\n"
    "UML_SESSION_RESOLVER=user-network\n"
    "UML_SESSION_CONTAINER_STATE=tmpfs\n"
)


def runner_guest_files(
    test_script: str | None,
    root_device: str,
    rootfs_type: str,
    docker_hello_world: bytes | None = None,
) -> dict[str, tuple[bytes, int]]:
    """The files this runner writes for one boot, by guest path.

    Without a test script the image's own init is the entry and the boot
    parameters are the only file."""
    if test_script is None:
        if docker_hello_world is not None:
            raise SafetyError("the Docker smoke needs the test session")
        return {
            BOOT_PARAMETERS_GUEST_PATH: (
                boot_parameters(root_device, rootfs_type, IMAGE_INIT).encode(),
                0o644,
            )
        }
    files = {
        BOOT_PARAMETERS_GUEST_PATH: (
            boot_parameters(root_device, rootfs_type).encode(), 0o644
        ),
        SESSION_PARAMETERS_GUEST_PATH: (SESSION_PARAMETERS.encode(), 0o644),
        TEST_SCRIPT_GUEST_PATH: (test_script.encode(), 0o755),
        TEST_UNIT_GUEST_PATH: (TEST_UNIT_TEXT.encode(), 0o644),
    }
    if docker_hello_world is not None:
        files[DOCKER_ARCHIVE_GUEST_PATH] = (
            make_docker_rootfs_archive(docker_hello_world), 0o644
        )
    return files


def runner_file_name(guest_path: str) -> str:
    """The host name of one of this runner's boot files."""
    if guest_path == BOOT_PARAMETERS_GUEST_PATH:
        return "boot.env"
    return Path(guest_path).name


def guest_file_arguments(
    directory: Path,
    files: Mapping[str, tuple[bytes, int]],
    caller_files: Sequence[tuple[str, Path]] = (),
) -> list[tuple[str, Path]]:
    """Pair every boot file's archive path with its host copy, writing nothing.

    This runner's files live in ``directory``; the session script comes from
    the checkout unchanged and a caller's files from where the caller put
    them. Archive paths are ``/boot.env`` or ``/payload`` plus the guest path.
    """
    pairs = (
        [(f"/payload{SESSION_GUEST_PATH}", SESSION_SCRIPT)]
        if TEST_SCRIPT_GUEST_PATH in files
        else []
    )
    for guest_path in files:
        archive = (
            guest_path
            if guest_path == BOOT_PARAMETERS_GUEST_PATH
            else f"/payload{guest_path}"
        )
        pairs.append((archive, directory / runner_file_name(guest_path)))
    guest_paths = [guest for guest, _ in caller_files]
    if len(set(guest_paths)) != len(guest_paths):
        raise SafetyError("a guest file is named twice")
    pairs.extend((f"/payload{guest}", host) for guest, host in caller_files)
    return pairs


def write_guest_files(
    directory: Path, files: Mapping[str, tuple[bytes, int]]
) -> None:
    """Write this runner's boot files where guest_file_arguments names them."""
    directory.mkdir(parents=True, exist_ok=True)
    for guest_path, (contents, mode) in files.items():
        host = directory / runner_file_name(guest_path)
        host.write_bytes(contents)
        host.chmod(mode)


def build_command(
    emulator: Path,
    rootfs: Path,
    memory: str,
    systemd_unit: str,
    extra_arguments: Sequence[str],
    guest_files: Sequence[tuple[str, Path]],
    rootfs_type: str = "erofs",
    virtio_console_socket: Path | None = None,
    virtio_serial_socket: Path | None = None,
) -> list[str]:
    """The launcher command; ``guest_files`` pairs archive and host paths."""
    validate_extra_arguments(extra_arguments)
    if rootfs_type not in ROOTFS_TYPES:
        raise SafetyError(f"unsupported root filesystem type: {rootfs_type}")
    if not any(guest == BOOT_PARAMETERS_GUEST_PATH for guest, _ in guest_files):
        raise SafetyError("the guest files carry no /boot.env")
    test_session = any(
        guest == f"/payload{TEST_UNIT_GUEST_PATH}" for guest, _ in guest_files
    )
    ubd_flags = "c" if rootfs_type == "erofs" else "cr"
    return [
        str(emulator.resolve()),
        *(f"--guest-file={guest}={host}" for guest, host in guest_files),
        f"ubd0{ubd_flags}={rootfs.resolve()}",
        f"mem={memory}",
        "console=tty0",
        "console=ttyS0",
        f"systemd.unit={systemd_unit}",
        *([f"systemd.wants={TEST_UNIT}"] if test_session else []),
        *extra_arguments,
        *(
            [f"--virtio-console-socket={virtio_console_socket.resolve()}"]
            if virtio_console_socket is not None
            else []
        ),
        *(
            [f"--virtio-serial-socket={virtio_serial_socket.resolve()}"]
            if virtio_serial_socket is not None
            else []
        ),
    ]


def make_test_script(
    commands: Sequence[str],
    auto_poweroff: bool,
    docker_hello_world: bool = False,
    lower_filesystem_type: str = "erofs",
    root_device: str = "/dev/ubda",
) -> str:
    if lower_filesystem_type not in ROOTFS_TYPES:
        raise SafetyError(
            f"unsupported root filesystem type: {lower_filesystem_type}"
        )
    command_parts = list(commands)
    if docker_hello_world:
        command_parts.append(make_docker_hello_world_smoke())
    command_block = "\n".join(command_parts)
    poweroff_block = """
echo "UML_GPU_OVERLAY_TEST: requesting normal poweroff"
# Ask systemd to enter poweroff.target without waiting for wall(1) delivery.
# SIGRTMIN+4 is systemd's documented orderly-poweroff request and avoids
# leaving a test guest running if its D-Bus request path is unhealthy.
timeout 10 systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1
""" if auto_poweroff else """
echo "UML_GPU_OVERLAY_TEST: guest left running by request"
"""
    block_read_only_check = ""
    if lower_filesystem_type == "ext4":
        block_read_only_check = (
            f'check_equal "$(blockdev --getro {shlex.quote(root_device)})" '
            '1 "lower block-device read-only state"\n'
        )
    return f"""#!/bin/sh
set -u
status=0
exec </dev/null >/dev/console 2>&1

check_equal()
{{
    actual="$1"
    expected="$2"
    description="$3"
    if [ "$actual" != "$expected" ]; then
        echo "overlay check failed: $description: $actual != $expected"
        status=1
    fi
}}

check_equal "$(findmnt -n -o FSTYPE -T /)" overlay "root filesystem type"
case ",$(findmnt -n -o OPTIONS -T /)," in
    *,rw,*) ;;
    *) echo "overlay check failed: root overlay is not writable"; status=1 ;;
esac
check_equal "$(findmnt -n -o FSTYPE -T /.uml-lower)" {lower_filesystem_type} "lower filesystem type"
case ",$(findmnt -n -o OPTIONS -T /.uml-lower)," in
    *,ro,*) ;;
    *) echo "overlay check failed: lower filesystem is not read-only"; status=1 ;;
esac
{block_read_only_check}check_equal "$(findmnt -n -o FSTYPE -T /.uml-state)" tmpfs "upper backing type"
check_equal "$(findmnt -n -o FSTYPE -T /var/lib/docker)" tmpfs "Docker state filesystem type"
check_equal "$(findmnt -n -o FSTYPE -T /var/lib/containerd)" tmpfs "containerd state filesystem type"

probe="/etc/.uml-overlay-write-probe"
if ! : > "$probe"; then
    echo "overlay check failed: upper layer is not writable"
    status=1
fi
rm -f "$probe"

if [ "$status" -eq 0 ]; then
    # Keep command-level shell failures (including expansion errors caused by
    # malformed test input) inside a subshell so the overlay-root result and
    # orderly-poweroff footer always run.
    (
        set -e
{command_block if command_block else "        :"}
    )
    status=$?
fi

if [ "$status" -eq 0 ]; then
    echo "{PASS_MARKER}"
else
    echo "{FAIL_MARKER} status=$status"
fi
{poweroff_block}
exit "$status"
"""


def terminate_process(process: subprocess.Popen[bytes]) -> None:
    """Stop the emulator and remove the run directories that one ended with
    SIGKILL could not."""
    guest_process.stop_process(process, guest_process.EMULATOR_STOP)
    remove_emulator_run_directories(process.pid)


def write_pending(stream: BinaryIO, pending: bytearray) -> None:
    """Drain queued serial-client input into a nonblocking tty."""
    while pending:
        try:
            written = stream.write(pending)
        except InterruptedError:
            continue
        except BlockingIOError:
            return
        if written is None:
            return
        if written <= 0:
            raise RuntimeError("guest console stopped accepting input")
        del pending[:written]


def run_guest(
    command: Sequence[str],
    console_log: Path,
    timeout: float,
    expect_poweroff: bool,
    serial_socket: Path | None = None,
) -> tuple[int, bool, bool]:
    artifact_socket = guest_process.SocketOwnership(command)
    serial = (
        SerialConsoleSocket(serial_socket) if serial_socket is not None else None
    )
    try:
        master, slave = pty.openpty()
    except BaseException:
        if serial is not None:
            serial.close()
        raise
    try:
        tty.setraw(slave)
    except BaseException:
        os.close(master)
        os.close(slave)
        if serial is not None:
            serial.close()
        raise
    console = None

    def stop_emulator(process: subprocess.Popen[bytes]) -> None:
        try:
            artifact_socket.observe()
        finally:
            terminate_process(process)

    def close_resources() -> None:
        try:
            if serial is not None:
                serial.close()
        finally:
            try:
                if console is not None:
                    console.close()
                else:
                    os.close(master)
            finally:
                try:
                    if slave is not None:
                        os.close(slave)
                finally:
                    artifact_socket.close()

    with guest_process.managed_child(
        command, stop=stop_emulator, cleanup=close_resources,
        journal_setting=os.environ.get(guest_process.OWNERSHIP_ENV),
        stdin=slave, stdout=slave, stderr=slave, bufsize=0, close_fds=True,
    ) as process:
        # The terminal end stays open until cleanup, so the emulator's exit
        # is never its last close: Darwin discards a terminal's unread output
        # then, and the guest's last lines, its verdict among them, would be
        # lost to a read that came late.
        console = os.fdopen(master, "r+b", buffering=0)
        os.set_blocking(console.fileno(), False)
        saw_pass = False
        saw_fail = False
        deadline = time.monotonic() + timeout if timeout > 0 else None
        pending = bytearray()
        guest_input = bytearray()
        if serial is not None:
            print(f"Serial console socket: {serial.path}", file=sys.stderr)
        with console_log.open("wb") as log:

            def record(data: bytes) -> None:
                nonlocal saw_pass, saw_fail
                log.write(data)
                log.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                pending.extend(data)
                if len(pending) > 8192:
                    del pending[:-8192]
                decoded = pending.decode("utf-8", errors="replace")
                saw_pass = saw_pass or PASS_MARKER in decoded
                saw_fail = saw_fail or FAIL_MARKER in decoded
                if serial is not None:
                    serial.queue_output(data)

            while process.poll() is None:
                artifact_socket.observe()
                if deadline is not None and time.monotonic() >= deadline:
                    raise TimeoutError(f"guest did not finish within {timeout:.0f}s")
                read_descriptors: list[Any] = [console]
                write_descriptors: list[Any] = [console] if guest_input else []
                if serial is not None:
                    read_descriptors.append(serial.listener)
                    if serial.client is not None:
                        read_descriptors.append(serial.client)
                        if serial.output:
                            write_descriptors.append(serial.client)
                readable, writable, _ = select.select(
                    read_descriptors, write_descriptors, [], 0.25
                )
                if serial is not None and serial.listener in readable:
                    serial.accept(bytes(pending))
                if (
                    serial is not None
                    and serial.client is not None
                    and serial.client in readable
                ):
                    data = serial.read_input()
                    if data:
                        if len(guest_input) + len(data) > MAX_SERIAL_INPUT_BACKLOG:
                            serial.close_client()
                        else:
                            guest_input.extend(data)
                if (
                    serial is not None
                    and serial.client is not None
                    and serial.client in writable
                ):
                    serial.flush_output()
                if console in writable:
                    write_pending(console, guest_input)
                if console not in readable:
                    continue
                try:
                    data = os.read(console.fileno(), 65536)
                except OSError:
                    if process.poll() is not None:
                        break
                    raise
                if not data:
                    break
                record(data)
            # What the emulator wrote before it exited is still queued.
            while True:
                try:
                    data = os.read(console.fileno(), 65536)
                except OSError:
                    break
                if not data:
                    break
                record(data)
            return_code = process.wait(timeout=5)


    if return_code != 0:
        raise RuntimeError(f"emulator exited with status {return_code}")
    return return_code, saw_pass, saw_fail


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Boot a tmpfs-overlay GPU guest from a read-only lower image"
    )
    parser.add_argument("--emulator", type=Path, default=DEFAULT_EMULATOR)
    parser.add_argument("--rootfs", type=Path, default=DEFAULT_ROOTFS)
    parser.add_argument(
        "--rootfs-type",
        choices=ROOTFS_TYPES,
        default="erofs",
        help=(
            "lower filesystem type; ext4 images are attached block-read-only "
            "and without the host-file lock"
        ),
    )
    parser.add_argument(
        "--root-device",
        default="/dev/ubda",
        help="block device the per-run /init mounts beneath the overlay",
    )
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--unit", default="multi-user.target")
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument(
        "--command",
        action="append",
        default=[],
        help="shell command to run after the root-overlay checks (repeatable)",
    )
    parser.add_argument(
        "--guest-file",
        action="append",
        default=[],
        metavar="GUEST_PATH=HOST_PATH",
        help=(
            "install a host file at GUEST_PATH before the guest starts, "
            "keeping its mode; outside /tmp, /run, /dev, /proc and /sys "
            "(repeatable)"
        ),
    )
    parser.add_argument(
        "--docker-hello-world-binary",
        type=Path,
        help=(
            "freestanding arm64/Linux payload to import and execute in the "
            "offline Docker smoke"
        ),
    )
    parser.add_argument(
        "--no-auto-poweroff",
        action="store_true",
        help="leave the guest running after its checks instead of shutting down normally",
    )
    parser.add_argument(
        "--image-init",
        action="store_true",
        help=(
            f"start the image's own {IMAGE_INIT} beneath the overlay, with no "
            "harness session or checks; the run ends when the guest stops "
            "(requires --no-auto-poweroff)"
        ),
    )
    parser.add_argument(
        "--serial-socket",
        type=Path,
        help=(
            "expose the bidirectional serial console at this Unix-domain "
            "socket while retaining the normal log/stdout copy"
        ),
    )
    parser.add_argument(
        "--virtio-console-socket",
        type=Path,
        help=(
            "attach a Virtio console and expose guest hvc0 at this "
            "reconnectable Unix-domain socket"
        ),
    )
    parser.add_argument(
        "--virtio-serial-socket",
        type=Path,
        help=(
            "attach a named Virtio serial port and expose it at this "
            "Unix-domain socket, which the caller owns and retires"
        ),
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "kernel_arguments",
        nargs="*",
        help="additional non-root kernel arguments after '--'",
    )
    options = parser.parse_args(arguments)
    if options.image_init and (
        options.command
        or options.docker_hello_world_binary is not None
        or not options.no_auto_poweroff
    ):
        parser.error(
            "--image-init runs no checks: it takes no --command or Docker "
            "smoke and requires --no-auto-poweroff"
        )
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    emulator = options.emulator.resolve()
    rootfs = options.rootfs.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        raise SafetyError(f"emulator is not executable: {emulator}")
    validate_rootfs_path(rootfs, options.rootfs_type)

    serial_socket = (
        options.serial_socket.resolve()
        if options.serial_socket is not None
        else None
    )
    virtio_console_socket = (
        options.virtio_console_socket.resolve()
        if options.virtio_console_socket is not None
        else None
    )
    virtio_serial_socket = (
        options.virtio_serial_socket.resolve()
        if options.virtio_serial_socket is not None
        else None
    )
    validate_console_socket_paths(
        serial_socket, virtio_console_socket, virtio_serial_socket
    )

    docker_hello_world = None
    if options.docker_hello_world_binary is not None:
        docker_hello_world = load_docker_hello_world(
            options.docker_hello_world_binary.resolve()
        )
    caller_files = [parse_guest_file(value) for value in options.guest_file]
    test_script = None if options.image_init else make_test_script(
        options.command,
        auto_poweroff=not options.no_auto_poweroff,
        docker_hello_world=docker_hello_world is not None,
        lower_filesystem_type=options.rootfs_type,
        root_device=options.root_device,
    )
    boot_files = runner_guest_files(
        test_script,
        options.root_device,
        options.rootfs_type,
        docker_hello_world,
    )
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifact_directory = options.artifact_root.resolve() / (
        f"{timestamp}-{os.getpid()}"
    )
    boot_file_directory = artifact_directory / "boot-files"
    command = build_command(
        emulator,
        rootfs,
        options.memory,
        options.unit,
        options.kernel_arguments,
        guest_file_arguments(boot_file_directory, boot_files, caller_files),
        rootfs_type=options.rootfs_type,
        virtio_console_socket=virtio_console_socket,
        virtio_serial_socket=virtio_serial_socket,
    )
    result = {
        "command": command,
        "rootfs": str(rootfs),
        "rootfs_type": options.rootfs_type,
        "auto_poweroff": not options.no_auto_poweroff,
        "image_init": options.image_init,
    }
    if serial_socket is not None:
        result["serial_socket"] = str(serial_socket)
    if virtio_console_socket is not None:
        result["virtio_console_socket"] = str(virtio_console_socket)
    if virtio_serial_socket is not None:
        result["virtio_serial_socket"] = str(virtio_serial_socket)
    if options.docker_hello_world_binary is not None:
        result["docker_hello_world"] = str(
            options.docker_hello_world_binary.resolve()
        )
    if options.dry_run:
        print(shlex.join(command))
        return 0

    artifact_directory.mkdir(parents=True, exist_ok=False)
    write_guest_files(boot_file_directory, boot_files)
    write_json(artifact_directory / "run.json", result)
    return_code, saw_pass, saw_fail = run_guest(
        command,
        artifact_directory / "console.log",
        options.timeout,
        expect_poweroff=not options.no_auto_poweroff,
        serial_socket=serial_socket,
    )
    result.update(
        {
            "emulator_return_code": return_code,
            "saw_pass_marker": saw_pass,
            "saw_fail_marker": saw_fail,
        }
    )
    write_json(artifact_directory / "run.json", result)
    # run_guest has already refused a nonzero emulator status; an image's own
    # session prints no test markers.
    if not options.image_init and (not saw_pass or saw_fail):
        raise RuntimeError("guest root-overlay verification did not pass")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print(
            f"{Path(sys.argv[0]).name}: interrupted; guest stopped",
            file=sys.stderr,
        )
        raise SystemExit(130) from None
    except (SafetyError, RuntimeError, TimeoutError) as error:
        print(f"{Path(sys.argv[0]).name}: {error}", file=sys.stderr)
        raise SystemExit(1) from error
