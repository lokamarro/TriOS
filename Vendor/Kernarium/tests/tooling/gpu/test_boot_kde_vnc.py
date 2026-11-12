#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[3] / "tools" / "boot_kde_vnc.py"
GPU_INTEGRATION = Path(__file__).resolve().parents[2] / "integration" / "gpu"
SUPPORT = Path(__file__).resolve().parents[2] / "integration" / "support"
sys.path.insert(0, str(SUPPORT))
sys.path.insert(0, str(GPU_INTEGRATION))
import kde_session  # noqa: E402
import run_webgl_samples  # noqa: E402


def legacy_seed() -> str:
    return kde_session.LEGACY_KDE_SEED.read_text(encoding="utf-8")


def seed_commands() -> list[str]:
    """The seed's commands, one per entry with continuations joined."""
    return [
        line.strip()
        for line in legacy_seed().replace("\\\n", " ").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def run_samples_launcher(
    deadlines: dict[str, int], chromium: str
) -> subprocess.CompletedProcess[str]:
    """Run the samples launcher on this host against stub guest commands."""

    launcher = run_webgl_samples.build_launcher(
        "/aquarium/aquarium.html", [], deadlines
    )
    stubs = {
        "id": "echo 1001",
        "sleep": ":",
        # Run the bounded command without its bound; the stubs never block.
        "timeout": (
            'while :; do case "$1" in -k) shift 2 ;; -*) shift ;; *) break ;; '
            'esac; done\nshift\nexec "$@"'
        ),
        "runuser": 'shift 3\nexec "$@"',
        "glxinfo": "echo stub-glxinfo",
        # Plasma has finished starting: its shell runs and its splash is gone.
        "pgrep": '[ "$2" = ksplashqml ] && exit 1\necho 4242',
        "curl": (
            'while [ "$1" != --output ]; do shift; done\n'
            'echo "<html>" > "$2"'
        ),
        "chromium": chromium,
    }
    with tempfile.TemporaryDirectory(dir="/tmp") as directory:
        root = Path(directory)
        uptime = root / "uptime"
        uptime.write_text("5.00 0\n", encoding="utf-8")
        runtime = root / "run"
        x11 = root / "x11"
        for path in (runtime, x11):
            path.mkdir()
        (runtime / "xauth_test").write_text("", encoding="utf-8")
        listeners = []
        for path in (runtime / "bus", x11 / "X0"):
            listener = socket.socket(socket.AF_UNIX)
            listener.bind(str(path))
            listeners.append(listener)
        bin_directory = root / "bin"
        bin_directory.mkdir()
        for name, body in stubs.items():
            stub = bin_directory / name
            stub.write_text(f"#!/bin/sh\n{body}\n", encoding="utf-8")
            stub.chmod(0o755)
        script = root / "launcher"
        script.write_text(
            launcher.replace("/proc/uptime", str(uptime))
            .replace("/run/user/$uml_uid", str(runtime))
            .replace("/tmp/.X11-unix", str(x11))
            .replace("/tmp/chromium-webgl-samples", str(root / "profile"))
            .replace("/run/uml-webgl-first.html", str(root / "first.html")),
            encoding="utf-8",
        )
        try:
            return subprocess.run(
                ["/bin/sh", str(script)],
                capture_output=True,
                text=True,
                timeout=60,
                env={"PATH": f"{bin_directory}:/usr/bin:/bin"},
            )
        finally:
            for listener in listeners:
                listener.close()


class KdeVncLauncherTests(unittest.TestCase):
    def test_defaults_to_localhost_without_a_password(self) -> None:
        options = kde_session.parse_arguments([])
        self.assertEqual(options.bind, "127.0.0.1")
        self.assertEqual(options.display, "native")
        self.assertIsNone(options.password)
        self.assertEqual(options.cpus, 10)
        self.assertEqual(options.engine, "jit")
        self.assertEqual(options.rootfs, kde_session.DEFAULT_ROOTFS)
        self.assertEqual(options.rootfs_type, "ext4")
        self.assertEqual(options.image_profile, "starter")
        self.assertEqual(options.root_device, "/dev/ubda1")

    def test_default_starter_disk_contract_is_forwarded(self) -> None:
        command = kde_session.build_harness_command(
            kde_session.parse_arguments([])
        )
        separator = kde_session.guest_argument_separator(command)
        harness_arguments = command[:separator]
        self.assertIn(str(kde_session.DEFAULT_ROOTFS), harness_arguments)
        self.assertEqual(
            harness_arguments[
                harness_arguments.index("--rootfs-type") :
                harness_arguments.index("--rootfs-type") + 2
            ],
            ["--rootfs-type", "ext4"],
        )
        self.assertEqual(
            harness_arguments[
                harness_arguments.index("--root-device") :
                harness_arguments.index("--root-device") + 2
            ],
            ["--root-device", "/dev/ubda1"],
        )
        # The starter disk owns its whole session: the initramfs starts its
        # own init, and the launcher adds no setup or network override.
        self.assertIn("--image-init", harness_arguments)
        self.assertNotIn("--command", harness_arguments)
        self.assertNotIn("--guest-file", harness_arguments)
        self.assertNotIn("ip=off", command[separator + 1 :])

    def test_execution_settings_are_forwarded(self) -> None:
        options = kde_session.parse_arguments(
            ["--cpus", "6", "--engine", "gadgets"]
        )
        command = kde_session.build_harness_command(options)
        separator = kde_session.guest_argument_separator(command)
        self.assertIn("ncpus=6", command[separator + 1 :])
        self.assertIn("--execution-engine=gadgets", command[separator + 1 :])

    def test_standard_launch_attaches_host_audio(self) -> None:
        command = kde_session.build_harness_command(
            kde_session.parse_arguments([])
        )
        separator = kde_session.guest_argument_separator(command)
        self.assertEqual(command.count("--audio"), 1)
        self.assertIn("--audio", command[separator + 1 :])

    def test_serial_socket_is_forwarded_to_the_guest_harness(self) -> None:
        options = kde_session.parse_arguments(
            ["--serial-socket", "/tmp/uml-serial.sock"]
        )
        command = kde_session.build_harness_command(options)
        separator = kde_session.guest_argument_separator(command)
        self.assertEqual(
            command[command.index("--serial-socket") : separator],
            ["--serial-socket", "/tmp/uml-serial.sock"],
        )
        self.assertIn(
            "systemd.wants=serial-getty@ttyS0.service",
            command[separator + 1 :],
        )

    def test_virtio_console_socket_attaches_hvc0_getty(self) -> None:
        options = kde_session.parse_arguments(
            ["--virtio-console-socket", "/tmp/uml-hvc0.sock"]
        )
        command = kde_session.build_harness_command(options)
        separator = kde_session.guest_argument_separator(command)
        self.assertEqual(
            command[command.index("--virtio-console-socket") : separator],
            ["--virtio-console-socket", "/tmp/uml-hvc0.sock"],
        )
        kernel_arguments = command[separator + 1 :]
        self.assertIn(
            "systemd.wants=serial-getty@hvc0.service", kernel_arguments
        )
        self.assertNotIn(
            "systemd.mask=serial-getty@hvc0.service", kernel_arguments
        )

    def test_console_socket_paths_must_be_distinct(self) -> None:
        with self.assertRaises(SystemExit):
            kde_session.parse_arguments(
                [
                    "--serial-socket",
                    "/tmp/uml-console.sock",
                    "--virtio-console-socket",
                    "/tmp/uml-console.sock",
                ]
            )

    def test_iso_is_attached_read_only_and_mounted_at_cdrom(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            iso = Path(directory) / "installer.iso"
            iso.touch()
            options = kde_session.parse_arguments(["--iso", str(iso)])
            command = kde_session.build_harness_command(options)

        separator = kde_session.guest_argument_separator(command)
        kernel_arguments = command[separator + 1 :]
        self.assertIn(f"ubd1r={iso.resolve()}", kernel_arguments)
        # The image's systemd mounts it, so the starter's own init needs no
        # command from the launcher.
        self.assertIn(
            "systemd.mount-extra=/dev/ubdb:/mnt/cdrom:iso9660:ro",
            kernel_arguments,
        )
        self.assertNotIn("--command", command[:separator])

    def test_no_iso_disk_or_mount_is_added_by_default(self) -> None:
        command = kde_session.build_harness_command(
            kde_session.parse_arguments([])
        )
        self.assertNotIn(kde_session.ISO_MOUNT_ARGUMENT, command)
        self.assertFalse(any(argument.startswith("ubd1") for argument in command))

    def test_optional_display_density_is_forwarded_to_the_host_gpu(self) -> None:
        default_command = kde_session.build_harness_command(
            kde_session.parse_arguments([])
        )
        self.assertFalse(
            any(item.startswith("--gpu-dpi=") for item in default_command)
        )
        density_command = kde_session.build_harness_command(
            kde_session.parse_arguments(["--dpi", "288"])
        )
        self.assertIn("--gpu-dpi=288", density_command)

    def test_webgl_commands_extend_the_gpu_harness(self) -> None:
        options = kde_session.parse_arguments([])
        command = kde_session.build_harness_command(options)
        guest_separator = kde_session.guest_argument_separator(command)
        self.assertGreater(
            guest_separator, command.index(str(kde_session.GUEST_HARNESS))
        )
        for runner_name in ("run_chromium_webgl.py", "run_webgl_samples.py"):
            runner = (GPU_INTEGRATION / runner_name).read_text(encoding="utf-8")
            self.assertIn(
                "kde_session.guest_argument_separator(command)", runner
            )

    def test_launcher_is_a_command_line_over_the_session(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--help"],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Boot KDE Plasma Wayland", result.stdout)

    def test_webgl_runners_boot_legacy_images(self) -> None:
        # rootfs.erofs and the receipt-bound ext4 derivatives sealed from it
        # are legacy images even when their filesystem is ext4, the type that
        # otherwise selects a starter disk.
        for runner_name in ("run_chromium_webgl.py", "run_webgl_samples.py"):
            runner = (GPU_INTEGRATION / runner_name).read_text(encoding="utf-8")
            self.assertRegex(runner, r'"--image-profile",\s*"legacy",')

    def test_webgl_samples_pin_gles_and_bound_first_navigation(self) -> None:
        runner = (
            GPU_INTEGRATION / "run_webgl_samples.py"
        ).read_text(encoding="utf-8")
        launcher = run_webgl_samples.build_launcher(
            "/aquarium/aquarium.html",
            [],
            run_webgl_samples.launcher_deadlines(300, 900),
        )
        self.assertIn("--use-gl=angle", launcher)
        self.assertIn("--use-angle=gles-egl", launcher)
        self.assertIn("--ignore-gpu-blocklist", launcher)
        self.assertIn("--disable-quic", launcher)
        self.assertIn("curl --fail --location", runner)
        self.assertIn("UML_WEBGL_FETCH_OK|BYTES=", runner)
        self.assertIn("first page fetch failed", runner)
        self.assertIn('"run_at": "document_end"', runner)
        self.assertNotIn('"run_at": "document_idle"', runner)
        self.assertIn('default=360', runner)
        self.assertIn("first WebGL sample did not render within", runner)
        self.assertIn('"state": "failed"', runner)
        self.assertIn('"state": "interrupted"', runner)

    def test_webgl_samples_derive_launcher_deadlines(self) -> None:
        deadlines = run_webgl_samples.launcher_deadlines(300, 900)
        self.assertEqual(
            deadlines,
            {"session_deadline": 75, "fetch_deadline": 105, "deadline": 855},
        )
        launcher = run_webgl_samples.build_launcher(
            "/aquarium/aquarium.html", [], deadlines
        )
        self.assertIsNone(re.search(r"@[A-Z_]+@", launcher))
        self.assertIn("session_deadline=75\n", launcher)
        self.assertIn('--max-time "$fetch_seconds"', launcher)
        self.assertIn("timeout 30 glxinfo -B", launcher)
        self.assertIn(
            'run_chromium "$((deadline - $(uptime_seconds)))"', launcher
        )
        self.assertIn('timeout -k 10 "$chromium_bound"', launcher)
        self.assertIn("Chromium exited with status", launcher)
        self.assertIn("Xauthority cookie was not found", launcher)
        self.assertNotIn("exec runuser", launcher)
        with self.assertRaises(ValueError):
            run_webgl_samples.launcher_deadlines(200, 900)

    def test_webgl_samples_launcher_names_how_chromium_stopped(self) -> None:
        deadlines = run_webgl_samples.launcher_deadlines(300, 900)
        for status, expected in (
            ("3", "Chromium exited with status 3 before WebGL samples "
             "reported"),
            ("124", "WebGL samples did not report by guest uptime 855s"),
        ):
            with self.subTest(status=status):
                self.assertEqual(
                    run_samples_launcher(
                        deadlines, f"echo started\nexit {status}"
                    ).stdout.strip().splitlines()[-1],
                    "UML_WEBGL_RUNNER_ERROR: " + expected,
                )

    def test_webgl_samples_capture_once_and_name_the_failure(self) -> None:
        # A stand-in capture client that counts its runs and fails the way
        # the real one does when no rendered frame arrives.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runs = root / "runs"
            client = root / "client"
            client.write_text(
                "#!/bin/sh\n"
                f"echo run >> {runs}\n"
                "echo 'uml-vnc-smoke-client: VNC framebuffer remained blank' "
                ">&2\n"
                "exit 1\n",
                encoding="utf-8",
            )
            client.chmod(0o755)
            with self.assertRaisesRegex(
                RuntimeError, "VNC framebuffer remained blank"
            ):
                run_webgl_samples.capture_sample(
                    client, "webgl", 5900, root / "sample.ppm"
                )
            self.assertEqual(runs.read_text().splitlines(), ["run"])

    def test_webgl_samples_reject_poisoned_virgl_contexts(self) -> None:
        clean = "UML_WEBGL_SAMPLE_OK|1/2|Aquarium"
        self.assertIsNone(run_webgl_samples.graphics_failure(clean))
        for signature in run_webgl_samples.GRAPHICS_FAILURES:
            line = "host: " + signature
            self.assertEqual(
                run_webgl_samples.graphics_failure(clean + "\n" + line),
                line,
            )

    def test_webgl_samples_reap_their_guest_harness(self) -> None:
        # The stand-in harness reports a guest failure, then keeps running
        # the way a --no-auto-poweroff guest does until it is signalled.
        harness = (
            "import os, signal, sys, time\n"
            "signal.signal(signal.SIGINT, signal.default_int_handler)\n"
            "with open(sys.argv[1], 'w') as record:\n"
            "    print(os.getpid(), os.getpgid(0), os.getsid(0), file=record)\n"
            "print('UML_WEBGL_RUNNER_ERROR: stand-in guest failed', flush=True)\n"
            "time.sleep(60)\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record = root / "harness"
            command = [sys.executable, "-c", harness, str(record), "--"]
            arguments = [
                "run_webgl_samples.py",
                "--artifact-root", str(root / "artifacts"),
                "--end-at", "1",
            ]
            samples_launcher = run_webgl_samples.kde_session
            with (
                mock.patch.object(sys, "argv", arguments),
                mock.patch.object(
                    run_webgl_samples.kernarium_common, "validate_rootfs"
                ),
                mock.patch.object(samples_launcher, "parse_arguments"),
                mock.patch.object(
                    samples_launcher,
                    "build_harness_command",
                    return_value=command,
                ),
                mock.patch.object(
                    run_webgl_samples.cts, "git_state", return_value={}
                ),
            ):
                try:
                    with self.assertRaisesRegex(
                        RuntimeError, "stand-in guest failed"
                    ):
                        run_webgl_samples.main()
                finally:
                    pid, group, session = map(int, record.read_text().split())
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        leaked = False
                    else:
                        leaked = True
                        os.kill(pid, signal.SIGKILL)
        self.assertFalse(leaked, "the runner left its guest harness running")
        self.assertEqual((group, session), (pid, pid))

    def test_guest_harness_runs_directly(self) -> None:
        options = kde_session.parse_arguments(
            [
                "--rootfs", "/tmp/rootfs.erofs",
                "--rootfs-type", "erofs",
            ]
        )
        command = kde_session.build_harness_command(options)
        self.assertEqual(command[0], sys.executable)
        self.assertEqual(
            command[1:10],
            [
                str(kde_session.GUEST_HARNESS),
                "--emulator",
                str(options.emulator),
                "--rootfs",
                "/tmp/rootfs.erofs",
                "--rootfs-type",
                "erofs",
                "--root-device",
                "/dev/ubda",
            ],
        )
        seed = command.index("--guest-file")
        self.assertEqual(
            command[seed : seed + 4],
            [
                "--guest-file",
                f"{kde_session.LEGACY_KDE_SEED_GUEST_PATH}="
                f"{kde_session.LEGACY_KDE_SEED}",
                "--command",
                kde_session.LEGACY_KDE_SEED_GUEST_PATH,
            ],
        )
        self.assertTrue(os.access(kde_session.LEGACY_KDE_SEED, os.X_OK))
        self.assertNotIn("--image-init", command)

    def test_ext4_root_type_is_forwarded_to_the_guest_harness(self) -> None:
        options = kde_session.parse_arguments(
            ["--rootfs", "/tmp/rootfs.ext4", "--rootfs-type", "ext4"]
        )
        command = kde_session.build_harness_command(options)
        separator = kde_session.guest_argument_separator(command)
        rootfs_type = command.index("--rootfs-type")
        self.assertLess(rootfs_type, separator)
        self.assertEqual(
            command[rootfs_type : rootfs_type + 2],
            ["--rootfs-type", "ext4"],
        )
        root_device = command.index("--root-device")
        self.assertEqual(
            command[root_device : root_device + 2],
            ["--root-device", "/dev/ubda1"],
        )

    def test_desktop_idle_lock_is_disabled_by_the_seed(self) -> None:
        commands = legacy_seed()
        self.assertIn("kscreenlockerrc", commands)
        self.assertIn("Autolock=false", commands)
        self.assertIn("LockOnResume=false", commands)

    def test_seed_home_directories_name_every_parent(self) -> None:
        # install -d leaves unnamed parents owned by root, which the session
        # user then cannot write, so each parent must be named before its
        # children.
        install = next(
            command
            for command in seed_commands()
            if command.startswith("install -d -m 0755 -o ")
        )
        directories = install.split()[8:]
        home = directories[0].rsplit("/", 1)[0]
        named: set[str] = set()
        for directory in directories:
            parent = directory.rsplit("/", 1)[0]
            self.assertTrue(
                parent == home or parent in named,
                f"{directory} is named before its parent",
            )
            named.add(directory)

    def test_seed_is_valid_shell(self) -> None:
        result = subprocess.run(
            ["sh", "-n", str(kde_session.LEGACY_KDE_SEED)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("set -eu", seed_commands())

    def test_starter_image_owns_account_and_login_policy(self) -> None:
        command = kde_session.build_harness_command(
            kde_session.parse_arguments([])
        )
        separator = kde_session.guest_argument_separator(command)
        harness_arguments = "\n".join(command[:separator])
        self.assertNotIn(str(kde_session.LEGACY_KDE_SEED), harness_arguments)
        self.assertNotIn("/home/debian", harness_arguments)
        self.assertNotIn("sddm", harness_arguments.lower())

    def test_conflicting_getties_are_masked_before_sddm(self) -> None:
        commands = legacy_seed()
        self.assertIn(
            "systemctl mask --now getty@tty0.service getty@tty1.service",
            commands,
        )
        self.assertNotIn("serial-getty@", commands)
        self.assertNotIn("systemctl start getty@", commands)
        self.assertNotIn("agetty", commands)
        self.assertNotIn("chpasswd", commands)

    def test_default_kernel_arguments_control_getties(self) -> None:
        command = kde_session.build_harness_command(
            kde_session.parse_arguments([])
        )
        separator = kde_session.guest_argument_separator(command)
        kernel_arguments = command[separator + 1 :]
        self.assertIn("systemd.mask=serial-getty@hvc0.service", kernel_arguments)
        # The starter Plasma session starts from its getty@tty1 autologin.
        self.assertNotIn("systemd.mask=getty@tty0.service", kernel_arguments)
        self.assertNotIn("systemd.mask=getty@tty1.service", kernel_arguments)

    def test_legacy_erofs_kernel_arguments_mask_display_manager_getties(
        self,
    ) -> None:
        command = kde_session.build_harness_command(
            kde_session.parse_arguments(["--rootfs-type", "erofs"])
        )
        separator = kde_session.guest_argument_separator(command)
        kernel_arguments = command[separator + 1 :]
        self.assertIn("systemd.mask=getty@tty0.service", kernel_arguments)
        self.assertIn("systemd.mask=getty@tty1.service", kernel_arguments)

    def test_legacy_ext4_derivative_keeps_the_legacy_session_policy(
        self,
    ) -> None:
        options = kde_session.parse_arguments(
            [
                "--rootfs", "/tmp/rootfs-vulkan-venus.ext4",
                "--rootfs-type", "ext4",
                "--image-profile", "legacy",
            ]
        )
        self.assertEqual(options.root_device, "/dev/ubda")
        command = kde_session.build_harness_command(options)
        separator = kde_session.guest_argument_separator(command)
        harness_arguments = command[:separator]
        kernel_arguments = command[separator + 1 :]
        rootfs_type = harness_arguments.index("--rootfs-type")
        self.assertEqual(
            harness_arguments[rootfs_type : rootfs_type + 2],
            ["--rootfs-type", "ext4"],
        )
        self.assertIn(kde_session.LEGACY_KDE_SEED_GUEST_PATH, harness_arguments)
        self.assertNotIn("--image-init", harness_arguments)
        self.assertIn("systemd.mask=getty@tty0.service", kernel_arguments)
        self.assertIn("systemd.mask=getty@tty1.service", kernel_arguments)
        self.assertNotIn("ip=off", kernel_arguments)

    def test_starter_profile_requires_an_ext4_disk(self) -> None:
        with self.assertRaises(SystemExit):
            kde_session.parse_arguments(
                ["--rootfs-type", "erofs", "--image-profile", "starter"]
            )

    def test_legacy_erofs_override_retains_uml_compatibility(self) -> None:
        options = kde_session.parse_arguments(["--rootfs-type", "erofs"])
        self.assertEqual(options.root_device, "/dev/ubda")
        command = kde_session.build_harness_command(options)
        separator = kde_session.guest_argument_separator(command)
        self.assertIn(kde_session.LEGACY_KDE_SEED_GUEST_PATH, command[:separator])
        commands = legacy_seed()
        self.assertIn("getent passwd uml", commands)
        self.assertIn("User=uml", commands)
        self.assertIn("/home/uml", commands)
        self.assertNotIn("useradd", commands)
        self.assertNotIn("getent passwd debian", commands)
        self.assertNotIn("ip=off", command[separator + 1 :])

    def test_password_is_forwarded_to_the_host_gpu(self) -> None:
        options = kde_session.parse_arguments(
            ["--display", "vnc", "--password", "secret", "--dry-run"]
        )
        command = kde_session.build_harness_command(options)
        self.assertIn("--gpu-display=vnc", command)
        self.assertIn("--gpu-vnc-bind=127.0.0.1", command)
        self.assertIn("--gpu-vnc-password=secret", command)

    def test_native_default_has_no_vnc_host_options(self) -> None:
        command = kde_session.build_harness_command(
            kde_session.parse_arguments([])
        )
        self.assertIn("--gpu-display=native", command)
        self.assertFalse(any(item.startswith("--gpu-vnc-") for item in command))

    def test_vnc_password_limit_is_enforced(self) -> None:
        with self.assertRaises(SystemExit):
            kde_session.parse_arguments(["--password", "123456789"])

    def test_native_display_rejects_a_vnc_password(self) -> None:
        with self.assertRaises(SystemExit):
            kde_session.parse_arguments(["--password", "secret"])

    def test_native_display_rejects_vnc_network_options(self) -> None:
        with self.assertRaises(SystemExit):
            kde_session.parse_arguments(["--port", "5901"])


if __name__ == "__main__":
    unittest.main()
