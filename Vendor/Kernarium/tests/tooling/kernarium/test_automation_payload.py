#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The host side of a Kernarium automation run writes what the runtime boots."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import guest  # noqa: E402
import ios_device  # noqa: E402
import kernarium  # noqa: E402

TOOL: pathlib.Path | None = None


def read_newc(data: bytes) -> dict[str, tuple[int, bytes]]:
    """Each member's mode and contents, by archive path, up to the trailer."""
    members: dict[str, tuple[int, bytes]] = {}
    offset = 0
    while True:
        header = data[offset:offset + 110]
        if header[:6] != b"070701":
            raise AssertionError(f"no newc header at {offset}")
        fields = [int(header[6 + 8 * index:14 + 8 * index], 16)
                  for index in range(13)]
        mode, size, name_size = fields[1], fields[6], fields[11]
        name_start = offset + 110
        name = data[name_start:name_start + name_size - 1].decode()
        body = (name_start + name_size + 3) & ~3
        contents = data[body:body + size]
        offset = (body + size + 3) & ~3
        if name == "TRAILER!!!":
            if offset != len(data):
                raise AssertionError("bytes follow the trailer")
            return members
        members["/" + name.lstrip("/")] = (mode, contents)


class AutomationPayloadTests(unittest.TestCase):
    def test_boot_parameters_describe_the_runtime_disks(self) -> None:
        self.assertEqual(
            kernarium.boot_parameters("erofs"),
            "UML_BOOT_ROOT=/dev/ubda\nUML_BOOT_ROOT_TYPE=erofs\n"
            "UML_BOOT_UPPER=ext4\nUML_BOOT_UPPER_DEVICE=/dev/ubdb\n"
            "UML_BOOT_ENTRY=/sbin/init\n",
        )
        self.assertEqual(
            kernarium.boot_parameters("ext4", upper="tmpfs", entry="/x"),
            "UML_BOOT_ROOT=/dev/ubda\nUML_BOOT_ROOT_TYPE=ext4\n"
            "UML_BOOT_UPPER=tmpfs\nUML_BOOT_ENTRY=/x\n",
        )
        with self.assertRaises(kernarium.KernariumError):
            kernarium.boot_parameters("squashfs")
        self.assertEqual(kernarium.root_type("kde-20260928-r1.erofs"), "erofs")
        self.assertEqual(kernarium.root_type("rootfs.EXT4"), "ext4")
        with self.assertRaises(kernarium.KernariumError):
            kernarium.root_type("guest.kern")

    def test_payload_paths_are_clean_and_unique(self) -> None:
        payload = kernarium.Payload()
        payload.add("/etc/uml.env", "A=1\n")
        for path in ("/etc/uml.env", "/run/x", "etc/x", "/etc/../x"):
            with self.subTest(path=path), self.assertRaises(
                (kernarium.KernariumError, ValueError)
            ):
                payload.add(path, "")
        with self.assertRaises(kernarium.KernariumError):
            kernarium.Payload().write(pathlib.Path("/nonexistent/p.cpio"))
        with self.assertRaises(kernarium.KernariumError):
            kernarium.validate_systemd_target("uml-cts.service")
        self.assertEqual(
            kernarium.validate_systemd_target("uml-cts.target"),
            "uml-cts.target",
        )

    def test_workload_payload_is_a_segment_the_runtime_appends(self) -> None:
        if TOOL is None:
            self.skipTest("uml-initramfs was not given")
        payload = kernarium.workload_payload(
            "erofs",
            b"#!/bin/sh\necho hello\n",
            files={"/etc/uml-geekbench.env": guest.GuestFile(b"W=1\n")},
            upper="tmpfs",
        )
        with tempfile.TemporaryDirectory() as name:
            output = pathlib.Path(name) / "payload.cpio"
            payload.write(output, TOOL)
            data = output.read_bytes()
            self.assertEqual(len(data) % 4, 0)
            self.assertLessEqual(len(data), kernarium.PAYLOAD_LIMIT)
            members = read_newc(data)
            first = kernarium.payload_name(output)
            self.assertRegex(first, r"^kernarium-payload-[0-9a-f]{24}\.cpio$")
            self.assertEqual(first, kernarium.payload_name(output))
            with self.assertRaises(Exception):
                payload.write(output, TOOL)
        files = {path: (mode & 0o7777, contents)
                 for path, (mode, contents) in members.items()
                 if mode & 0o170000 == 0o100000}
        self.assertEqual(
            files["/boot.env"][1].decode(),
            kernarium.boot_parameters(
                "erofs", upper="tmpfs", entry=guest.SESSION_GUEST_PATH
            ),
        )
        self.assertEqual(
            files[f"/payload{guest.SESSION_GUEST_PATH}"],
            (0o755, guest.SESSION_SCRIPT.read_bytes()),
        )
        self.assertEqual(
            files[f"/payload{guest.SESSION_PARAMETERS_GUEST_PATH}"][1],
            f"UML_SESSION_ENTRY={kernarium.WORKLOAD_GUEST_PATH}\n"
            "UML_SESSION_RUNTIME_MOUNTS=1\n".encode(),
        )
        self.assertEqual(
            files[f"/payload{kernarium.WORKLOAD_GUEST_PATH}"],
            (0o755, b"#!/bin/sh\necho hello\n"),
        )
        self.assertEqual(
            files["/payload/etc/uml-geekbench.env"], (0o644, b"W=1\n")
        )

    def test_only_the_automation_build_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            app = pathlib.Path(name) / "Kernarium.app"
            for framework in kernarium.LINUX_RUNTIME_FRAMEWORKS:
                directory = app / "Frameworks" / f"{framework}.framework"
                directory.mkdir(parents=True)
                (directory / framework).write_bytes(b"release runtime")
            with self.assertRaisesRegex(kernarium.KernariumError, "release"):
                kernarium.require_automation_app(app)
            for framework in kernarium.LINUX_RUNTIME_FRAMEWORKS:
                binary = app / "Frameworks" / f"{framework}.framework" / framework
                binary.write_bytes(b"x\0KERNARIUM_BOOT_PAYLOAD\0y")
            kernarium.require_automation_app(app)
        kernarium.require_automation_boot(
            "Kernarium: automation boot with a 512-byte payload and target "
            "uml-cts.target\n"
        )
        with self.assertRaisesRegex(kernarium.KernariumError, "release"):
            kernarium.require_automation_boot("Linux version 6.18\n")

    def test_a_boot_with_a_host_fast_path_off_is_refused(self) -> None:
        with self.assertRaisesRegex(kernarium.KernariumError, "fast path off"):
            kernarium.require_automation_boot(
                "Kernarium: automation boot with a 512-byte payload and "
                "target uml-cts.target\n"
                "Darwin: CPU1's thread-specific data is not where expected; "
                "reading the CPU through pthread_getspecific()\n"
            )

    def test_device_settings_clear_inherited_kernarium_settings(self) -> None:
        settings = kernarium.settings_for(
            rootfs="kde-20260928-r1.erofs",
            payload="kernarium-payload-0.cpio",
            unit="uml-cts.target",
            cpus=4,
            memory_mib=1536,
            engine="gadgets",
        )
        self.assertEqual(settings["KERNARIUM_SYSTEMD_UNIT"], "uml-cts.target")
        with mock.patch.dict(
            kernarium.os.environ,
            {"DEVICECTL_CHILD_KERNARIUM_GEEKBENCH": "1"},
        ):
            environment = ios_device.device_launch_environment(
                kernarium.APPLICATION, settings
            )
        self.assertNotIn("DEVICECTL_CHILD_KERNARIUM_GEEKBENCH", environment)
        self.assertEqual(
            environment["DEVICECTL_CHILD_KERNARIUM_BOOT_PAYLOAD"],
            "kernarium-payload-0.cpio",
        )
        self.assertEqual(
            kernarium.session_exit("x\nUML_SESSION_EXIT:3\r\n"), 3
        )
        self.assertEqual(
            kernarium.runtime_result("Kernarium: runtime returned 0\n"), 0
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=pathlib.Path)
    options, remaining = parser.parse_known_args()
    TOOL = options.tool
    unittest.main(argv=[sys.argv[0], *remaining])
