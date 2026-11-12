#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "integration/gpu"))
import run_vulkan_probe as probe
import receipt_fixtures


class VulkanProbeTests(unittest.TestCase):
    @staticmethod
    def elf() -> bytes:
        payload = bytearray(64)
        payload[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<HH", payload, 16, 3, 183)
        return bytes(payload)

    def test_elf_identity_rejects_wrong_architecture_class_endian_and_type(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "probe"
            path.write_bytes(self.elf())
            self.assertEqual(probe.load_probe(path), self.elf())
            for offset, value in ((0, 0), (4, 1), (5, 2), (16, 1), (18, 62)):
                with self.subTest(offset=offset):
                    payload = bytearray(self.elf())
                    payload[offset] = value
                    path.write_bytes(payload)
                    with self.assertRaises(probe.guest.SafetyError):
                        probe.load_probe(path)
            path.write_bytes(b"short")
            with self.assertRaises(probe.guest.SafetyError):
                probe.load_probe(path)
            with path.open("wb") as output:
                output.truncate(probe.MAX_PROBE_BYTES + 1)
            with self.assertRaises(probe.guest.SafetyError):
                probe.load_probe(path)

    def test_overlay_payload_is_checked_and_driver_is_receipt_bound(self):
        receipt = receipt_fixtures.vulkan_receipt(self)
        payload = self.elf() + b"Vulkan probe data" * 64
        script = probe.build_probe_script(payload, receipt, ["a b", "$(false)"])
        # The probe travels as a guest file; the script only checks it.
        self.assertNotIn("base64", script)
        self.assertIn(
            f"{probe.hashlib.sha256(payload).hexdigest()}  {probe.PROBE_GUEST_PATH}",
            script,
        )
        self.assertIn('test "$(nproc)" = 4', script)
        self.assertIn('test "$(getconf PAGESIZE)" = 4096', script)
        self.assertIn("/^MemTotal:/", script)
        self.assertIn("$2 < 4000000 || $2 > 4194304", script)
        self.assertIn("export VK_DRIVER_FILES=" + probe.ICD_PATH, script)
        self.assertIn("export UML_VULKAN_REQUIRE_VENUS=1", script)
        self.assertIn("unset VK_ICD_FILENAMES VK_ADD_DRIVER_FILES VK_INSTANCE_LAYERS", script)
        self.assertIn(f"{probe.PROBE_GUEST_PATH} 'a b' '$(false)'", script)
        for path in (probe.ICD_PATH, probe.LIBRARY_PATH):
            self.assertIn(probe.cts._receipt_file(receipt, path)["sha256"], script)
        subprocess.run(["/bin/sh", "-n"], input=script, text=True, check=True)

    def test_result_requires_clean_shutdown_and_verified_device(self):
        transcript = (probe.DEVICE_MARKER + "\r\n" + probe.RESULT_MARKER + "\r\n" +
                      probe.cts.common.DARWIN_CLEAN_STOP + "\r\n")
        probe.validate_result(0, True, False, transcript)
        for code, passed, failed, text in (
            (1, True, False, transcript), (0, False, False, transcript),
            (0, True, True, transcript), (0, True, False, probe.RESULT_MARKER),
            (0, True, False, probe.DEVICE_MARKER),
            (0, True, False, "echo '" + probe.DEVICE_MARKER + "'\n" + probe.RESULT_MARKER),
            (0, True, False, transcript.replace(probe.cts.common.DARWIN_CLEAN_STOP, "")),
            (0, True, False, transcript + probe.cts.common.DIRTY_DRM_SHUTDOWN),
        ):
            with self.subTest(code=code, passed=passed, failed=failed, text=text):
                with self.assertRaises(RuntimeError):
                    probe.validate_result(code, passed, failed, text)

    def test_argument_validation_does_not_boot(self):
        for timeout in ("0", "-1", "nan", "7201"):
            with self.subTest(timeout=timeout):
                with self.assertRaises(probe.guest.SafetyError):
                    probe.main(["--emulator", "/missing", "--rootfs", "/missing",
                                "--probe", "/missing", "--timeout", timeout])


if __name__ == "__main__":
    unittest.main()
