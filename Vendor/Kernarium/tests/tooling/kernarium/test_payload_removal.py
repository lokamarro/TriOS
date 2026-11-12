#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A device run leaves no boot payload in the application's container.

``devicectl`` has no command that removes a file from an application
container, so the automation runtime deletes the payload it was launched with
when the launch sets ``KERNARIUM_PAYLOAD_REMOVE=1``
(Kernarium/README.md, "Automation runs"). A fake device models the container
and an application that honours the setting; the runners are driven against it
through the ``devicectl`` boundary.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import io
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "gpu"))
sys.path.insert(0, str(REPOSITORY / "tests" / "perf"))
import ios_device  # noqa: E402
import kernarium  # noqa: E402
import run_ios_device_discard_bench as discard_bench  # noqa: E402
import run_ios_device_vk_cts as device_cts  # noqa: E402
import run_ios_device_workload as workload  # noqa: E402
import run_ios_mac_vk_gl_cts as mac_cts  # noqa: E402

KNOB = "DEVICECTL_CHILD_KERNARIUM_PAYLOAD_REMOVE"
PAYLOAD_SETTING = "DEVICECTL_CHILD_KERNARIUM_BOOT_PAYLOAD"
IMAGE = "rootfs.erofs"
# What earlier runs left in the container, and what is not a payload.
LEFTOVERS = {
    "kernarium-payload-" + "a" * 24 + ".cpio": b"old one",
    "kernarium-payload-" + "b" * 24 + ".cpio": b"old two",
    "kde-20260928-r1.erofs": b"another image",
}
CONSOLE_END = "UML_SESSION_EXIT:{status}\nKernarium: runtime returned 0\n"


def fake_payload_write(
    self: kernarium.Payload, output: pathlib.Path, tool: pathlib.Path = ...
) -> pathlib.Path:
    """Stand in for uml-initramfs: the files' bytes behind a newc magic."""
    output.write_bytes(kernarium.NEWC_MAGIC + b"".join(
        archive_path.encode() + b"\n" + file.contents + b"\n"
        for archive_path, file in sorted(self.files.items())
    ))
    return output


class FakeDevice:
    """A connected device's container and the application launched in it.

    Only the ``devicectl`` subcommands the runners use exist: listing and
    copying files, installing, and launching. Each command is recorded.
    """

    def __init__(self, *, honours_removal: bool = True, status: int = 0,
                 hangs: bool = False) -> None:
        self.documents: dict[str, bytes] = {
            IMAGE: b"the image", **LEFTOVERS,
        }
        self.honours_removal = honours_removal
        self.status = status
        self.hangs = hangs
        self.commands: list[list[str]] = []
        self.launches: list[dict[str, str]] = []

    def run(self, command: list[str], **_keywords: object
            ) -> subprocess.CompletedProcess[bytes]:
        self.commands.append(list(command))
        if command[:2] == ["xcrun", "devicectl"]:
            arguments = command[2:]
            if arguments[:3] == ["device", "info", "files"]:
                report = pathlib.Path(
                    arguments[arguments.index("--json-output") + 1]
                )
                report.write_text(json.dumps({"result": {"files": [
                    {"relativePath": name,
                     "metadata": {"size": len(contents)},
                     "resources": {"isDirectory": False}}
                    for name, contents in self.documents.items()
                ]}}))
            elif arguments[:3] == ["device", "copy", "to"]:
                source = pathlib.Path(
                    arguments[arguments.index("--source") + 1]
                )
                destination = arguments[arguments.index("--destination") + 1]
                assert destination.startswith("Documents/"), destination
                self.documents[destination[len("Documents/"):]] = (
                    source.read_bytes()
                )
            elif arguments[:3] != ["device", "install", "app"]:
                raise AssertionError(f"unexpected devicectl: {arguments}")
        return subprocess.CompletedProcess(command, 0)

    def guest_output(self, payload: bytes) -> str:
        return CONSOLE_END.format(status=self.status)

    def popen(self, command: list[str], *, stdout, env, **_keywords: object
              ) -> "FakeDevice.Console":
        self.commands.append(list(command))
        assert command[:5] == [
            "xcrun", "devicectl", "device", "process", "launch"
        ], command
        self.launches.append(dict(env))
        name = env.get(PAYLOAD_SETTING)
        text = ""
        if name is not None:
            data = self.documents[name]
            text += (
                f"{kernarium.AUTOMATION_BOOT_LINE}{len(data)}-byte payload "
                "and target graphical.target\n"
            )
            if self.honours_removal and env.get(KNOB) == "1":
                del self.documents[name]
                text += kernarium.PAYLOAD_REMOVED_LINE + name + "\n"
            if not self.hangs:
                text += self.guest_output(data)
        stdout.write(text.encode())
        stdout.flush()
        return self.Console(self.hangs)

    class Console:
        def __init__(self, hangs: bool) -> None:
            self.returncode = None if hangs else 0

        def poll(self) -> int | None:
            return self.returncode

        def send_signal(self, _number: int) -> None:
            self.returncode = 0

        def terminate(self) -> None:
            self.returncode = 0

        def wait(self, timeout: float | None = None) -> int:
            self.returncode = 0
            return 0

    def payloads(self) -> set[str]:
        return {name for name in self.documents
                if name.startswith("kernarium-payload-")}


