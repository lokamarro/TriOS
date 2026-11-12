#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run an offline arm64 Vulkan regression in a 4-vCPU, 4-GiB Venus guest.

This supplements, but does not replace, targeted CTS qualification. The probe
must honor UML_VULKAN_REQUIRE_VENUS=1 and print UML_VULKAN_PROBE_DEVICE: Venus
only after verifying the physical device's VK_DRIVER_ID_MESA_VENUS identity.
"""

from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import struct
import sys
import time
from typing import Sequence

import run_gpu_guest as guest
import run_vk_cts as cts
import vulkan_cts_receipt as receipt_contract


MAX_PROBE_BYTES = 8 * 1024 * 1024
DEVICE_MARKER = "UML_VULKAN_PROBE_DEVICE: Venus"
RESULT_MARKER = "UML_VULKAN_PROBE: PASS"
ICD_PATH = "/usr/share/vulkan/icd.d/virtio_icd.json"
PROBE_GUEST_PATH = "/usr/local/libexec/uml-vulkan-probe"
LIBRARY_PATH = "/usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"


def load_probe(path: Path) -> bytes:
    size = path.stat().st_size
    if size < 64 or size > MAX_PROBE_BYTES:
        raise guest.SafetyError("probe must be a bounded arm64 ELF executable")
    payload = path.read_bytes()
    # ELF64, little-endian, current version, AArch64 ET_EXEC or ET_DYN (PIE).
    if (payload[:7] != b"\x7fELF\x02\x01\x01" or
            struct.unpack_from("<H", payload, 16)[0] not in (2, 3) or
            struct.unpack_from("<H", payload, 18)[0] != 183):
        raise guest.SafetyError("probe is not a little-endian arm64 ELF executable")
    return payload


def build_probe_script(payload: bytes, receipt: dict,
                       arguments: Sequence[str]) -> str:
    """The guest check around the probe, which is installed as a guest file."""
    digest = hashlib.sha256(payload).hexdigest()
    identities = [cts._receipt_file(receipt, path) for path in (ICD_PATH, LIBRARY_PATH)]
    checks = "\n".join(
        f"printf '%s\\n' {shlex.quote(item['sha256'] + '  ' + path)} | sha256sum -c -"
        for item, path in zip(identities, (ICD_PATH, LIBRARY_PATH))
    )
    invocation = shlex.join([PROBE_GUEST_PATH, *arguments])
    return f"""test "$(nproc)" = 4
