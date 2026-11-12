#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import io
import os
import signal
from pathlib import Path
import socket
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
from unittest import mock

GPU_INTEGRATION_ROOT = Path(__file__).resolve().parents[2] / "integration/gpu"
INTEGRATION_CMAKE = Path(__file__).resolve().parents[2] / "integration/CMakeLists.txt"
sys.path.insert(0, str(GPU_INTEGRATION_ROOT))
import run_gpu_guest  # noqa: E402

# How long a step may take before the run is called wedged. The tests below
# start a Python process and pass bytes between threads and sockets; on a host
# running other work at many times its core count such a step takes seconds,
# so a short bound measures the host's load rather than the code. This one
# only separates a wedged run from a slow one, and is the only time anything
# here waits on: every step otherwise proceeds on its own signal.
WEDGE_BOUND_SECONDS = 120


def boot_file_pairs(
    rootfs_type: str = "erofs",
    root_device: str = "/dev/ubda",
    caller_files=(),
) -> list[tuple[str, Path]]:
    files = run_gpu_guest.runner_guest_files(
        run_gpu_guest.make_test_script(
            ["true"], auto_poweroff=True, lower_filesystem_type=rootfs_type
        ),
        root_device,
        rootfs_type,
    )
    return run_gpu_guest.guest_file_arguments(
        Path("/artifacts/boot-files"), files, caller_files
    )