class DiscardDevice(FakeDevice):
    """Answers each discard mode with the records the runner checks."""

    def guest_output(self, payload: bytes) -> str:
        mode = re.search(rb"UML_DISCARD_MODE=([a-z0-9-]+)", payload)
        assert mode is not None
        name = mode.group(1).decode()
        requests = discard_bench.EXPECTED_REQUESTS[name]
        nanoseconds = 100_000_000 if name == "batched-128m" else 900_000_000
        samples = "".join(
            f"UML_DISCARD_SAMPLE:{name}:{run}:{nanoseconds}:{requests}:"
            f"1048576:7\n" for run in range(1, 8)
        )
        return (f"{samples}UML_DISCARD_READY:{name}:7\n"
                + CONSOLE_END.format(status=0))


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


def automation_app(directory: pathlib.Path) -> pathlib.Path:
    app = directory / "Kernarium.app"
    for framework in kernarium.LINUX_RUNTIME_FRAMEWORKS:
        bundle = app / "Frameworks" / f"{framework}.framework"
        bundle.mkdir(parents=True)
        (bundle / framework).write_bytes(b"x\0KERNARIUM_BOOT_PAYLOAD\0y")
    return app


@contextlib.contextmanager
def attached(device: FakeDevice, clock: FakeClock | None = None):
    """Route every subprocess the runners start to the fake device."""
    with contextlib.ExitStack() as stack:
        stack.enter_context(
            mock.patch.object(kernarium.subprocess, "run", device.run))
        stack.enter_context(
            mock.patch.object(kernarium.subprocess, "Popen", device.popen))
        stack.enter_context(
            mock.patch.object(kernarium.Payload, "write", fake_payload_write))
        if clock is not None:
            stack.enter_context(
                mock.patch.object(ios_device.time, "monotonic", clock.monotonic))
            stack.enter_context(
                mock.patch.object(ios_device.time, "sleep", clock.sleep))
        yield


