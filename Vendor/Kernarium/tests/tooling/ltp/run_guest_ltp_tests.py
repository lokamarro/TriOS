#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for result parsing and LTP expectation classification."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import gzip
import io
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

LTP_ROOT = pathlib.Path(__file__).resolve().parents[2] / "integration/ltp"
sys.path.insert(0, str(LTP_ROOT))
import run_guest_ltp as runner  # noqa: E402
import guest as guest_support  # noqa: E402

# How long a test waits for a child process or a worker thread it started. The
# wait is a hard bound that only a failure reaches, so it outlasts a host under
# many times its normal load instead of being sized to the time a test needs.
WAIT_SECONDS = 60


class GuestConsoleWriteTests(unittest.TestCase):
    def test_console_write_retries_short_writes_until_complete(self) -> None:
        guest = guest_support.Guest.__new__(guest_support.Guest)
        guest._master = 17
        guest._input = mock.Mock()
        guest._input.fileno.return_value = 19
        guest._process = mock.Mock()
        guest._process.poll.return_value = None
        payload = b"overlay-bootstrap\n"
        received = bytearray()

        def short_write(descriptor: int, remaining: memoryview) -> int:
            self.assertEqual(descriptor, 19)
            count = min(3, len(remaining))
            received.extend(bytes(remaining[:count]))
            return count

        with mock.patch.object(
            guest_support.os, "write", side_effect=short_write
        ) as write_mock:
            guest._write(payload.decode())

        self.assertEqual(bytes(received), payload)
        self.assertGreater(write_mock.call_count, 1)

    def test_console_write_waits_after_nonblocking_backpressure(self) -> None:
        guest = guest_support.Guest.__new__(guest_support.Guest)
        guest._master = 23
        guest._input = mock.Mock()
        guest._input.fileno.return_value = 29
        guest._process = mock.Mock()
        guest._process.poll.return_value = None

        with (
            mock.patch.object(
                guest_support.os,
                "write",
                side_effect=(BlockingIOError(), len(b"command\n")),
            ) as write_mock,
            mock.patch.object(
                guest_support.select,
                "select",
                return_value=([23], [29], []),
            ) as select_mock,
            mock.patch.object(guest, "_drain") as drain_mock,
        ):
            guest._write("command\n")

        self.assertEqual(write_mock.call_count, 2)
        select_mock.assert_called_once()
        self.assertEqual(select_mock.call_args.args[:3], ([23], [29], []))
        drain_mock.assert_called_once()