class GpuGuestSafetyTests(unittest.TestCase):
    def test_command_boots_the_erofs_image_through_the_ramdisk(self) -> None:
        command = run_gpu_guest.build_command(
            Path("/bin/echo"),
            Path("/tmp/rootfs.erofs"),
            "8G",
            "multi-user.target",
            ["quiet"],
            boot_file_pairs(),
        )
        resolved_image = Path("/tmp/rootfs.erofs").resolve()
        self.assertIn(f"ubd0c={resolved_image}", command)
        self.assertNotIn(f"ubd0r={resolved_image}", command)
        self.assertIn("console=tty0", command)
        self.assertIn("console=ttyS0", command)
        self.assertIn("systemd.unit=multi-user.target", command)
        self.assertIn(f"systemd.wants={run_gpu_guest.TEST_UNIT}", command)
        # The per-run /init owns the root, the init and the network.
        for argument in command:
            self.assertNotIn(
                argument.split("=", 1)[0],
                ("root", "rootfstype", "init", "ip"),
            )
        guest_files = [
            argument.removeprefix("--guest-file=").split("=", 1)
            for argument in command
            if argument.startswith("--guest-file=")
        ]
        self.assertEqual(
            {guest for guest, _ in guest_files},
            {
                "/boot.env",
                "/payload/usr/local/libexec/uml-session",
                "/payload/etc/uml-session.env",
                "/payload/usr/local/libexec/uml-gpu-overlay-test",
                "/payload/etc/systemd/system/uml-gpu-overlay-test.service",
            },
        )
        self.assertIn(
            ["/payload/usr/local/libexec/uml-session",
             str(run_gpu_guest.SESSION_SCRIPT)],
            guest_files,
        )
        with self.assertRaisesRegex(run_gpu_guest.SafetyError, "no /boot.env"):
            run_gpu_guest.build_command(
                Path("/bin/echo"), Path("/tmp/rootfs.erofs"), "8G",
                "multi-user.target", [], [],
            )

    def test_ext4_lower_is_attached_block_read_only(self) -> None:
        command = run_gpu_guest.build_command(
            Path("/bin/echo"),
            Path("/tmp/rootfs.ext4"),
            "8G",
            "multi-user.target",
            ["quiet"],
            boot_file_pairs("ext4", "/dev/ubda1"),
            rootfs_type="ext4",
        )
        resolved_image = Path("/tmp/rootfs.ext4").resolve()
        self.assertIn(f"ubd0cr={resolved_image}", command)
        self.assertNotIn(f"ubd0c={resolved_image}", command)
        self.assertEqual(
            run_gpu_guest.boot_parameters("/dev/ubda1", "ext4").splitlines(),
            [
                "UML_BOOT_ROOT=/dev/ubda1",
                "UML_BOOT_ROOT_TYPE=ext4",
                "UML_BOOT_UPPER=tmpfs",
                "UML_BOOT_ENTRY=/usr/local/libexec/uml-session",
            ],
        )
        for device in ("ubda", "/dev/ubda 1", ""):
            with self.subTest(device=device), self.assertRaises(
                run_gpu_guest.SafetyError
            ):
                run_gpu_guest.boot_parameters(device, "ext4")

        script = run_gpu_guest.make_test_script(
            ["true"],
            auto_poweroff=True,
            lower_filesystem_type="ext4",
            root_device="/dev/ubda",
        )
        self.assertIn(
            'findmnt -n -o FSTYPE -T /.uml-lower)" ext4', script
        )
        self.assertIn('blockdev --getro /dev/ubda)" 1', script)

    def test_virtio_console_socket_is_forwarded_to_the_emulator(self) -> None:
        command = run_gpu_guest.build_command(
            Path("/bin/echo"),
            Path("/tmp/rootfs.erofs"),
            "8G",
            "multi-user.target",
            [],
            boot_file_pairs(),
            virtio_console_socket=Path("/tmp/uml-hvc0.sock"),
        )
        self.assertIn(
            "--virtio-console-socket="
            f"{Path('/tmp/uml-hvc0.sock').resolve()}",
            command,
        )

    def test_console_socket_paths_must_be_distinct(self) -> None:
        with self.assertRaisesRegex(
            run_gpu_guest.SafetyError, "must be different"
        ):
            run_gpu_guest.validate_console_socket_paths(
                Path("/tmp/uml-console.sock"),
                Path("/tmp/uml-console.sock"),
            )

    def test_ext4_lower_requires_immutable_host_permissions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            rootfs = Path(temporary) / "rootfs.ext4"
            rootfs.touch()
            rootfs.chmod(0o444)
            run_gpu_guest.validate_rootfs_path(rootfs, "ext4")
            rootfs.chmod(0o644)
            with self.assertRaisesRegex(
                run_gpu_guest.SafetyError, "host write permission"
            ):
                run_gpu_guest.validate_rootfs_path(rootfs, "ext4")

            run_gpu_guest.validate_rootfs_path(rootfs, "erofs")

    def test_unknown_lower_filesystem_is_rejected(self) -> None:
        with self.assertRaisesRegex(
            run_gpu_guest.SafetyError, "unsupported root filesystem type"
        ):
            run_gpu_guest.build_command(
                Path("/bin/echo"),
                Path("/tmp/rootfs.unknown"),
                "8G",
                "multi-user.target",
                [],
                boot_file_pairs(),
                rootfs_type="unknown",
            )

    def test_conflicting_arguments_are_rejected(self) -> None:
        forbidden = (
            "ubd0=/tmp/other.erofs",
            "ubd0c=/tmp/other.erofs",
            "ubd0r=/tmp/other.erofs",
            "root=/dev/ubdb",
            "rootfstype=ext4",
            "init=/sbin/init",
            "initrd=/tmp/other.img",
            "--guest-file=/boot.env=/tmp/boot.env",
            "--guest-init=/tmp/init",
            "mem=2G",
            "console=ttyS0",
            "systemd.unit=graphical.target",
            "--virtio-console-socket=/tmp/other.sock",
            "--virtio-serial-socket=/tmp/other.sock",
        )
        for argument in forbidden:
            with self.subTest(argument=argument):
                with self.assertRaises(run_gpu_guest.SafetyError):
                    run_gpu_guest.validate_extra_arguments([argument])

    def test_boot_files_hand_pid1_to_systemd_through_the_session(self) -> None:
        script = run_gpu_guest.make_test_script(["true"], auto_poweroff=True)
        files = run_gpu_guest.runner_guest_files(script, "/dev/ubda", "erofs")
        self.assertEqual(
            files["/boot.env"][0].decode().splitlines(),
            [
                "UML_BOOT_ROOT=/dev/ubda",
                "UML_BOOT_ROOT_TYPE=erofs",
                "UML_BOOT_UPPER=tmpfs",
                "UML_BOOT_ENTRY=/usr/local/libexec/uml-session",
            ],
        )
        self.assertEqual(
            files["/etc/uml-session.env"][0].decode().splitlines(),
            [
                "UML_SESSION_ENTRY=systemd",
                "UML_SESSION_RESOLVER=user-network",
                "UML_SESSION_CONTAINER_STATE=tmpfs",
            ],
        )
        self.assertEqual(
            files[run_gpu_guest.TEST_SCRIPT_GUEST_PATH], (script.encode(), 0o755)
        )
        unit = files[run_gpu_guest.TEST_UNIT_GUEST_PATH][0].decode()
        self.assertIn(
            f"ExecStart={run_gpu_guest.TEST_SCRIPT_GUEST_PATH}", unit
        )
        self.assertIn("Before=display-manager.service", unit)
        self.assertNotIn(run_gpu_guest.DOCKER_ARCHIVE_GUEST_PATH, files)
        # The session keeps container state out of the upper layer and the
        # test checks it; the image keeps its own accounts and gettys.
        session = run_gpu_guest.SESSION_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("/var/lib/docker /var/lib/containerd", session)
        for directory in ("/var/lib/docker", "/var/lib/containerd"):
            self.assertIn(
                f'findmnt -n -o FSTYPE -T {directory})" tmpfs', script
            )
        self.assertIn('findmnt -n -o FSTYPE -T /.uml-state)" tmpfs', script)
        for text in (script, unit):
            self.assertNotIn("getty@tty0.service", text)
            self.assertNotIn("useradd", text)

    def test_boot_files_are_written_where_the_command_names_them(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = b"\x7fELFtest-payload"
            files = run_gpu_guest.runner_guest_files(
                "#!/bin/sh\n", "/dev/ubda", "erofs", payload
            )
            caller = root / "caller-file"
            caller.write_text("caller", encoding="utf-8")
            pairs = run_gpu_guest.guest_file_arguments(
                root / "boot-files", files,
                [("/usr/local/libexec/caller", caller)],
            )
            self.assertFalse((root / "boot-files").exists())
            run_gpu_guest.write_guest_files(root / "boot-files", files)
            by_archive = dict(pairs)
            self.assertEqual(
                by_archive["/payload/usr/local/libexec/caller"], caller
            )
            for guest_path, (contents, mode) in files.items():
                archive = (
                    guest_path if guest_path == "/boot.env"
                    else "/payload" + guest_path
                )
                host = by_archive[archive]
                self.assertEqual(host.read_bytes(), contents)
                self.assertEqual(host.stat().st_mode & 0o777, mode)
            with self.assertRaisesRegex(run_gpu_guest.SafetyError, "twice"):
                run_gpu_guest.guest_file_arguments(
                    root, files,
                    [("/usr/local/a", caller), ("/usr/local/a", caller)],
                )

    def test_caller_files_are_validated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            host = Path(directory) / "launcher"
            host.write_text("#!/bin/sh\n", encoding="utf-8")
            self.assertEqual(
                run_gpu_guest.parse_guest_file(
                    f"/usr/local/libexec/launcher={host}"
                ),
                ("/usr/local/libexec/launcher", host.resolve()),
            )
            for value in (
                f"/tmp/launcher={host}",
                f"/run/launcher={host}",
                f"relative={host}",
                f"/usr/../launcher={host}",
                "/usr/local/libexec/launcher",
                f"/usr/local/libexec/launcher={directory}",
                f"{run_gpu_guest.TEST_SCRIPT_GUEST_PATH}={host}",
                f"/etc/uml-session.env={host}",
            ):
                with self.subTest(value=value), self.assertRaises(
                    run_gpu_guest.SafetyError
                ):
                    run_gpu_guest.parse_guest_file(value)

    def test_dry_run_prints_the_command_and_writes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rootfs = root / "rootfs.erofs"
            rootfs.write_bytes(b"image")
            launcher = root / "launcher"
            launcher.write_text("#!/bin/sh\n", encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = run_gpu_guest.main([
                    "--emulator", "/bin/echo", "--rootfs", str(rootfs),
                    "--artifact-root", str(root / "artifacts"),
                    "--guest-file", f"/usr/local/libexec/launcher={launcher}",
                    "--dry-run",
                ])
            self.assertEqual(status, 0)
            self.assertIn(
                f"--guest-file=/payload/usr/local/libexec/launcher={launcher.resolve()}",
                output.getvalue(),
            )
            self.assertFalse((root / "artifacts").exists())

    def test_staged_files_are_named_by_content_and_mode(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve() / "staged"
            staged = run_gpu_guest.StagedGuestFiles(root)
            staged.add("/usr/local/libexec/tool", "#!/bin/sh\n", 0o755)
            staged.add("/usr/local/share/tool/data", b"data")
            self.assertFalse(root.exists(), "adding a file wrote it")
            again = run_gpu_guest.StagedGuestFiles(root)
            again.add("/usr/local/libexec/tool", "#!/bin/sh\n", 0o755)
            again.add("/usr/local/share/tool/data", b"other")
            self.assertEqual(staged.arguments[0], again.arguments[0])
            self.assertNotEqual(staged.arguments[1], again.arguments[1])
            mode_only = run_gpu_guest.StagedGuestFiles(root)
            mode_only.add("/usr/local/libexec/tool", "#!/bin/sh\n", 0o700)
            self.assertNotEqual(staged.arguments[0], mode_only.arguments[0])
            staged.write()
            for argument in staged.arguments:
                guest, host = run_gpu_guest.parse_guest_file(
                    argument.removeprefix("--guest-file=")
                )
                contents, mode = staged.files[host]
                self.assertEqual(host.read_bytes(), contents)
                self.assertEqual(host.stat().st_mode & 0o777, mode)
            self.assertEqual(
                sorted(path.name for path in root.iterdir()),
                sorted(path.name for path in staged.files),
            )
            for guest_path in (
                "/tmp/tool",
                "relative/tool",
                run_gpu_guest.TEST_SCRIPT_GUEST_PATH,
                run_gpu_guest.SESSION_GUEST_PATH,
            ):
                with self.subTest(guest_path=guest_path), self.assertRaises(
                    run_gpu_guest.SafetyError
                ):
                    staged.add(guest_path, b"")

    def test_image_init_boots_the_image_with_no_session(self) -> None:
        files = run_gpu_guest.runner_guest_files(None, "/dev/ubda1", "ext4")
        self.assertEqual(list(files), [run_gpu_guest.BOOT_PARAMETERS_GUEST_PATH])
        self.assertEqual(
            files[run_gpu_guest.BOOT_PARAMETERS_GUEST_PATH][0]
            .decode()
            .splitlines(),
            [
                "UML_BOOT_ROOT=/dev/ubda1",
                "UML_BOOT_ROOT_TYPE=ext4",
                "UML_BOOT_UPPER=tmpfs",
                "UML_BOOT_ENTRY=/sbin/init",
            ],
        )
        pairs = run_gpu_guest.guest_file_arguments(Path("/boot-files"), files)
        self.assertEqual(pairs, [("/boot.env", Path("/boot-files/boot.env"))])
        command = run_gpu_guest.build_command(
            Path("/bin/echo"),
            Path("/tmp/rootfs.ext4"),
            "8G",
            "graphical.target",
            [],
            pairs,
            rootfs_type="ext4",
        )
        self.assertFalse(
            any(argument.startswith("systemd.wants=") for argument in command)
        )
        with self.assertRaises(run_gpu_guest.SafetyError):
            run_gpu_guest.runner_guest_files(None, "/dev/ubda", "erofs", b"x")

    def test_image_init_runs_no_checks_and_never_powers_off(self) -> None:
        for extra in (
            ["--no-auto-poweroff", "--command", "true"],
            [],
        ):
            with self.subTest(extra=extra), contextlib.redirect_stderr(
                io.StringIO()
            ), self.assertRaises(SystemExit):
                run_gpu_guest.parse_arguments(["--image-init", *extra])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rootfs = root / "rootfs.ext4"
            rootfs.write_bytes(b"image")
            rootfs.chmod(0o444)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = run_gpu_guest.main([
                    "--emulator", "/bin/echo", "--rootfs", str(rootfs),
                    "--rootfs-type", "ext4", "--root-device", "/dev/ubda1",
                    "--artifact-root", str(root / "artifacts"),
                    "--image-init", "--no-auto-poweroff", "--dry-run",
                ])
            self.assertEqual(status, 0)
            self.assertNotIn("uml-session", output.getvalue())
            self.assertNotIn(run_gpu_guest.TEST_UNIT, output.getvalue())

    def test_offline_docker_smoke_is_local_and_fail_closed(self) -> None:
        payload = b"\x7fELFtest-payload"
        script = run_gpu_guest.make_test_script(
            ["true"],
            auto_poweroff=True,
            docker_hello_world=True,
        )
        self.assertIn("docker import", script)
        self.assertIn(
            f"docker_hello_world_archive='{run_gpu_guest.DOCKER_ARCHIVE_GUEST_PATH}'",
            script,
        )
        self.assertNotIn("base64", script)
        self.assertIn("docker image inspect", script)
        self.assertIn("--pull=never --network none", script)
        self.assertIn("grep -Fxq 'Hello from Docker!'", script)
        self.assertIn('cat "$docker_hello_world_output"', script)
        self.assertIn("trap docker_hello_world_cleanup EXIT HUP INT TERM", script)
        self.assertIn(
            'timeout 10 docker rm -f \\\n'
            '        "$docker_hello_world_container"',
            script,
        )
        self.assertIn(
            'timeout 10 docker image rm -f \\\n'
            '        "$docker_hello_world_image"',
            script,
        )
        self.assertNotIn("docker pull", script)
        self.assertNotIn("docker run --rm hello-world", script)
        run_start = script.index("if ! timeout 30 docker run")
        grep_start = script.index("grep -Fxq 'Hello from Docker!'")
        run_block = script[run_start:grep_start]
        self.assertNotIn("|", run_block)
        self.assertIn('> "$docker_hello_world_output" 2>&1', run_block)
        subprocess.run(["/bin/sh", "-n"], input=script, text=True, check=True)

        # The archive member carries the time it was made, so the two
        # archives are made at one pinned time: a second boundary between
        # them is not a difference in the archive.
        with mock.patch.object(run_gpu_guest.time, "time", return_value=1e9):
            files = run_gpu_guest.runner_guest_files(
                script, "/dev/ubda", "erofs", payload
            )
            expected_archive = run_gpu_guest.make_docker_rootfs_archive(payload)
        archive_bytes, mode = files[run_gpu_guest.DOCKER_ARCHIVE_GUEST_PATH]
        self.assertEqual(mode, 0o644)
        self.assertEqual(archive_bytes, expected_archive)
        with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode="r:") as archive:
            member = archive.getmember("hello")
            self.assertEqual(member.mode, 0o755)
            self.assertEqual(member.uid, 0)
            self.assertEqual(member.gid, 0)
            extracted = archive.extractfile(member)
            self.assertIsNotNone(extracted)
            assert extracted is not None
            self.assertEqual(extracted.read(), payload)

    def test_docker_payload_loader_rejects_non_elf_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            payload = Path(directory) / "hello"
            payload.write_bytes(b"not an ELF")
            with self.assertRaisesRegex(
                run_gpu_guest.SafetyError, "is not an ELF file"
            ):
                run_gpu_guest.load_docker_hello_world(payload)

    def test_integration_graph_builds_and_passes_the_local_payload(self) -> None:
        source = INTEGRATION_CMAKE.read_text(encoding="utf-8")
        # Every guest payload joins integration-test-binaries.
        self.assertIn(
            "add_dependencies(integration-test-binaries uml-${NAME})", source
        )
        self.assertIn(
            "uml_add_guest_payload(UML_DOCKER_HELLO_WORLD docker-hello-world\n",
            source,
        )
        self.assertIn(
            "--docker-hello-world-binary ${UML_DOCKER_HELLO_WORLD}",
            source,
        )
        # Ordinary acceptance must never reach a registry; the offline smoke
        # imports a locally built payload instead. A registry pull is allowed
        # only behind the external-network opt-in, so scope the prohibition to
        # the unconditional part of the graph. Checking the whole file would
        # also forbid the sanctioned opt-in campaign, and checking nothing
        # would let a pull drift back into the default suite.
        guard = "if(UML_ENABLE_EXTERNAL_NETWORK_TESTS)"
        self.assertIn(guard, source)
        unconditional, opt_in = source.split(guard, 1)
        self.assertNotIn("docker run", unconditional)
        self.assertNotIn("docker pull", unconditional)
        self.assertIn("docker run --rm hello-world", opt_in)

    def test_test_script_stops_at_the_first_failed_command(self) -> None:
        script = run_gpu_guest.make_test_script(
            ["false", "true"], auto_poweroff=True
        )
        self.assertIn("    (\n        set -e\nfalse\ntrue\n    )", script)

    def test_console_short_writes_are_retried(self) -> None:
        class ShortWriter(io.BytesIO):
            def write(self, payload: bytes) -> int:
                return super().write(payload[:3])

        stream = ShortWriter()
        pending = bytearray(b"0123456789")
        run_gpu_guest.write_pending(stream, pending)
        self.assertEqual(pending, b"")
        self.assertEqual(stream.getvalue(), b"0123456789")

    def test_no_auto_poweroff_rejects_nonzero_emulator_exit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fake_emulator = (
                "import sys;"
                f"sys.stdout.write({run_gpu_guest.PASS_MARKER!r}+'\\n');"
                "sys.stdout.flush();"
                "raise SystemExit(9)"
            )
            with self.assertRaisesRegex(
                RuntimeError, "emulator exited with status 9"
            ):
                run_gpu_guest.run_guest(
                    [sys.executable, "-c", fake_emulator],
                    Path(directory) / "console.log",
                    timeout=WEDGE_BOUND_SECONDS,
                    expect_poweroff=False,
                )

    def test_repeated_interrupt_cannot_skip_emulator_kill_fallback(self) -> None:
        observed_handlers: list[tuple[object, object]] = []

        class StubbornProcess:
            def __init__(self) -> None:
                self.alive = True
                self.signals: list[int] = []
                # A process that exists, so the stop removes no run
                # directories.
                self.pid = os.getpid()

            def poll(self) -> int | None:
                return None if self.alive else -signal.SIGKILL

            def send_signal(self, signal_number: int) -> None:
                self.signals.append(signal_number)
                if signal_number == signal.SIGKILL:
                    self.alive = False

            def wait(self, timeout: float) -> int:
                if self.alive:
                    observed_handlers.append(
                        (
                            signal.getsignal(signal.SIGINT),
                            signal.getsignal(signal.SIGTERM),
                        )
                    )
                    raise subprocess.TimeoutExpired("emulator", timeout)
                return -signal.SIGKILL

        process = StubbornProcess()
        previous_handlers = (
            signal.getsignal(signal.SIGINT),
            signal.getsignal(signal.SIGTERM),
        )
        run_gpu_guest.terminate_process(process)  # type: ignore[arg-type]

        self.assertEqual(
            observed_handlers, [(signal.SIG_IGN, signal.SIG_IGN)]
        )
        self.assertEqual(process.signals, [signal.SIGTERM, signal.SIGKILL])
        self.assertEqual(
            (
                signal.getsignal(signal.SIGINT),
                signal.getsignal(signal.SIGTERM),
            ),
            previous_handlers,
        )

    def test_serial_socket_relays_console_output_and_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            serial_path = root / "serial.sock"
            received = bytearray()
            client_errors: list[BaseException] = []

            run_ended = threading.Event()

            def attach() -> None:
                try:
                    # The socket appears once run_guest has started its
                    # relay; wait for that, or for the run to end without it.
                    deadline = time.monotonic() + WEDGE_BOUND_SECONDS
                    while not serial_path.exists():
                        if run_ended.is_set() or time.monotonic() >= deadline:
                            raise TimeoutError("serial socket was not created")
                        time.sleep(0.01)

                    def receive_until(
                        client: socket.socket,
                        output: bytearray,
                        marker: bytes,
                    ) -> None:
                        while marker not in output:
                            chunk = client.recv(4096)
                            if not chunk:
                                raise RuntimeError(
                                    "serial console closed before expected output"
                                )
                            output.extend(chunk)

                    first = bytearray()
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.settimeout(WEDGE_BOUND_SECONDS)
                        client.connect(str(serial_path))
                        receive_until(client, first, b"READY\n")

                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.settimeout(WEDGE_BOUND_SECONDS)
                        client.connect(str(serial_path))
                        receive_until(client, received, b"READY\n")
                        client.sendall(b"status\n")
                        receive_until(client, received, b"ECHO:status\n")
                except BaseException as error:
                    client_errors.append(error)

            # Nothing is typed before the console is the caller's: the first
            # line the guest reads is the one the serial client sends.
            fake_emulator = (
                "import sys,time;"
                f"sys.stdout.write({run_gpu_guest.PASS_MARKER!r}+"
                "'\\nTTY:'+str(int(sys.stdin.isatty()))+'\\nREADY\\n');"
                "sys.stdout.flush();"
                "line=sys.stdin.buffer.readline();"
                "sys.stdout.buffer.write(b'ECHO:'+line);"
                "sys.stdout.buffer.flush();"
                "time.sleep(0.2)"
            )
            client_thread = threading.Thread(target=attach)
            client_thread.start()
            try:
                return_code, saw_pass, saw_fail = run_gpu_guest.run_guest(
                    [sys.executable, "-c", fake_emulator],
                    root / "console.log",
                    timeout=WEDGE_BOUND_SECONDS,
                    expect_poweroff=False,
                    serial_socket=serial_path,
                )
            finally:
                run_ended.set()
            # The relay closed its socket as the run ended, so a client that
            # has not finished is on its way out: it sees the close.
            client_thread.join(timeout=WEDGE_BOUND_SECONDS)
            self.assertFalse(client_thread.is_alive())
            self.assertEqual(client_errors, [])
            self.assertEqual(return_code, 0)
            self.assertTrue(saw_pass)
            self.assertFalse(saw_fail)
            self.assertIn(b"TTY:1\n", received)
            self.assertIn(b"ECHO:status\n", received)
            self.assertFalse(serial_path.exists())

    def test_serial_socket_refuses_to_replace_an_existing_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "serial.sock"
            path.write_text("keep", encoding="utf-8")
            with self.assertRaisesRegex(
                run_gpu_guest.SafetyError, "already exists"
            ):
                run_gpu_guest.SerialConsoleSocket(path)
            self.assertEqual(path.read_text(encoding="utf-8"), "keep")

    def test_serial_socket_does_not_remove_a_replacement_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "serial.sock"
            serial = run_gpu_guest.SerialConsoleSocket(path)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            path.unlink()
            path.write_text("replacement", encoding="utf-8")
            serial.close()
            self.assertEqual(path.read_text(encoding="utf-8"), "replacement")


if __name__ == "__main__":
    unittest.main()