class PayloadRemovalTests(unittest.TestCase):
    def run_workload(self, device: FakeDevice, *, clock: FakeClock | None = None,
                     timeout: int = 3600) -> tuple[int | None, str]:
        """Run the workload runner; returns its status (None if it raised)
        and its standard error."""
        errors = io.StringIO()
        with tempfile.TemporaryDirectory() as name:
            root = pathlib.Path(name)
            script = root / "probe.sh"
            script.write_text("#!/bin/sh\necho hello\n")
            argv = ["--device", "fake", "--script", str(script),
                    "--artifact-root", str(root / "artifacts"),
                    "--timeout", str(timeout),
                    "--stall-timeout", str(timeout + 1)]
            status = None
            with attached(device, clock), contextlib.redirect_stdout(
                io.StringIO()
            ), contextlib.redirect_stderr(errors):
                try:
                    status = workload.main(argv)
                except ios_device.DeviceError:
                    pass
        return status, errors.getvalue()

    def assert_container_untouched(self, device: FakeDevice) -> None:
        """Only this run's payload came and went; nothing else was touched."""
        self.assertEqual(
            device.documents, {IMAGE: b"the image", **LEFTOVERS}
        )
        for command in device.commands:
            self.assertNotIn("--remove-existing-content", command)
            if command[0] == "codesign":
                continue
            self.assertEqual(command[:2], ["xcrun", "devicectl"])
            self.assertIn(
                command[2:5],
                (["device", "info", "files"], ["device", "copy", "to"],
                 ["device", "install", "app"],
                 ["device", "process", "launch"]),
            )

    def test_the_workload_runner_asks_for_removal_and_leaves_nothing(self
                                                                      ) -> None:
        device = FakeDevice()
        status, errors = self.run_workload(device)
        self.assertEqual(status, 0)
        self.assertEqual(errors, "")
        (launch,) = device.launches
        self.assertEqual(launch[KNOB], "1")
        self.assert_container_untouched(device)
        copies = [c for c in device.commands if c[2:5] == [
            "device", "copy", "to"]]
        self.assertEqual(len(copies), 1)
        self.assertEqual(
            copies[0][copies[0].index("--destination") + 1],
            "Documents/" + launch[PAYLOAD_SETTING],
        )

    def test_a_failed_workload_leaves_nothing(self) -> None:
        device = FakeDevice(status=3)
        status, _ = self.run_workload(device)
        self.assertEqual(status, 1)
        self.assertEqual(device.launches[0][KNOB], "1")
        self.assert_container_untouched(device)

    def test_a_workload_that_times_out_leaves_nothing(self) -> None:
        device = FakeDevice(hangs=True)
        status, _ = self.run_workload(device, clock=FakeClock(), timeout=30)
        self.assertIsNone(status)
        self.assertEqual(device.launches[0][KNOB], "1")
        self.assert_container_untouched(device)

    def test_a_runtime_that_ignores_the_setting_is_reported(self) -> None:
        device = FakeDevice(honours_removal=False)
        status, errors = self.run_workload(device)
        self.assertEqual(status, 0)
        (kept,) = device.payloads() - set(LEFTOVERS)
        self.assertIn(f"did not remove Documents/{kept}", errors)

    def test_the_discard_benchmark_removes_each_modes_payload(self) -> None:
        device = DiscardDevice()
        errors = io.StringIO()
        with tempfile.TemporaryDirectory() as name:
            root = pathlib.Path(name)
            app = automation_app(root)
            argv = ["run_ios_device_discard_bench.py", "--device", "fake",
                    "--app", str(app), "--artifact-root", str(root / "out")]
            with attached(device), mock.patch.object(
                sys, "argv", argv
            ), contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(errors):
                self.assertEqual(discard_bench.main(), 0)
        self.assertEqual(errors.getvalue(), "")
        self.assertEqual(len(device.launches), 2)
        names = {launch[PAYLOAD_SETTING] for launch in device.launches}
        self.assertEqual(len(names), 2)
        self.assertTrue(all(l[KNOB] == "1" for l in device.launches))
        self.assert_container_untouched(device)

    def test_the_device_cts_runner_asks_for_removal(self) -> None:
        options = argparse.Namespace(
            rootfs_filename="rootfs-vulkan-venus.ext4",
            payload="kernarium-payload-" + "c" * 24 + ".cpio",
            systemd_unit="uml-cts.target", cpus=4, memory_mib=4096,
            engine="gadgets",
        )
        environment = device_cts.launch_environment(options)
        self.assertEqual(environment[KNOB], "1")
        self.assertEqual(environment[PAYLOAD_SETTING], options.payload)

    def test_only_a_single_use_launch_may_ask(self) -> None:
        """The setting is opt-in: a launch that reuses one copied payload, and
        the Mac lane, whose host unlinks the payload itself, do not set it."""
        plain = kernarium.settings_for(
            rootfs=IMAGE, payload="p.cpio", cpus=1, memory_mib=512,
            engine="gadgets",
        )
        self.assertNotIn("KERNARIUM_PAYLOAD_REMOVE", plain)
        asking = kernarium.settings_for(
            rootfs=IMAGE, payload="p.cpio", cpus=1, memory_mib=512,
            engine="gadgets", remove_payload=True,
        )
        self.assertEqual(asking["KERNARIUM_PAYLOAD_REMOVE"], "1")
        with self.assertRaises(kernarium.KernariumError):
            kernarium.settings_for(
                rootfs=IMAGE, payload=None, cpus=1, memory_mib=512,
                engine="gadgets", remove_payload=True,
            )
        mac = argparse.Namespace(
            campaign="vulkan-info-development-v1", shard=None,
            systemd_unit="uml-cts.target", timeout=60, cpus=1,
            memory_mib=512, engine="gadgets", host_env=[],
        )
        self.assertNotIn(
            "TEST_RUNNER_KERNARIUM_PAYLOAD_REMOVE",
            mac_cts.session_environment(mac, "p.cpio"),
        )

    def test_exactly_the_single_launch_runners_opt_in(self) -> None:
        """A new caller of settings_for must decide, in this list."""
        asking = sorted(
            str(path.relative_to(REPOSITORY))
            for root in ("tests", "Kernarium/scripts", "tools")
            for path in (REPOSITORY / root).rglob("*.py")
            if "remove_payload=True" in path.read_text(encoding="utf-8")
            and path != pathlib.Path(__file__).resolve()
            and path != pathlib.Path(kernarium.__file__).resolve()
        )
        self.assertEqual(asking, [
            "tests/integration/gpu/run_ios_device_vk_cts.py",
            "tests/perf/run_ios_device_discard_bench.py",
            "tests/perf/run_ios_device_workload.py",
        ])


if __name__ == "__main__":
    unittest.main()