class ResultParsingTests(unittest.TestCase):
    def test_repository_disabled_cases_are_not_verified(self) -> None:
        # The two manifests own their rows; a case belongs to one of them.
        disabled = runner.load_disabled_manifest(LTP_ROOT / "disabled-tests.tsv")
        verified = {
            case.key for case in runner.load_manifest(LTP_ROOT / "verified-tests.tsv")
        }
        self.assertTrue(disabled.keys().isdisjoint(verified))

    def test_disabled_manifest_records_design_omissions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "disabled.tsv"
            path.write_text(
                "# scenario\\ttest_id\\treason\n"
                "mm\tksm01\tKSM intentionally disabled\n",
                encoding="utf-8",
            )
            self.assertEqual(
                runner.load_disabled_manifest(path),
                {"mm/ksm01": "KSM intentionally disabled"},
            )

    def test_disabled_manifest_rejects_duplicate_cases(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "disabled.tsv"
            path.write_text(
                "mm\tksm01\tfirst\nmm\tksm01\tsecond\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "duplicate disabled case"):
                runner.load_disabled_manifest(path)

    def test_expectation_overrides_replace_only_named_cases(self) -> None:
        cases = [
            runner.Case("syscalls", "a", "a", "hardware-exempt", 60, "arm64"),
            runner.Case("syscalls", "b", "b", "pass", 60, "shared"),
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "expectations.tsv"
            path.write_text(
                "# scenario\ttest_id\texpectation\treason\n"
                "syscalls\ta\tpass\tx86-64 has the legacy syscall\n",
                encoding="utf-8",
            )
            overridden = runner.load_expectation_overrides(path, cases)
        self.assertEqual(
            [(case.expectation, case.reason) for case in overridden],
            [("pass", "x86-64 has the legacy syscall"), ("pass", "shared")],
        )

    def test_expectation_overrides_are_strict(self) -> None:
        cases = [runner.Case("syscalls", "a", "a", "pass", 60, "shared")]
        for body, message in (
            ("syscalls\tz\tpass\tr\n", "unknown case"),
            ("syscalls\ta\tpass\tr\n", "already expects"),
            ("syscalls\ta\tbogus\tr\n", "invalid expectation"),
            ("syscalls\ta\thardware-exempt\n", "four nonempty"),
            (
                "syscalls\ta\thardware-exempt\tr\n"
                "syscalls\ta\thardware-exempt\tr\n",
                "duplicate case",
            ),
        ):
            with self.subTest(message=message):
                with tempfile.TemporaryDirectory() as directory:
                    path = pathlib.Path(directory) / "expectations.tsv"
                    path.write_text(body, encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, message):
                        runner.load_expectation_overrides(path, cases)

    def test_repository_x86_64_manifests_match_the_shared_manifest(self) -> None:
        cases = runner.load_manifest(LTP_ROOT / "verified-tests.tsv")
        runner.load_expectation_overrides(
            LTP_ROOT / "x86_64/expectations.tsv", cases
        )
        shared = runner.load_disabled_manifest(LTP_ROOT / "disabled-tests.tsv")
        x86_64 = runner.load_disabled_manifest(
            LTP_ROOT / "x86_64/disabled-tests.tsv"
        )
        self.assertFalse(shared.keys() & x86_64.keys())

    def test_the_launcher_owns_the_root_device(self) -> None:
        arguments = guest_support.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.erofs"), "1G"
        )
        self.assertIn("--execution-engine=jit", arguments)
        self.assertIn("ubd0c=rootfs.erofs", arguments)
        for argument in arguments:
            self.assertNotIn(
                argument.split("=", 1)[0],
                ("root", "rootfstype", "init", "ip", "uml_ltp_workspace"),
            )

    def test_overlay_shell_session_prepares_only_disposable_state(self) -> None:
        # Every LTP guest is an overlay shell; its workspace, swap and cgroup
        # choices travel as session parameters, not kernel arguments.
        self.assertEqual(
            guest_support.session_parameters(
                overlay_shell=True,
                workspace="ext2",
                swap=True,
                cgroup1=None,
                cgroup2_nsdelegate=True,
            ).splitlines(),
            [
                "UML_SESSION_ENTRY=/bin/sh",
                "UML_SESSION_RESOLVER=user-network",
                "UML_SESSION_RUNTIME_MOUNTS=1",
                "UML_SESSION_NETWORK=user-network",
                "UML_SESSION_WORKSPACE=ext2",
                "UML_SESSION_SWAP=/dev/ubdb",
                "UML_SESSION_CGROUP=v2-nsdelegate",
            ],
        )
        session = guest_support.SESSION_SCRIPT.read_text(encoding="utf-8")
        for fragment in (
            "mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run",
            "mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /tmp",
            "mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /dev/shm",
            "mount -t devpts",
            "mount -t securityfs",
            "mount --make-rshared /",
            "ip link set lo up",
            "ip route add default via 10.0.2.2",
            "command -v mkfs.ext2",
            "mke2fs -q -t ext2",
            "mount -t ext2 -o noatime",
            "mount -t cgroup2 -o nsdelegate",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, session)
        self.assertLess(
            session.index("ip link set lo up"),
            session.index('echo "UML_SESSION_READY:$entry"'),
        )
        # A shell or systemd entry replaces the session as PID 1.
        self.assertIn('[ "$workload" = 1 ] || exec "$entry"', session)

    def test_modules_builtin_validation_is_canonical_and_exhaustive(self) -> None:
        entries = tuple(
            f"kernel/drivers/audit/driver_{index:03}.ko"
            for index in range(250)
        )
        data = ("\n".join(entries) + "\n").encode()
        self.assertEqual(runner.validate_modules_builtin(data), entries)
        invalid = (
            b"",
            b"kernel/drivers/net/veth.ko",
            b"/kernel/drivers/net/veth.ko\n",
            b"kernel/../veth.ko\n",
            b"kernel/drivers/net/veth.ko\n\n",
            b"kernel/drivers/net/veth.ko\nkernel/drivers/net/veth.ko\n",
            b"kernel/drivers/net/veth.o\n",
            b"\xff\n",
        )
        for value in invalid:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    runner.validate_modules_builtin(value)

    def test_modules_builtin_loader_accepts_the_kernel_artifact_layout(
        self,
    ) -> None:
        # KernelArtifact.cmake's UML_KERNEL_MODULES_BUILTIN is
        # <artifact>/kernel/modules.builtin, with kernel/kernel.release.
        with tempfile.TemporaryDirectory() as directory:
            kernel = pathlib.Path(directory) / "artifact/kernel"
            kernel.mkdir(parents=True)
            modules_builtin = kernel / "modules.builtin"
            data = b"kernel/fs/binfmt_misc.ko\n"
            modules_builtin.write_bytes(data)
            release = kernel / "kernel.release"
            release.write_text("6.18.54-uml-darwin\n", encoding="utf-8")
            self.assertEqual(
                runner.load_modules_builtin(modules_builtin),
                (data, "6.18.54-uml-darwin"),
            )
            release.write_text("6.18.54\n6.18.54\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "invalid sibling"):
                runner.load_modules_builtin(modules_builtin)
            release.write_text("bad release\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "invalid sibling"):
                runner.load_modules_builtin(modules_builtin)
            release.unlink()
            with self.assertRaisesRegex(
                ValueError, "cannot read sibling kernel release"
            ):
                runner.load_modules_builtin(modules_builtin)

    def test_ltp_builtin_driver_payloads_carry_complete_kbuild_output(
        self,
    ) -> None:
        entries = (
            "kernel/fs/binfmt_misc.ko",
            "kernel/drivers/net/veth.ko",
        ) + tuple(
            f"kernel/drivers/audit/driver_{index:03}.ko"
            for index in range(248)
        )
        data = ("\n".join(entries) + "\n").encode()
        payloads = runner.ltp_builtin_driver_payloads(data, "test-release")
        modules = "/usr/lib/modules/test-release"
        tools = "/usr/local/libexec/uml-ltp-tools"
        self.assertEqual(
            set(payloads),
            {
                f"{modules}/modules.builtin",
                f"{modules}/modules.dep",
                f"{tools}/kmod",
                f"{tools}/modprobe",
                f"{tools}/rmmod",
                runner.LTP_METADATA_CHECK,
            },
        )
        for path in payloads:
            guest_support.validate_payload_path(path)
        self.assertEqual(payloads[f"{modules}/modules.builtin"].contents, data)
        self.assertEqual(payloads[f"{modules}/modules.dep"].contents, b"")
        shim = payloads[f"{tools}/kmod"]
        self.assertEqual(shim.mode, 0o755)
        for tool in ("modprobe", "rmmod"):
            self.assertEqual(payloads[f"{tools}/{tool}"], shim)
        self.assertIn(b"driver_247) ;;", shim.contents)
        self.assertEqual(payloads[runner.LTP_METADATA_CHECK].mode, 0o755)
        self.assertIn(
            b"expected_kernel_release=test-release",
            payloads[runner.LTP_METADATA_CHECK].contents,
        )
        self.assertTrue(
            runner.ltp_guest_command("true").find(f"PATH={tools}:") >= 0
        )
        for invalid in ((b"kernel/x.ko\n", "bad release"), (b"", "r")):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                runner.ltp_builtin_driver_payloads(*invalid)

    def install_metadata(
        self,
        root: pathlib.Path,
        data: bytes,
        release: str = "test-release",
        config_text: str | None = "# CONFIG_MODULES is not set\n",
    ) -> tuple[pathlib.Path, pathlib.Path]:
        """Lay the payloads out under root, with host paths for the guest's."""
        modules_root = root / "lib/modules"
        (modules_root / release).mkdir(parents=True)
        (modules_root / release / "modules.builtin").write_bytes(data)
        (modules_root / release / "modules.dep").write_bytes(b"")
        config = root / "config.gz"
        if config_text is not None:
            with gzip.open(config, "wt", encoding="utf-8") as output:
                output.write(config_text)
        tools = root / "tools"
        tools.mkdir()
        names = tuple(
            entry.rsplit("/", 1)[-1].removesuffix(".ko")
            for entry in runner.validate_modules_builtin(data)
        )
        shim = runner.ltp_kmod_shim(
            names, release, str(modules_root), str(config), ("gzip", "-cd")
        )
        for tool in ("kmod", "modprobe", "rmmod"):
            (tools / tool).write_text(shim, encoding="utf-8")
            (tools / tool).chmod(0o755)
        return modules_root, config

    def run_metadata_check(
        self,
        modules_root: pathlib.Path,
        config: pathlib.Path,
        running_release: str = "test-release",
        config_reader: tuple[str, ...] = ("gzip", "-cd"),
    ) -> subprocess.CompletedProcess[str]:
        script = runner.ltp_metadata_check(
            "test-release",
            str(modules_root),
            str(config),
            config_reader,
            ("printf", running_release),
        )
        return subprocess.run(
            ["/bin/sh", "-c", script],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_ltp_kmod_shim_accepts_only_built_in_modprobe(self) -> None:
        data = (
            b"kernel/fs/binfmt_misc.ko\nkernel/fs/quota/quota_v2.ko\n"
            b"kernel/drivers/block/zram/zram.ko\nkernel/drivers/net/veth.ko\n"
            b"kernel/net/netfilter/nf_tables.ko\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            modules_root, config = self.install_metadata(root, data)
            tools = root / "tools"

            def run_tool(
                name: str, *arguments: str
            ) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    [str(tools / name), *arguments],
                    check=False,
                    capture_output=True,
                    text=True,
                )

            accepted = (
                ("modprobe", "binfmt_misc"),
                ("modprobe", "-a", "binfmt_misc", "veth"),
                ("modprobe", "--all", "quota_v2", "zram"),
                ("kmod", "modprobe", "nf_tables"),
            )
            for invocation in accepted:
                with self.subTest(invocation=invocation):
                    result = run_tool(*invocation)
                    self.assertEqual(result.returncode, 0, result.stderr)

            rejected = (
                ("modprobe",),
                ("modprobe", "geneve"),
                ("modprobe", "kernel/fs/binfmt_misc.ko"),
                ("modprobe", "binfmt_misc", "veth"),
                ("modprobe", "zram", "num_devices=2"),
                ("modprobe", "-q", "binfmt_misc"),
                ("modprobe", "-r", "binfmt_misc"),
                ("modprobe", "--remove", "binfmt_misc"),
                ("rmmod", "zram"),
                ("kmod", "rmmod", "zram"),
                ("kmod", "depmod"),
            )
            for invocation in rejected:
                with self.subTest(invocation=invocation):
                    self.assertNotEqual(run_tool(*invocation).returncode, 0)

            builtin = modules_root / "test-release/modules.builtin"
            with builtin.open("a", encoding="utf-8") as output:
                output.write("kernel/drivers/net/geneve.ko\n")
            self.assertNotEqual(run_tool("modprobe", "geneve").returncode, 0)
            for text in (
                "# CONFIG_MODULES is not set\nCONFIG_MODULES=y\n",
                "CONFIG_MODULES=y\n",
            ):
                with gzip.open(config, "wt", encoding="utf-8") as output:
                    output.write(text)
                self.assertNotEqual(
                    run_tool("modprobe", "binfmt_misc").returncode, 0
                )
            config.unlink()
            self.assertNotEqual(run_tool("modprobe", "binfmt_misc").returncode, 0)
            with gzip.open(config, "wt", encoding="utf-8") as output:
                output.write("# CONFIG_MODULES is not set\n")
            builtin.unlink()
            self.assertNotEqual(run_tool("modprobe", "binfmt_misc").returncode, 0)

    def test_ltp_metadata_check_accepts_only_the_matching_static_kernel(
        self,
    ) -> None:
        data = b"kernel/drivers/net/veth.ko\n"
        with tempfile.TemporaryDirectory() as directory:
            modules_root, config = self.install_metadata(
                pathlib.Path(directory), data
            )
            completed = self.run_metadata_check(modules_root, config)
            self.assertEqual(completed.returncode, 0, completed.stderr)

            completed = self.run_metadata_check(
                modules_root, config, running_release="wrong-release"
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("kernel release mismatch", completed.stderr)

            completed = self.run_metadata_check(
                modules_root, config, config_reader=("false",)
            )
            self.assertNotEqual(completed.returncode, 0)

            for text in (
                "# CONFIG_MODULES is not set\nCONFIG_MODULES=y\n",
                "CONFIG_MODULES=y\n",
            ):
                with self.subTest(config=text):
                    with gzip.open(config, "wt", encoding="utf-8") as output:
                        output.write(text)
                    completed = self.run_metadata_check(modules_root, config)
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertIn("requires CONFIG_MODULES=n", completed.stderr)
            with gzip.open(config, "wt", encoding="utf-8") as output:
                output.write("# CONFIG_MODULES is not set\n")

            # A payload LTP cannot see at /lib/modules, as on an image whose
            # /lib is not the link to /usr/lib the payload path assumes.
            (modules_root / "test-release/modules.dep").unlink()
            completed = self.run_metadata_check(modules_root, config)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("LTP cannot see", completed.stderr)

    def test_ltp_boot_checks_the_driver_metadata(self) -> None:
        guest = object.__new__(runner.Guest)
        with (
            mock.patch.object(runner.GuestHarness, "boot_and_login") as boot,
            mock.patch.object(
                guest,
                "run_command",
                return_value=guest_support.CommandResult(0, ""),
            ) as run_command,
        ):
            guest.boot_and_login(17)
        boot.assert_called_once_with(17)
        run_command.assert_called_once_with(runner.LTP_METADATA_CHECK, 17)

        with (
            mock.patch.object(runner.GuestHarness, "boot_and_login"),
            mock.patch.object(
                guest,
                "run_command",
                return_value=guest_support.CommandResult(1, "kernel release mismatch"),
            ),
            self.assertRaisesRegex(
                RuntimeError, "metadata check failed: kernel release mismatch"
            ),
        ):
            guest.boot_and_login()

    def test_prompt_patterns_match_the_bytes_a_real_console_emits(self) -> None:
        """Pin the login prompt matchers against transcripts.

        The Steam boot bench logs in as uml and execs a root shell through
        sudo. bash emits its bracketed-paste reset and a bare carriage return
        when it execs a replacement shell, and `[^\\r\\n]*` may not cross that
        carriage return, so a matcher must be proven against real bytes.
        """
        root_prompt = re.compile(guest_support.LINE_START + rb"[^\r\n]*# $")
        user_prompt = re.compile(guest_support.LINE_START + rb"[^\r\n]*\$ $")

        exec_replacement_shell = (
            b" sudo -n /bin/sh\r\n"
            b"\x1b[?2004huml@debian-rootfs:~$ exec sudo -n /bin/sh\r\n"
            b"\x1b[?2004l\r# "
        )
        self.assertIsNotNone(root_prompt.search(exec_replacement_shell))

        for transcript in (
            b"\r\n# ",
            b"\n# ",
            b"# ",
            b"\r\n\x1b[?2004l\rdebian-rootfs:~# ",
        ):
            with self.subTest(transcript=transcript):
                self.assertIsNotNone(root_prompt.search(transcript))

        self.assertIsNotNone(
            user_prompt.search(b"\r\n\x1b[?2004h\ruml@debian-rootfs:~$ ")
        )

        # The anchor must still not match a prompt buried mid-line, which is
        # what the `[^\r\n]*` bound is there to prevent.
        self.assertIsNone(root_prompt.search(b"\r\nnot a prompt # yet\r\n"))

    def test_guest_boots_exactly_one_shell_mode(self) -> None:
        for modes in (
            {"init_shell": True, "overlay_shell": True},
            {},
        ):
            with (
                self.subTest(modes=modes),
                self.assertRaisesRegex(ValueError, "exactly one"),
            ):
                runner.Guest(
                    pathlib.Path("uml-darwin"),
                    pathlib.Path("rootfs.erofs"),
                    "1G",
                    "tmpfs",
                    **modes,
                )

    def test_guest_termination_signals_the_child_directly(self) -> None:
        # The emulator shares this runner's process group, so terminate must
        # signal the child alone. A group signal here would reach the runner
        # itself and kill the harness along with the guest.
        guest = object.__new__(runner.Guest)
        guest._process = mock.Mock(pid=1234)
        guest._process.poll.return_value = None
        with mock.patch.object(runner.os, "killpg") as killpg:
            guest.terminate()
        killpg.assert_not_called()
        guest._process.send_signal.assert_called_once_with(runner.signal.SIGKILL)

    def test_ext2_workspace_is_opt_in(self) -> None:
        for workspace in ("tmpfs", "ext2"):
            with self.subTest(workspace=workspace):
                session = guest_support.session_parameters(
                    overlay_shell=True,
                    workspace=workspace,
                    swap=False,
                    cgroup1=None,
                    cgroup2_nsdelegate=False,
                )
                self.assertIn(f"UML_SESSION_WORKSPACE={workspace}\n", session)

    def test_kernel_arguments_are_appended(self) -> None:
        arguments = guest_support.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.erofs"),
            "1G", kernel_arguments=("ncpus=2",),
        )
        self.assertEqual(arguments[-1], "ncpus=2")

    def test_disposable_swap_disk_is_separate_from_rootfs(self) -> None:
        arguments = guest_support.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.erofs"),
            "1G", swap_path=pathlib.Path("/tmp/swap.img"),
        )
        self.assertIn("ubd0c=rootfs.erofs", arguments)
        self.assertIn("ubd1=/tmp/swap.img", arguments)

    def test_swap_size_uses_binary_suffixes(self) -> None:
        self.assertEqual(runner.size_bytes("1G"), 1 << 30)
        self.assertEqual(runner.size_bytes("4K"), 4096)
        with self.assertRaises(ValueError):
            runner.size_bytes("3GB")

    def test_legacy_cgroup_controller_name_is_restricted(self) -> None:
        self.assertIsNotNone(runner.CGROUP1_CONTROLLER.fullmatch("memory"))
        self.assertIsNone(runner.CGROUP1_CONTROLLER.fullmatch("memory;reboot"))

    def test_manifest_parses_cgroup2_nsdelegate_resource(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "controllers\tcgroup_core02\tcgroup_core02\tpass\t120\t"
                "namespace delegation\tcgroup2=nsdelegate\n",
                encoding="utf-8",
            )
            case = runner.load_manifest(path)[0]
            self.assertTrue(case.cgroup2_nsdelegate)
            self.assertIsNone(case.cgroup1)

    def test_manifest_rejects_mixed_cgroup_versions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "controllers\tmixed\ttrue\tpass\t120\tinvalid\t"
                "cgroup1=memory\tcgroup2=nsdelegate\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "mutually exclusive"):
                runner.load_manifest(path)

    def test_manifest_rejects_duplicate_cgroup2_resource(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "controllers\tduplicate\ttrue\tpass\t120\tinvalid\t"
                "cgroup2=nsdelegate\tcgroup2=nsdelegate\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "duplicate"):
                runner.load_manifest(path)

    def test_manifest_parses_serial_option(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "syscalls\ttiming\ttrue\tpass\t120\ttiming fidelity\t"
                "serial\n",
                encoding="utf-8",
            )
            case = runner.load_manifest(path)[0]
            self.assertTrue(case.serial)
            self.assertFalse(case.host_pressure)

    def test_manifest_rejects_duplicate_or_mixed_serial_options(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "syscalls\tduplicate\ttrue\tpass\t120\tinvalid\t"
                "serial\tserial\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "duplicate"):
                runner.load_manifest(path)
            path.write_text(
                "syscalls\tmixed\ttrue\tpass\t120\tinvalid\t"
                "serial\thost-pressure\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "mutually exclusive"):
                runner.load_manifest(path)

    def test_manifest_parses_per_case_kernel_arguments(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "hugetlb\tlarge\tlarge\tpass\t120\tlarge page\t"
                "kernel-arg=hugepagesz=1G\tkernel-arg=ncpus=4\n",
                encoding="utf-8",
            )
            case = runner.load_manifest(path)[0]
            self.assertEqual(
                case.kernel_arguments, ("hugepagesz=1G", "ncpus=4")
            )

    def test_manifest_rejects_invalid_kernel_arguments(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "verified.tsv"
            path.write_text(
                "hugetlb\tempty\ttrue\tpass\t120\tinvalid\tkernel-arg=\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "empty kernel argument"):
                runner.load_manifest(path)
            path.write_text(
                "hugetlb\tduplicate\ttrue\tpass\t120\tinvalid\t"
                "kernel-arg=ncpus=4\tkernel-arg=ncpus=4\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "duplicate kernel argument"):
                runner.load_manifest(path)

    def test_run_case_forwards_kernel_arguments(self) -> None:
        case = runner.Case(
            "hugetlb", "large", "large", "pass", 120, "large page",
            kernel_arguments=("hugepagesz=1G",),
        )
        expected = runner.Result(case, 0, {"passed": 1}, "")
        with mock.patch.object(runner, "Guest") as guest_class:
            guest_class.return_value.run.return_value = expected
            metadata = {"/usr/local/libexec/uml-test": runner.GuestFile(b"")}
            actual = runner.run_case(
                pathlib.Path("emulator"), pathlib.Path("rootfs"), "4G", case,
                driver_metadata=metadata,
            )
        self.assertEqual(actual, expected)
        guest_class.assert_called_once_with(
            pathlib.Path("emulator"), pathlib.Path("rootfs"), "4G", "tmpfs",
            kernel_arguments=("hugepagesz=1G",), swap_size=None, cgroup1=None,
            cgroup2_nsdelegate=False, overlay_shell=True, engine="jit",
            payloads=metadata,
        )

    def test_execution_engine_is_explicitly_selected(self) -> None:
        arguments = guest_support.emulator_arguments(
            pathlib.Path("uml-darwin"), pathlib.Path("rootfs.erofs"), "1G",
            engine="gadgets",
        )
        self.assertEqual(arguments[1], "--execution-engine=gadgets")

    def test_summary_is_authoritative(self) -> None:
        output = """case.c:1: TPASS: passed

Summary:
passed   7
failed   0
broken   0
skipped  1
warnings 0
"""
        self.assertEqual(
            runner.parse_result_counts(output),
            {"passed": 7, "failed": 0, "broken": 0, "skipped": 1,
             "warnings": 0},
        )

    def test_uppercase_summary_is_authoritative(self) -> None:
        output = """SUMMARY:
PASSED   2
FAILED   0
BROKEN   0
SKIPPED  0
WARNINGS 0
"""
        self.assertEqual(
            runner.parse_result_counts(output),
            {"passed": 2, "failed": 0, "broken": 0, "skipped": 0,
             "warnings": 0},
        )

    def test_a_timed_out_case_keeps_its_partial_output(self) -> None:
        guest = object.__new__(runner.Guest)
        guest.run_command = mock.Mock(
            side_effect=runner.CommandTimeout(
                "guest operation exceeded 5s", "partial output"
            )
        )
        case = runner.Case("syscalls", "slow01", "slow01", "pass", 5, "")
        result = guest.run(case)
        self.assertEqual(
            result,
            runner.Result(
                case, None, {}, "partial output", "guest operation exceeded 5s"
            ),
        )
        guest.run_command.assert_called_once_with(
            runner.ltp_guest_command("slow01"), 5
        )

    def test_a_jit_scales_case_bounds_and_the_gadgets_keep_them(self) -> None:
        case = runner.Case("kernel_misc", "zram01", "zram01.sh", "pass", 600, "")
        # At least the measured exec-heavy ratio and a quarter more.
        for guest_arch, minimum in (("arm64", 5), ("x86_64", 4)):
            with self.subTest(guest_arch=guest_arch, engine="jit"):
                factor = runner.JIT_CASE_BOUND_FACTORS[guest_arch]
                self.assertGreaterEqual(factor, minimum)
                jit = runner.bound_for_engine(case, guest_arch, "jit")
                self.assertEqual(jit.engine, "jit")
                self.assertEqual(jit.timeout_seconds, 600 * factor)
                self.assertEqual(
                    jit.ltp_timeout_mul, runner.LTP_TIMEOUT_MUL * factor
                )
            with self.subTest(guest_arch=guest_arch, engine="gadgets"):
                gadgets = runner.bound_for_engine(case, guest_arch, "gadgets")
                self.assertEqual(
                    gadgets, runner.dataclasses.replace(case, engine="gadgets")
                )
        self.assertEqual(
            set(runner.JIT_CASE_BOUND_FACTORS), set(runner.GUEST_ARCHITECTURES)
        )

    def test_a_case_runs_under_its_own_bound_and_ltp_timers(self) -> None:
        guest = object.__new__(runner.Guest)
        guest.run_command = mock.Mock(
            side_effect=runner.CommandTimeout("exceeded", "")
        )
        case = runner.bound_for_engine(
            runner.Case("kernel_misc", "zram01", "zram01.sh", "pass", 600, ""),
            "arm64", "jit",
        )
        guest.run(case)
        command, bound = guest.run_command.call_args.args
        self.assertEqual(bound, case.timeout_seconds)
        self.assertIn(f"LTP_TIMEOUT_MUL={case.ltp_timeout_mul} ", command)
        self.assertNotIn(f"LTP_TIMEOUT_MUL={runner.LTP_TIMEOUT_MUL} ", command)

    def test_a_finished_case_reports_counts_and_guest_health(self) -> None:
        output = "SUMMARY:\nPASSED   1\nFAILED   0\n"
        case = runner.Case("syscalls", "quick01", "quick01", "pass", 5, "")
        for health, expected_error in (
            (0, None),
            (
                1,
                "post-test guest health check failed: "
                "zcat /proc/config.gz exited 1",
            ),
        ):
            guest = object.__new__(runner.Guest)
            guest.run_command = mock.Mock(
                side_effect=(
                    guest_support.CommandResult(0, output),
                    guest_support.CommandResult(health, ""),
                )
            )
            result = guest.run(case)
            self.assertEqual(result.return_code, 0)
            self.assertEqual(result.summary, {"passed": 1, "failed": 0})
            self.assertEqual(result.output, output)
            self.assertEqual(result.error, expected_error)

    def test_protocol_marker_tolerates_tty_case_conversion(self) -> None:
        pattern = guest_support.marker_pattern("__UML_LTP_END_deadbeef__")
        match = pattern.search(b"__UML_LTP_END_DEADBEEF__:0\r\n")
        self.assertIsNotNone(match)
        self.assertEqual(guest_support.marker_status(match), 0)

    def test_protocol_marker_survives_interleaved_kernel_lines(self) -> None:
        # The console shows whole kernel lines wherever the shell's output
        # stopped short: between a status and its line end, where the
        # terminal driver splits every line, or anywhere a full console
        # buffer cut it.
        pattern = guest_support.marker_pattern("__UML_LTP_END_deadbeef__")
        kernel = (
            b"[    2.244317] virtio-video virtio0: failed to get stream bitrate"
            b"\r\n"
        )
        for console in (
            b"__UML_LTP_END_deadbeef__:12" + kernel + b"\r\n",
            b"__UML_LTP_END_dead" + kernel + b"beef__:" + kernel + b"12\r\n",
            b"__UML_LTP_END_deadbeef__:1" + kernel + kernel + b"2\r\n",
        ):
            with self.subTest(console=console):
                match = pattern.search(console)
                self.assertIsNotNone(match)
                self.assertEqual(guest_support.marker_status(match), 12)
        # A status whose line has not ended does not match yet, and neither
        # does one that other text follows.
        for console in (
            b"__UML_LTP_END_deadbeef__:12",
            b"__UML_LTP_END_deadbeef__:1" + kernel,
            b"__UML_LTP_END_deadbeef__:12 more\r\n",
        ):
            with self.subTest(console=console):
                self.assertIsNone(pattern.search(console))

    def test_case_runs_from_ltp_binary_directory(self) -> None:
        command = runner.ltp_guest_command("./helper.sh 'argument'")
        self.assertIn(
            "mkdir -p /tmp/uml-ltp-case "
            "/tmp/uml-ltp-output &&",
            command,
        )
        self.assertIn(
            "chmod 1777 /tmp/uml-ltp-case "
            "/tmp/uml-ltp-output &&",
            command,
        )
        self.assertIn(
            "cd /opt/ltp/testcases/bin &&", command
        )
        self.assertIn(
            "PATH=/usr/local/libexec/uml-ltp-tools:/opt/ltp/testcases/bin:$PATH",
            command,
        )
        self.assertIn("LTPROOT=/opt/ltp", command)
        self.assertIn("TMPDIR=/tmp/uml-ltp-case", command)
        self.assertIn("LTP_TIMEOUT_MUL=4", command)

    def test_early_tconf_without_summary_is_counted(self) -> None:
        output = "tst_test.c:1417: TCONF: This arch 'aarch64' is not supported\n"
        self.assertEqual(runner.parse_result_counts(output), {"skipped": 1})

    def test_legacy_result_without_summary_is_counted(self) -> None:
        output = """clone02     0  TINFO  :  setting up
clone02     1  TPASS  :  Test Passed
clone02     2  TPASS  :  Test Passed
"""
        self.assertEqual(runner.parse_result_counts(output), {"passed": 2})

    def test_legacy_result_without_space_before_colon_is_counted(self) -> None:
        output = "cpuset_base_ops 1 TCONF: The total of nodes is less than 2\n"
        self.assertEqual(runner.parse_result_counts(output), {"skipped": 1})

    def test_legacy_child_result_without_summary_is_counted(self) -> None:
        output = "setfsuid04  0  TINFO  :  Child process returned TPASS\n"
        self.assertEqual(runner.parse_result_counts(output), {"passed": 1})

    def test_kernel_bug_invalidates_nominal_userspace_pass(self) -> None:
        output = """mmap1.c:246: TPASS: System survived.
BUG: Bad page cache in process mmap1 pfn:06030
page dumped because: still mapped when deleted
"""
        self.assertEqual(
            runner.kernel_failure(output),
            "guest kernel reported a fatal diagnostic: "
            "BUG: Bad page cache in process mmap1 pfn:06030",
        )

    def test_normal_ltp_output_has_no_kernel_failure(self) -> None:
        self.assertIsNone(runner.kernel_failure("case.c:1: TPASS: passed\n"))

    def test_early_architecture_exemption_is_accepted(self) -> None:
        case = runner.Case(
            "syscalls", "arch_prctl01", "arch_prctl01",
            "hardware-exempt", 60, "x86-only test",
        )
        result = runner.Result(case, 32, {"skipped": 1}, "")
        self.assertTrue(runner.accepted(result))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"passed": 7, "skipped": 1}, "cannot fork")
        ))

    def test_hardware_failure_exemption_requires_assertion_failure(self) -> None:
        case = runner.Case(
            "irq", "irqbalance01", "irqbalance01",
            "hardware-failure-exempt", 60, "no physical IRQ router",
        )
        self.assertTrue(runner.accepted(
            runner.Result(case, 1, {"failed": 1, "broken": 0}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"passed": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 32, {"skipped": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"failed": 1, "broken": 1}, "")
        ))

    def test_obsolete_userspace_case_is_accepted_as_not_applicable(self) -> None:
        case = runner.Case(
            "syscalls", "creat07", "creat07",
            "userspace-exempt", 60, "obsolete since Linux 6.11",
        )
        result = runner.Result(case, 32, {"skipped": 1}, "")
        self.assertTrue(runner.accepted(result))

    def test_explicit_silent_userspace_case_is_accepted(self) -> None:
        case = runner.Case(
            "syscalls", "fcntl01", "fcntl01",
            "userspace-exempt", 60, "upstream success path emits no result",
        )
        result = runner.Result(case, 0, {}, "")
        self.assertTrue(runner.accepted(result))

    def test_silent_result_is_not_accepted_as_a_pass(self) -> None:
        case = runner.Case(
            "syscalls", "silent", "silent", "pass", 60, "",
        )
        result = runner.Result(case, 0, {}, "")
        self.assertFalse(runner.accepted(result))

    def test_legacy_exit_zero_expectation_accepts_silent_success(self) -> None:
        case = runner.Case(
            "sched", "time-schedule01", "time-schedule",
            "exit-zero", 60, "legacy benchmark has no LTP result tags",
        )
        self.assertTrue(runner.accepted(runner.Result(case, 0, {}, "")))
        self.assertFalse(runner.accepted(runner.Result(case, 1, {}, "")))
        self.assertFalse(
            runner.accepted(runner.Result(case, 0, {"failed": 1}, ""))
        )

    def test_pass_with_ltp_warning_status_is_accepted(self) -> None:
        case = runner.Case(
            "syscalls", "stress", "stress", "pass", 60, "",
        )
        result = runner.Result(
            case, 4, {"passed": 1, "failed": 0, "broken": 0,
                      "skipped": 0, "warnings": 1}, "",
        )
        self.assertTrue(runner.accepted(result))

    def test_warning_status_without_warning_or_pass_is_rejected(self) -> None:
        case = runner.Case(
            "syscalls", "stress", "stress", "pass", 60, "",
        )
        self.assertFalse(runner.accepted(
            runner.Result(case, 4, {"passed": 1, "warnings": 0}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 4, {"passed": 0, "warnings": 1}, "")
        ))

    def test_missing_packaged_binary_is_accepted_explicitly(self) -> None:
        case = runner.Case(
            "syscalls", "missing", "missing",
            "userspace-missing-exempt", 60, "not packaged for this ABI",
        )
        result = runner.Result(case, 127, {}, "missing: not found")
        self.assertTrue(runner.accepted(result))

    def test_missing_packaged_binary_contract_is_strict(self) -> None:
        case = runner.Case(
            "syscalls", "missing", "missing",
            "userspace-missing-exempt", 60, "not packaged for this ABI",
        )
        self.assertFalse(runner.accepted(runner.Result(case, 0, {}, "")))
        self.assertFalse(
            runner.accepted(runner.Result(case, 127, {"broken": 1}, ""))
        )

    def test_userspace_setup_break_is_accepted_explicitly(self) -> None:
        case = runner.Case(
            "containers", "netns_netlink", "netns_netlink",
            "userspace-broken-exempt", 60, "BusyBox ip lacks link add",
        )
        result = runner.Result(
            case, 2,
            {"passed": 0, "failed": 0, "broken": 1, "skipped": 0},
            "BusyBox ip usage followed by an LTP setup TBROK",
        )
        self.assertTrue(runner.accepted(result))

    def test_userspace_setup_break_contract_is_strict(self) -> None:
        case = runner.Case(
            "containers", "netns_netlink", "netns_netlink",
            "userspace-broken-exempt", 60, "BusyBox ip lacks link add",
        )
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"broken": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"passed": 1, "broken": 1}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 2, {"failed": 1, "broken": 1}, "")
        ))

    def test_variable_userspace_matrix_requires_a_passed_check(self) -> None:
        case = runner.Case(
            "controllers", "legacy", "legacy",
            "userspace-variable-matrix-exempt", 60,
            "obsolete mode produces timing-dependent result ordering",
        )
        for summary in (
            {"passed": 6, "failed": 3, "broken": 0, "skipped": 0},
            {"passed": 1, "failed": 0, "broken": 1, "skipped": 0},
            {"passed": 3, "failed": 1, "broken": 1, "skipped": 0},
            {"passed": 3, "failed": 4, "broken": 0, "skipped": 1},
        ):
            self.assertTrue(runner.accepted(runner.Result(case, 1, summary, "")))
        self.assertTrue(runner.accepted(runner.Result(
            case, 4,
            {
                "passed": 5,
                "failed": 0,
                "broken": 0,
                "skipped": 0,
                "warnings": 1,
            },
            "",
        )))
        self.assertTrue(runner.accepted(
            runner.Result(case, 0, {"passed": 1}, "")
        ))
        for return_code, summary in (
            (1, {"failed": 1}),
            (2, {"broken": 1}),
            (32, {"skipped": 1}),
        ):
            self.assertFalse(
                runner.accepted(runner.Result(case, return_code, summary, ""))
            )

    def test_static_kernel_module_case_is_accepted_as_not_applicable(self) -> None:
        case = runner.Case(
            "syscalls", "delete_module01", "delete_module01",
            "static-kernel-exempt", 60, "loadable modules intentionally disabled",
        )
        result = runner.Result(case, 32, {"skipped": 1}, "")
        self.assertTrue(runner.accepted(result))

    def test_known_stale_userspace_failure_requires_a_real_failure(self) -> None:
        case = runner.Case(
            "syscalls", "ioctl_pidfd06", "ioctl_pidfd06",
            "userspace-failure-exempt", 60,
            "installed LTP expects the pre-backport errno",
        )
        stale = runner.Result(case, 1, {"failed": 1, "broken": 0}, "")
        fixed = runner.Result(case, 0, {"passed": 1, "failed": 0}, "")
        broken = runner.Result(case, 2, {"failed": 1, "broken": 1}, "")
        self.assertTrue(runner.accepted(stale))
        self.assertFalse(runner.accepted(fixed))
        self.assertFalse(runner.accepted(broken))

    def test_legacy_zero_exit_matrix_requires_mixed_results(self) -> None:
        case = runner.Case(
            "controllers", "legacy", "legacy",
            "userspace-legacy-matrix-exempt", 60,
            "legacy wrapper always exits zero",
        )
        self.assertTrue(runner.accepted(
            runner.Result(case, 0, {"passed": 14, "failed": 9}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"passed": 14}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 0, {"failed": 9}, "")
        ))
        self.assertFalse(runner.accepted(
            runner.Result(
                case, 0, {"passed": 14, "failed": 9, "broken": 1}, ""
            )
        ))
        self.assertFalse(runner.accepted(
            runner.Result(case, 1, {"passed": 14, "failed": 9}, "")
        ))

    def test_flaky_userspace_exemption_accepts_only_known_outcomes(self) -> None:
        case = runner.Case(
            "controllers", "memcg_usage_in_bytes",
            "memcg_usage_in_bytes_test.sh",
            "userspace-flaky-exempt", 60,
            "kernel charge movement can trip the byte-exact helper delta",
        )
        warning = runner.Result(
            case, 4,
            {"passed": 4, "failed": 0, "broken": 0,
             "skipped": 0, "warnings": 1},
            "",
        )
        partial = runner.Result(
            case, 6,
            {"passed": 1, "failed": 0, "broken": 1, "skipped": 0},
            "",
        )
        clean = runner.Result(
            case, 0,
            {"passed": 4, "failed": 0, "broken": 0,
             "skipped": 0, "warnings": 0},
            "",
        )
        self.assertTrue(runner.accepted(clean))
        self.assertTrue(runner.accepted(warning))
        self.assertTrue(runner.accepted(partial))
        self.assertFalse(runner.accepted(
            runner.Result(case, 7, {"passed": 1, "failed": 1, "broken": 1}, "")
        ))

    def test_parallel_runner_uses_requested_workers(self) -> None:
        barrier = threading.Barrier(2)
        worker_threads: set[int] = set()
        lock = threading.Lock()
        cases = [
            runner.Case("syscalls", f"case{index}", "true", "pass", 1, "")
            for index in range(2)
        ]

        def worker(emulator, rootfs, memory, case):
            del emulator, rootfs, memory
            self.assertEqual(case.kernel_arguments, ("ncpus=1",))
            with lock:
                worker_threads.add(threading.get_ident())
            barrier.wait(timeout=WAIT_SECONDS)
            return runner.Result(case, 0, {"passed": 1}, "")

        results = list(runner.run_cases(
            pathlib.Path("emulator"),
            pathlib.Path("rootfs"),
            "1M",
            cases,
            2,
            worker,
        ))
        self.assertEqual({index for index, _ in results}, {1, 2})
        self.assertEqual(len(worker_threads), 2)

    def test_host_pressure_runner_preserves_manifest_cpu_topology(self) -> None:
        observed: list[tuple[str, ...]] = []
        case = runner.Case(
            "syscalls", "explicit-smp", "true", "pass", 1, "",
            host_pressure=True,
            kernel_arguments=("ncpus=4",),
        )

        def worker(emulator, rootfs, memory, selected):
            del emulator, rootfs, memory
            observed.append(selected.kernel_arguments)
            return runner.Result(selected, 0, {"passed": 1}, "")

        list(runner.run_cases(
            pathlib.Path("emulator"), pathlib.Path("rootfs"), "1M",
            [case], 2, worker,
        ))
        self.assertEqual(observed, [("ncpus=4",)])

    def test_single_worker_limits_only_ordinary_guest_topology(self) -> None:
        observed: list[tuple[str, tuple[str, ...]]] = []
        ordinary = runner.Case(
            "syscalls", "ordinary", "true", "pass", 1, "",
            kernel_arguments=("console=tty0", "ncpus=8"),
        )
        pressure = runner.Case(
            "sched", "smp", "true", "pass", 1, "",
            host_pressure=True,
        )

        def worker(emulator, rootfs, memory, selected):
            del emulator, rootfs, memory
            observed.append((selected.test_id, selected.kernel_arguments))
            return runner.Result(selected, 0, {"passed": 1}, "")

        list(runner.run_cases(
            pathlib.Path("emulator"), pathlib.Path("rootfs"), "1M",
            [pressure, ordinary], 1, worker,
        ))
        self.assertEqual(observed, [
            ("ordinary", ("console=tty0", "ncpus=1")),
            ("smp", ()),
        ])

    def test_cases_from_returns_manifest_suffix(self) -> None:
        cases = [
            runner.Case("syscalls", name, "true", "pass", 1, "")
            for name in ("first", "second", "third")
        ]
        self.assertEqual(
            [case.key for case in runner.cases_from(cases, "syscalls/second")],
            ["syscalls/second", "syscalls/third"],
        )

    def test_cases_from_rejects_unknown_key(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown start case"):
            runner.cases_from([], "syscalls/missing")

    def test_host_pressure_cases_are_excluded_by_default(self) -> None:
        ordinary = runner.Case(
            "syscalls", "ordinary", "true", "pass", 1, "",
        )
        pressure = runner.Case(
            "stress", "pressure", "true", "pass", 1, "",
            host_pressure=True,
        )
        selected, skipped = runner.select_host_pressure_cases(
            [ordinary, pressure], False
        )
        self.assertEqual(selected, [ordinary])
        self.assertEqual(skipped, 1)

    def test_host_pressure_cases_require_explicit_cli_option(self) -> None:
        required = [
            "--emulator", "uml-darwin",
            "--rootfs", "rootfs.erofs",
            "--manifest", "verified-tests.tsv",
            "--modules-builtin", "kernel-build/modules.builtin",
        ]
        defaults = runner.parse_arguments(required)
        explicit = runner.parse_arguments(required + ["--run-host-pressure"])
        self.assertFalse(defaults.run_host_pressure)
        self.assertTrue(explicit.run_host_pressure)

    def test_campaign_requires_the_kernel_builds_modules_builtin(self) -> None:
        arguments = [
            "--emulator", "uml-darwin",
            "--rootfs", "rootfs.erofs",
            "--manifest", "verified-tests.tsv",
        ]
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                runner.parse_arguments(arguments)

    def test_explicit_host_pressure_selection_preserves_all_cases(self) -> None:
        ordinary = runner.Case(
            "syscalls", "ordinary", "true", "pass", 1, "",
        )
        pressure = runner.Case(
            "stress", "pressure", "true", "pass", 1, "",
            host_pressure=True,
        )
        cases = [ordinary, pressure]
        selected, skipped = runner.select_host_pressure_cases(cases, True)
        self.assertIs(selected, cases)
        self.assertEqual(skipped, 0)

    def test_case_memory_override_is_forwarded_to_worker(self) -> None:
        observed: list[str] = []
        case = runner.Case(
            "syscalls", "large", "true", "pass", 1, "large scratch", "4G"
        )

        def worker(emulator, rootfs, memory, selected):
            del emulator, rootfs
            observed.append(memory)
            return runner.Result(selected, 0, {"passed": 1}, "")

        list(runner.run_cases(
            pathlib.Path("emulator"), pathlib.Path("rootfs"), "1G",
            [case], 1, worker,
        ))
        self.assertEqual(observed, ["4G"])

    def test_exclusive_cases_run_in_order_after_parallel_cases(self) -> None:
        barrier = threading.Barrier(2)
        lock = threading.Lock()
        regular_completed = 0
        serial_completed = 0
        exclusive_active = 0
        maximum_exclusive_active = 0
        completion_order: list[str] = []
        cases = [
            runner.Case("syscalls", f"regular{index}", "true", "pass", 1, "")
            for index in range(2)
        ] + [runner.Case(
            "syscalls", "serial0", "true", "pass", 1, "", serial=True,
        )] + [
            runner.Case(
                "stress", f"pressure{index}", "true", "pass", 1, "",
                host_pressure=True,
            )
            for index in range(2)
        ]

        def worker(emulator, rootfs, memory, case):
            nonlocal regular_completed, serial_completed, exclusive_active
            nonlocal maximum_exclusive_active
            del emulator, rootfs, memory
            if not case.host_pressure and not case.serial:
                barrier.wait(timeout=WAIT_SECONDS)
                with lock:
                    regular_completed += 1
                    completion_order.append(case.test_id)
            else:
                self.assertEqual(case.kernel_arguments, ())
                with lock:
                    self.assertEqual(regular_completed, 2)
                    if case.host_pressure:
                        self.assertEqual(serial_completed, 1)
                    exclusive_active += 1
                    maximum_exclusive_active = max(
                        maximum_exclusive_active, exclusive_active
                    )
                    completion_order.append(case.test_id)
                    if case.serial:
                        serial_completed += 1
                    exclusive_active -= 1
            return runner.Result(case, 0, {"passed": 1}, "")

        results = list(runner.run_cases(
            pathlib.Path("emulator"), pathlib.Path("rootfs"), "1M",
            cases, 2, worker,
        ))
        self.assertEqual({index for index, _ in results}, {1, 2, 3, 4, 5})
        self.assertEqual(
            completion_order[-3:], ["serial0", "pressure0", "pressure1"]
        )
        self.assertEqual(maximum_exclusive_active, 1)

    def test_parallel_interrupt_terminates_active_guests(self) -> None:
        case = runner.Case(
            "syscalls", "interrupt", "true", "pass", 1, ""
        )

        def worker(emulator, rootfs, memory, selected):
            del emulator, rootfs, memory, selected
            raise KeyboardInterrupt

        with mock.patch.object(runner, "terminate_active_guests") as terminate:
            with self.assertRaises(KeyboardInterrupt):
                list(runner.run_cases(
                    pathlib.Path("emulator"), pathlib.Path("rootfs"), "1M",
                    [case], 2, worker,
                ))
        terminate.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
