#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[3] / "tools/boot_android.py"
SPEC = importlib.util.spec_from_file_location("boot_android", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
boot_android = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot_android)


class BootAndroidTests(unittest.TestCase):
    def installed_release(
        self,
        root: Path,
        name: str = "accepted-1",
        *,
        build: int = 100,
        data_layout: int = 1,
    ) -> Path:
        release = root / name
        (release / "boot").mkdir(parents=True)
        (release / "guest").mkdir()
        (release / "release-manifest.json").write_text(
            json.dumps({
                "release_id": name,
                "android": {
                    "fingerprint": "fixture/fingerprint",
                    "build_number": build,
                    "security_patch": "2026-10-05",
                },
            }) + "\n",
            encoding="utf-8",
        )
        (release / "guest/partition-layout.json").write_text(
            json.dumps({
                "system_disk": {"sha256": f"{build:064x}"},
                "data_disk": {
                    "layout_version": data_layout,
                    "capacity_gib": {"minimum": 4, "default": 8, "maximum": 64},
                },
            }) + "\n",
            encoding="utf-8",
        )
        (release / "boot/kernel-command-line.txt").write_text(
            "console=ttyS0\n", encoding="utf-8"
        )
        (release / "guest/system.img").write_bytes(b"image")
        return release

    def test_a_density_is_passed_to_the_emulator(self) -> None:
        options = boot_android.parse_arguments(
            ["--width", "1179", "--height", "2400", "--dpi", "480"]
        )
        command = boot_android.emulator_command(
            options,
            Path("/tmp/release"),
            Path("/tmp/instance/data.img"),
            Path("/tmp/uml-darwin-android17"),
        )
        self.assertIn("--gpu-width=1179", command)
        self.assertIn("--gpu-height=2400", command)
        self.assertIn("--gpu-dpi=480", command)
        with self.assertRaises(SystemExit):
            boot_android.parse_arguments(["--dpi", "0"])

    def fake_tools(self, root: Path) -> tuple[Path, Path]:
        emulator = root / "uml-darwin-android17"
        adb = root / "adb"
        for executable in (emulator, adb):
            executable.write_text("#!/bin/sh\n", encoding="utf-8")
            executable.chmod(0o755)
        return emulator, adb

    def test_defaults_use_native_portrait_configuration(self) -> None:
        options = boot_android.parse_arguments([])
        command = boot_android.emulator_command(
            options,
            Path("/tmp/release"),
            Path("/tmp/instance/data.img"),
            Path("/tmp/uml-darwin-android17"),
        )
        self.assertIn("--gpu-display=native", command)
        self.assertIn("--gpu-width=720", command)
        self.assertIn("--gpu-height=1280", command)
        self.assertFalse(any(value.startswith("--gpu-dpi") for value in command))
        self.assertIn("--execution-engine=gadgets", command)
        self.assertIn("--video", command)
        self.assertIn("--video-encoder", command)
        self.assertIn("ncpus=10", command)
        self.assertIn("mem=8G", command)
        self.assertIn("--android-boot-mode=auto", command)
        self.assertIn("--android-data-disk=/tmp/instance/data.img", command)
        self.assertFalse(any(value.startswith("--android-disk") for value in command))
        self.assertFalse(any("android-data-gib" in value for value in command))
        grown = boot_android.emulator_command(
            options,
            Path("/tmp/release"),
            Path("/tmp/instance/data.img"),
            Path("/tmp/uml-darwin-android17"),
            12,
        )
        self.assertIn("--android-data-gib=12", grown)
        self.assertFalse(options.build)
        self.assertTrue(boot_android.parse_arguments(["--build"]).build)

    def test_vnc_display_names_its_port(self) -> None:
        options = boot_android.parse_arguments(
            ["--display", "vnc", "--vnc-port", "5931"]
        )
        command = boot_android.emulator_command(
            options, Path("/r"), Path("/i"), Path("/e")
        )
        self.assertIn("--gpu-display=vnc", command)
        self.assertIn("--gpu-vnc-port=5931", command)
        self.assertNotIn("--gpu-display=native", command)
        native = boot_android.emulator_command(
            boot_android.parse_arguments([]), Path("/r"), Path("/i"), Path("/e")
        )
        self.assertFalse(any("vnc" in argument for argument in native))

    def test_offscreen_display_names_its_options(self) -> None:
        plain = boot_android.emulator_command(
            boot_android.parse_arguments(["--display", "offscreen"]),
            Path("/r"),
            Path("/i"),
            Path("/e"),
        )
        self.assertIn("--gpu-display=offscreen", plain)
        self.assertFalse(any("vnc" in argument for argument in plain))
        self.assertFalse(any("--gpu-offscreen" in argument for argument in plain))
        options = boot_android.parse_arguments(
            [
                "--display",
                "offscreen",
                "--offscreen-size",
                "2266x1374",
                "--offscreen-mirror",
                "800@15",
            ]
        )
        command = boot_android.emulator_command(
            options, Path("/r"), Path("/i"), Path("/e")
        )
        self.assertIn("--gpu-offscreen-size=2266x1374", command)
        self.assertIn("--gpu-offscreen-mirror=800@15", command)
        on = boot_android.parse_arguments(
            ["--display", "offscreen", "--offscreen-mirror", "on"]
        )
        self.assertIn(
            "--gpu-offscreen-mirror=on",
            boot_android.emulator_command(on, Path("/r"), Path("/i"), Path("/e")),
        )

    def test_offscreen_options_need_the_offscreen_display(self) -> None:
        for arguments in (
            ["--offscreen-size", "800x600"],
            ["--display", "vnc", "--offscreen-mirror", "on"],
        ):
            with self.subTest(arguments=arguments), self.assertRaises(SystemExit):
                boot_android.parse_arguments(arguments)
        for arguments in (
            ["--display", "offscreen", "--offscreen-size", "800"],
            ["--display", "offscreen", "--offscreen-size", "0x600"],
            ["--display", "offscreen", "--offscreen-size", "800x"],
            ["--display", "offscreen", "--offscreen-mirror", "800"],
            ["--display", "offscreen", "--offscreen-mirror", "@15"],
            ["--display", "offscreen", "--offscreen-mirror", "maybe"],
        ):
            with self.subTest(arguments=arguments), self.assertRaises(SystemExit):
                boot_android.parse_arguments(arguments)

    def test_audio_output_defaults_to_the_host_and_can_be_null(self) -> None:
        default = boot_android.emulator_command(
            boot_android.parse_arguments([]), Path("/r"), Path("/i"), Path("/e")
        )
        self.assertFalse(any("--audio" in argument for argument in default))
        options = boot_android.parse_arguments(["--audio-output", "null"])
        command = boot_android.emulator_command(
            options, Path("/r"), Path("/i"), Path("/e")
        )
        self.assertIn("--audio-output=null", command)
        with self.assertRaises(SystemExit):
            boot_android.parse_arguments(["--audio-output", "speaker"])

    def test_audio_input_is_silent_unless_the_host_microphone_is_asked_for(
        self
    ) -> None:
        default = boot_android.emulator_command(
            boot_android.parse_arguments([]), Path("/r"), Path("/i"), Path("/e")
        )
        self.assertFalse(any("--audio-input" in argument for argument in default))
        options = boot_android.parse_arguments(["--audio-input", "host"])
        command = boot_android.emulator_command(
            options, Path("/r"), Path("/i"), Path("/e")
        )
        self.assertIn("--audio-input=host", command)
        with self.assertRaises(SystemExit):
            boot_android.parse_arguments(["--audio-input", "mic"])

    def test_adb_forward_is_loopback_only(self) -> None:
        options = boot_android.parse_arguments(["--adb-port", "15555"])
        command = boot_android.emulator_command(
            options, Path("/r"), Path("/i"), Path("/e")
        )
        self.assertIn("--net-forward=tcp:127.0.0.1:15555-5555", command)
        self.assertFalse(any("0.0.0.0" in argument for argument in command))

    def test_boot_mode_defaults_to_auto_and_stays_an_override(self) -> None:
        # A restart in place picks its ramdisk in auto mode whatever the first
        # boot was given, so the launcher's own default is auto too.
        self.assertEqual(boot_android.parse_arguments([]).boot_mode, "auto")
        for mode in ("auto", "normal", "recovery"):
            options = boot_android.parse_arguments([f"--boot-mode={mode}"])
            command = boot_android.emulator_command(
                options, Path("/r"), Path("/i"), Path("/e")
            )
            self.assertIn(f"--android-boot-mode={mode}", command)

    def test_recovery_flag_selects_recovery_boot(self) -> None:
        options = boot_android.parse_arguments(["--recovery"])
        command = boot_android.emulator_command(
            options, Path("/r"), Path("/i"), Path("/e")
        )
        self.assertEqual(options.boot_mode, "recovery")
        self.assertIn("--android-boot-mode=recovery", command)
        with self.assertRaises(SystemExit):
            boot_android.parse_arguments(["--recovery", "--boot-mode=auto"])

    def test_accepted_descriptor_selects_installed_release(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            releases = root / "releases"
            release = self.installed_release(releases)
            descriptor = root / "accepted.env"
            descriptor.write_text(
                "# fixture\nLINEAGE24_ACCEPTED_DIRECTORY_NAME='accepted-1'\n",
                encoding="utf-8",
            )
            selected = boot_android.release_path(
                None, descriptor=descriptor, releases_root=releases
            )
            self.assertEqual(selected, release.resolve())

    def test_accepted_descriptor_rejects_path_escape(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            descriptor = Path(temporary) / "accepted.env"
            descriptor.write_text(
                "LINEAGE24_ACCEPTED_DIRECTORY_NAME='../escape'\n",
                encoding="utf-8",
            )
            with self.assertRaises(boot_android.LauncherError):
                boot_android.accepted_directory(descriptor)

    def test_release_requires_runtime_payloads(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            release = Path(temporary) / "incomplete"
            release.mkdir()
            with self.assertRaises(boot_android.LauncherError):
                boot_android.release_path(release)

    def test_cli_rejects_invalid_resources(self) -> None:
        for arguments in (
            ["--cpus", "0"],
            ["--width", "0"],
            ["--height", "0"],
            ["--adb-port", "65536"],
            ["--memory", "8 G"],
        ):
            with self.subTest(arguments=arguments):
                with self.assertRaises(SystemExit):
                    boot_android.parse_arguments(arguments)

    def test_default_instance_is_the_stable_directory(self) -> None:
        instance = boot_android.instance_path(None)
        self.assertEqual(instance.name, "android-17")
        # instance_path() resolves symbolic links, and `.build` may be one.
        self.assertEqual(
            instance.parent, boot_android.INSTANCES_ROOT.resolve(strict=False)
        )

    def test_data_capacity_stays_inside_the_releases_range(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            release = boot_android.android_instance.read_release(
                self.installed_release(Path(temporary))
            )
        self.assertIsNone(
            boot_android.data_capacity(boot_android.parse_arguments([]), release)
        )
        for gib, accepted in ((3, False), (4, True), (64, True), (65, False)):
            options = boot_android.parse_arguments(["--data-gib", str(gib)])
            if accepted:
                self.assertEqual(boot_android.data_capacity(options, release), gib)
            else:
                with self.assertRaises(boot_android.LauncherError):
                    boot_android.data_capacity(options, release)

    def test_update_flags_exclude_each_other(self) -> None:
        for arguments in (
            ["--retry", "--reset"],
            ["--retry", "--restore-snapshot"],
            ["--restore-snapshot", "--reset"],
        ):
            with self.subTest(arguments=arguments):
                with self.assertRaises(SystemExit):
                    boot_android.parse_arguments(arguments)
        for removed in (["--disk", "x.img"], ["--sgdisk", "sgdisk"]):
            with self.subTest(removed=removed):
                with self.assertRaises(SystemExit):
                    boot_android.parse_arguments(removed)

    def test_a_release_before_the_two_disk_layout_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            release = Path(temporary) / "old"
            (release / "boot").mkdir(parents=True)
            (release / "guest").mkdir()
            (release / "release-manifest.json").write_text("{}\n", encoding="utf-8")
            (release / "boot/kernel-command-line.txt").write_text("x\n", encoding="utf-8")
            (release / "guest/android-17-uml-arm64.img").write_bytes(b"image")
            with self.assertRaises(boot_android.LauncherError):
                boot_android.release_path(release)

    def test_build_commands_use_the_checked_in_preset(self) -> None:
        self.assertEqual(
            boot_android.build_commands(Path("/usr/bin/cmake")),
            [
                ["/usr/bin/cmake", "--preset", "macos-android17"],
                [
                    "/usr/bin/cmake", "--build", "--preset", "macos-android17"
                ],
            ],
        )

    def test_adb_is_ready_in_the_device_or_recovery_state(self) -> None:
        guest = mock.Mock()
        guest.poll.return_value = None
        for state in ("device", "recovery"):
            def run(command, **_kwargs):
                text = state if "get-state" in command else "connected"
                return subprocess.CompletedProcess(command, 0, stdout=text + "\n")

            with mock.patch.object(boot_android.subprocess, "run", side_effect=run):
                boot_android.wait_for_adb(Path("adb"), "127.0.0.1:5555", guest, 5)
        for state in ("offline", "unauthorized"):
            def refuse(command, **_kwargs):
                text = state if "get-state" in command else "connected"
                return subprocess.CompletedProcess(command, 0, stdout=text + "\n")

            with (
                mock.patch.object(boot_android.subprocess, "run", side_effect=refuse),
                mock.patch.object(boot_android.time, "sleep"),
                mock.patch.object(
                    boot_android.time, "monotonic", side_effect=(0.0, 0.5, 6.0)
                ),
                self.assertRaises(boot_android.LauncherError),
            ):
                boot_android.wait_for_adb(Path("adb"), "127.0.0.1:5555", guest, 5)

    class Guest:
        """A guest that exits with `status` once waited for."""

        pid = 123

        def __init__(self, status: int = 0) -> None:
            self.status = status
            self.exited = False
            self.waits = 0
            self.returncode: int | None = None

        def poll(self) -> int | None:
            return self.status if self.exited else None

        def wait(self, timeout: int | None = None) -> int:
            del timeout
            self.waits += 1
            self.exited = True
            self.returncode = self.status
            return self.status

    def run_main(
        self,
        root: Path,
        release: Path,
        instance: Path,
        extra: list[str] | None = None,
        *,
        guest: "BootAndroidTests.Guest | None" = None,
        booted: bool = True,
    ) -> tuple[int, "BootAndroidTests.Guest", mock.Mock]:
        emulator, adb = self.fake_tools(root)
        guest = guest or self.Guest()
        with (
            mock.patch.object(
                boot_android, "start_guest", return_value=guest
            ) as popen,
            mock.patch.object(boot_android, "ensure_loopback_port_available"),
            mock.patch.object(boot_android, "wait_for_adb") as wait_for_adb,
            mock.patch.object(
                boot_android, "wait_for_boot_completed", return_value=booted
            ),
            mock.patch.object(boot_android, "install_shutdown_handlers"),
            mock.patch.object(boot_android, "disconnect_adb"),
        ):
            result = boot_android.main(
                [
                    "--release", str(release),
                    "--instance", str(instance),
                    "--emulator", str(emulator),
                    "--adb", str(adb),
                    *(extra or []),
                ]
            )
        popen.wait_for_adb = wait_for_adb
        return result, guest, popen

    def test_normal_exit_disconnects_adb_and_reaps_guest(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            release = self.installed_release(root / "releases")
            emulator, adb = self.fake_tools(root)
            guest = self.Guest()
            with (
                mock.patch.object(
                    boot_android, "start_guest", return_value=guest
                ) as popen,
                mock.patch.object(boot_android, "ensure_loopback_port_available"),
                mock.patch.object(boot_android, "wait_for_adb"),
                mock.patch.object(
                    boot_android, "wait_for_boot_completed", return_value=True
                ),
                mock.patch.object(boot_android, "install_shutdown_handlers"),
                mock.patch.object(boot_android, "disconnect_adb") as disconnect,
            ):
                result = boot_android.main(
                    [
                        "--release", str(release),
                        "--instance", str(root / "instance"),
                        "--emulator", str(emulator),
                        "--adb", str(adb),
                    ]
                )
            self.assertEqual(result, 0)
            self.assertTrue(guest.exited)
            popen.assert_called_once()
            self.assertEqual(
                disconnect.call_args_list,
                [
                    mock.call(adb.resolve(), "127.0.0.1:5555"),
                    mock.call(adb.resolve(), "127.0.0.1:5555"),
                ],
            )
            command = popen.call_args.args[0]
            self.assertIn(
                f"--android-data-disk={(root / 'instance').resolve()}/data.img",
                command,
            )

    def test_provision_runs_after_boot_completed_only_when_asked(self) -> None:
        # The product starts a fresh guest in Setup Wizard; only a harness that
        # passes --provision gets the guest provisioned, and only once it has
        # booted.
        for extra, booted, calls in (
            ([], True, 0),
            (["--provision"], True, 1),
            (["--provision"], False, 0),
        ):
            with self.subTest(extra=extra, booted=booted), tempfile.TemporaryDirectory(
                prefix="android-launcher-"
            ) as temporary:
                root = Path(temporary)
                release = self.installed_release(root / "releases")
                with mock.patch.object(
                    boot_android.android_provision, "provision", return_value=True
                ) as provision:
                    result, _, _ = self.run_main(
                        root, release, root / "instance", extra, booted=booted
                    )
                self.assertEqual(result, 0)
                self.assertEqual(provision.call_count, calls)

    def test_a_guest_that_cannot_be_provisioned_ends_the_start(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            release = self.installed_release(root / "releases")
            error = boot_android.android_provision.ProvisionError("HOME is the wizard")
            with (
                mock.patch.object(
                    boot_android.android_provision, "provision", side_effect=error
                ),
                mock.patch.object(boot_android, "stop_guest") as stop,
            ):
                result, guest, _ = self.run_main(
                    root, release, root / "instance", ["--provision"]
                )
            self.assertEqual(result, 1)
            # The start ends: the guest is stopped instead of left to run.
            stop.assert_called_once_with(guest)
            self.assertEqual(guest.waits, 0)

    def test_a_restarting_guest_is_one_process_until_it_ends(self) -> None:
        # The emulator restarts the guest inside its process, so the launcher
        # starts it once and waits on that process alone. The wait returns the
        # status that ended the whole run, here the loop guard's 3.
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            release = self.installed_release(root / "releases")
            result, guest, popen = self.run_main(
                root, release, root / "instance", guest=self.Guest(3)
            )
            self.assertEqual(result, 3)
            popen.assert_called_once()
            popen.wait_for_adb.assert_called_once()
            self.assertEqual(guest.waits, 1)
            self.assertIn("--android-boot-mode=auto", popen.call_args.args[0])

    def test_a_new_instance_records_the_release_and_completes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            release = self.installed_release(root / "releases", "rel-a", build=100)
            instance = root / "instance"
            result, _guest, popen = self.run_main(root, release, instance)
            self.assertEqual(result, 0)
            state = json.loads((instance / "instance.json").read_text())
            self.assertEqual(state["release_id"], "rel-a")
            self.assertEqual(state["created_with"], "rel-a")
            self.assertEqual(state["build_incremental"], "100")
            self.assertEqual(state["last_completed"]["release_id"], "rel-a")
            self.assertIsNone(state["pending_update"])
            # A new disk asks for no capacity: the release's default applies.
            self.assertFalse(
                any("android-data-gib" in argument for argument in popen.call_args.args[0])
            )

    def test_a_start_that_never_booted_or_died_is_not_complete(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            release = self.installed_release(root / "releases", "rel-a", build=100)
            for label, booted, status in (
                ("no boot_completed", False, 0),
                ("a failing exit", True, 1),
            ):
                with self.subTest(label):
                    instance = root / label.replace(" ", "-")
                    result, _guest, _popen = self.run_main(
                        root, release, instance, booted=booted, guest=self.Guest(status)
                    )
                    self.assertEqual(result, status)
                    state = json.loads((instance / "instance.json").read_text())
                    self.assertIsNone(state["last_completed"])

    def test_a_newer_release_snapshots_and_the_snapshot_ends_with_a_clean_boot(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            releases = root / "releases"
            first = self.installed_release(releases, "rel-a", build=100)
            second = self.installed_release(releases, "rel-b", build=200)
            instance = root / "instance"
            self.run_main(root, first, instance)
            (instance / "data.img").write_bytes(b"guest data")

            # The second release's first boot does not complete.
            result, _guest, _popen = self.run_main(
                root, second, instance, booted=False
            )
            self.assertEqual(result, 0)
            state = json.loads((instance / "instance.json").read_text())
            self.assertEqual(state["release_id"], "rel-b")
            self.assertEqual(state["pending_update"]["to_release_id"], "rel-b")
            self.assertEqual(state["pending_update"]["from"]["release_id"], "rel-a")
            snapshot = instance / "data.img.pre-rel-b"
            self.assertEqual(snapshot.read_bytes(), b"guest data")

            # Nothing starts until the user decides.
            blocked, _guest, popen = self.run_main(root, second, instance)
            self.assertEqual(blocked, 1)
            popen.assert_not_called()

            # Restoring puts back the data the update started from.
            (instance / "data.img").write_bytes(b"damaged by the update")
            result, _guest, _popen = self.run_main(
                root, second, instance, ["--restore-snapshot"]
            )
            self.assertEqual(result, 0)
            state = json.loads((instance / "instance.json").read_text())
            self.assertIsNone(state["pending_update"])
            self.assertEqual(state["last_completed"]["release_id"], "rel-b")
            self.assertEqual((instance / "data.img").read_bytes(), b"guest data")
            self.assertFalse(snapshot.exists())

    def test_an_update_that_booted_is_not_rolled_back_by_the_next_release(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            releases = root / "releases"
            first = self.installed_release(releases, "rel-a", build=100)
            second = self.installed_release(releases, "rel-b", build=200)
            third = self.installed_release(releases, "rel-c", build=300)
            instance = root / "instance"
            self.run_main(root, first, instance)
            (instance / "data.img").write_bytes(b"guest data")

            # The update boots, then the run ends without a clean shutdown
            # (the host application was killed): the update still counts.
            result, _guest, _popen = self.run_main(
                root, second, instance, guest=self.Guest(1)
            )
            self.assertEqual(result, 1)
            state = json.loads((instance / "instance.json").read_text())
            self.assertIsNone(state["pending_update"])
            # Not a clean end: the last completed start is still rel-a's.
            self.assertEqual(state["last_completed"]["release_id"], "rel-a")
            self.assertFalse((instance / "data.img.pre-rel-b").exists())

            # The data has moved on; the next release updates from it.
            (instance / "data.img").write_bytes(b"data of the booted update")
            result, _guest, _popen = self.run_main(
                root, third, instance, booted=False
            )
            self.assertEqual(result, 0)
            state = json.loads((instance / "instance.json").read_text())
            self.assertEqual(state["pending_update"]["to_release_id"], "rel-c")
            self.assertEqual(state["pending_update"]["from"]["release_id"], "rel-b")
            self.assertEqual(
                (instance / "data.img").read_bytes(), b"data of the booted update"
            )
            self.assertEqual(
                (instance / "data.img.pre-rel-c").read_bytes(), b"data of the booted update"
            )
            self.assertFalse((instance / "data.img.pre-rel-b").exists())

    def test_retry_keeps_the_snapshot_until_a_clean_boot(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            releases = root / "releases"
            first = self.installed_release(releases, "rel-a", build=100)
            second = self.installed_release(releases, "rel-b", build=200)
            instance = root / "instance"
            self.run_main(root, first, instance)
            (instance / "data.img").write_bytes(b"guest data")
            self.run_main(root, second, instance, booted=False)
            snapshot = instance / "data.img.pre-rel-b"
            self.run_main(root, second, instance, ["--retry"], booted=False)
            self.assertTrue(snapshot.exists())
            self.run_main(root, second, instance, ["--retry"])
            self.assertFalse(snapshot.exists())
            state = json.loads((instance / "instance.json").read_text())
            self.assertIsNone(state["pending_update"])

    def test_a_downgrade_is_refused_before_the_guest_starts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            releases = root / "releases"
            newer = self.installed_release(releases, "rel-b", build=200)
            older = self.installed_release(releases, "rel-a", build=100)
            instance = root / "instance"
            self.run_main(root, newer, instance)
            (instance / "data.img").write_bytes(b"guest data")
            before = (instance / "instance.json").read_bytes()
            result, _guest, popen = self.run_main(root, older, instance)
            self.assertEqual(result, 1)
            popen.assert_not_called()
            self.assertEqual((instance / "instance.json").read_bytes(), before)
            self.assertEqual((instance / "data.img").read_bytes(), b"guest data")
            dry = boot_android.main([
                "--release", str(older), "--instance", str(instance), "--dry-run",
                "--adb", str(self.fake_tools(root)[1]),
            ])
            self.assertEqual(dry, 1)

            # Resetting is the user's way out.
            result, _guest, _popen = self.run_main(
                root, older, instance, ["--reset"]
            )
            self.assertEqual(result, 0)
            state = json.loads((instance / "instance.json").read_text())
            self.assertEqual(state["release_id"], "rel-a")
            self.assertFalse((instance / "data.img").exists())

    def test_dry_run_changes_nothing(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            release = self.installed_release(root / "releases")
            _emulator, adb = self.fake_tools(root)
            instance = root / "instance"
            result = boot_android.main([
                "--release", str(release), "--instance", str(instance),
                "--dry-run", "--adb", str(adb),
            ])
            self.assertEqual(result, 0)
            self.assertFalse(instance.exists())

    def test_the_emulator_leads_its_own_process_group(self) -> None:
        with mock.patch.object(boot_android.subprocess, "Popen") as popen:
            boot_android.start_guest(["emulator"])
        self.assertTrue(popen.call_args.kwargs["start_new_session"])

    def test_guest_that_leads_no_group_is_still_stopped(self) -> None:
        guest = mock.Mock()
        guest.pid = 123
        guest.poll.return_value = None
        guest.wait.side_effect = (
            subprocess.TimeoutExpired("guest", 10),
            subprocess.TimeoutExpired("guest", 5),
            0,
        )
        with mock.patch.object(
            boot_android.os, "killpg", side_effect=ProcessLookupError
        ):
            boot_android.stop_guest(guest)
        self.assertEqual(
            guest.send_signal.call_args_list,
            [
                mock.call(boot_android.signal.SIGINT),
                mock.call(boot_android.signal.SIGTERM),
                mock.call(boot_android.signal.SIGKILL),
            ],
        )
        self.assertEqual(guest.wait.call_count, 3)

    def test_a_killed_emulator_leaves_no_run_directories(self) -> None:
        with tempfile.TemporaryDirectory(prefix="android-launcher-") as temporary:
            root = Path(temporary)
            scratch = root / "tmp"
            scratch.mkdir()
            marker = root / "emulator.pid"
            # An emulator that ignores the stop requests, as a wedged one
            # does, so it is killed and no handler of its own runs.
            emulator = (
                "trap '' INT TERM; "
                'mkdir "$TMPDIR/uml-kern$$" "$TMPDIR/uml-guest-initrd-$$-x"; '
                'echo $$ > "$1"; exec sleep 30'
            )
            # Another run's directory is that run's.
            other = scratch / f"uml-kern{os.getpid()}"
            other.mkdir()
            stages = ((signal.SIGINT, 0.3), (signal.SIGTERM, 0.3))
            with (
                mock.patch.dict(os.environ, {"TMPDIR": str(scratch)}),
                mock.patch.object(tempfile, "tempdir", str(scratch)),
                mock.patch.object(boot_android, "GUEST_STOP_STAGES", stages),
            ):
                guest = subprocess.Popen(
                    ["/bin/sh", "-c", emulator, "emulator", str(marker)],
                    start_new_session=True,
                )
                try:
                    deadline = time.monotonic() + 5
                    while not marker.exists() or not marker.read_text():
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.01)
                    pid = int(marker.read_text())
                    self.assertTrue((scratch / f"uml-kern{pid}").is_dir())
                    boot_android.stop_guest(guest)
                finally:
                    if guest.poll() is None:
                        guest.kill()
                    guest.wait(timeout=5)
            self.assertEqual(guest.returncode, -signal.SIGKILL)
            self.assertEqual(
                sorted(path.name for path in scratch.iterdir()), [other.name]
            )


if __name__ == "__main__":
    unittest.main()