test "$(getconf PAGESIZE)" = {guest.GUEST_PAGE_SIZE}
# UML consumes mem= before publishing /proc/cmdline. Check the usable amount
# as well as the host's fixed 4G boot argument, allowing kernel reservations.
awk '/^MemTotal:/ {{ print; found=1; if ($2 < 4000000 || $2 > 4194304) exit 1 }} END {{ if (!found) exit 1 }}' /proc/meminfo
{checks}
printf '%s\\n' '{digest}  {PROBE_GUEST_PATH}' | sha256sum -c -
timeout 60 sh -c 'until test -e /dev/dri/renderD128; do sleep 1; done'
unset VK_ICD_FILENAMES VK_ADD_DRIVER_FILES VK_INSTANCE_LAYERS VK_LOADER_DRIVERS_SELECT VK_LOADER_DRIVERS_DISABLE
export VK_DRIVER_FILES={ICD_PATH}
export UML_VULKAN_REQUIRE_VENUS=1
{invocation}
printf '%s\\n' '{RESULT_MARKER}'
"""


def validate_result(return_code: int, saw_pass: bool, saw_fail: bool,
                    transcript: str) -> None:
    lines = [line.strip() for line in transcript.splitlines()]
    if return_code != 0 or not saw_pass or saw_fail:
        raise RuntimeError("Vulkan probe guest failed or did not shut down cleanly")
    if DEVICE_MARKER not in lines or RESULT_MARKER not in lines:
        raise RuntimeError("missing verified Venus device or probe completion marker")
    cts.common.validate_guest_shutdown(transcript)


def main(arguments: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--rootfs", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--artifact-root", type=Path,
                        default=guest.REPO_ROOT / ".build/vulkan-probes")
    parser.add_argument("probe_arguments", nargs=argparse.REMAINDER)
    options = parser.parse_args(arguments)
    if not 0 < options.timeout <= 7200:
        raise guest.SafetyError("timeout must be positive and no more than two hours")
    emulator = options.emulator.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        raise guest.SafetyError("emulator must be executable")
    rootfs = options.rootfs.resolve()
    probe = options.probe.resolve()
    payload = load_probe(probe)
    receipt = receipt_contract.validate_receipt()
    guest.validate_rootfs_path(rootfs, "ext4")
    receipt_contract.verify_image(rootfs, receipt)
    provider_hash = cts.verify_staged_kosmickrisp(emulator)
    probe_arguments = options.probe_arguments
    if probe_arguments[:1] == ["--"]:
        probe_arguments = probe_arguments[1:]
    script = guest.make_test_script(
        [build_probe_script(payload, receipt, probe_arguments)],
        auto_poweroff=True, lower_filesystem_type="ext4")
    directory = options.artifact_root.resolve() / (
        time.strftime("%Y%m%d-%H%M%S-") + str(os.getpid()))
    boot_files = guest.runner_guest_files(script, "/dev/ubda", "ext4")
    probe_file = directory / "guest-files" / Path(PROBE_GUEST_PATH).name
    command = guest.build_command(
        emulator, rootfs, "4G", "multi-user.target",
        ["ncpus=4", "--execution-engine=jit", "--gpu", "--gpu-display=vnc",
         "--gpu-vnc-bind=127.0.0.1", "--gpu-vnc-port=0",
         "systemd.mask=NetworkManager.service",
         "systemd.mask=systemd-networkd-wait-online.service"],
        guest.guest_file_arguments(directory / "boot-files", boot_files,
                                   [(PROBE_GUEST_PATH, probe_file)]),
        rootfs_type="ext4")
    directory.mkdir(parents=True, exist_ok=False)
    probe_file.parent.mkdir()
    probe_file.write_bytes(payload)
    probe_file.chmod(0o700)
    guest.write_guest_files(directory / "boot-files", boot_files)
    report = {
        "schema_version": 1, "formal_conformance": False,
        "scope": "supplemental Vulkan behavioral regression, not CTS",
        "command": command, "cpus": 4, "memory": "4G",
        "page_size": guest.GUEST_PAGE_SIZE, "probe": str(probe),
        "probe_arguments": probe_arguments,
        "probe_sha256": hashlib.sha256(payload).hexdigest(),
        "emulator_sha256": receipt_contract.sha256_file(emulator),
        "kosmickrisp_sha256": provider_hash,
        "receipt_sha256": receipt_contract.sha256_file(receipt_contract.RECEIPT_PATH),
        "rootfs_sha256": receipt["derivative"]["sha256"],
        "git": cts.common.git_state(), "passed": False,
    }
    guest.write_json(directory / "run.json", report)
    try:
        code, passed, failed = guest.run_guest(
            command, directory / "console.log", options.timeout,
            expect_poweroff=True)
        report.update(emulator_return_code=code, saw_pass_marker=passed,
                      saw_fail_marker=failed)
        validate_result(code, passed, failed,
                        (directory / "console.log").read_text(errors="replace"))
        report["passed"] = True
    finally:
        if (directory / "console.log").is_file():
            report["console_sha256"] = receipt_contract.sha256_file(directory / "console.log")
        guest.write_json(directory / "run.json", report)
        print(f"Vulkan probe artifacts: {directory}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("Vulkan probe interrupted; guest stopped", file=sys.stderr)
        raise SystemExit(130) from None
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print(f"Vulkan probe: {error}", file=sys.stderr)
        raise SystemExit(1) from error
