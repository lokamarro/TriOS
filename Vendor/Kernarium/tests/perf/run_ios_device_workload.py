#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run one guest workload in Kernarium on a connected iOS device.

The workload is a guest script, such as ``guest/geekbench.sh``, that the
harness session runs over the image's overlay root, without systemd or a
desktop, before it powers the guest off. The script and the files it reads
reach the guest in a boot payload that this runner writes and copies into the
application's container, and only the automation build of the runtime boots
one. Nothing is typed, so every line the script prints is its own.
tests/perf/README.md owns the lane.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
from pathlib import Path
import re
import sys
import time

REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import guest  # noqa: E402
import ios_device  # noqa: E402
import kernarium  # noqa: E402

DEFAULT_ARTIFACT_ROOT = REPOSITORY / ".build" / "ios-device-workload"
GEEKBENCH = Path(__file__).resolve().parent / "guest" / "geekbench.sh"
GEEKBENCH_PARAMETERS = "/etc/uml-geekbench.env"
WORKLOAD_LIST = re.compile(r"[0-9]+(?:,[0-9]+)*")


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True,
                        help="devicectl device identifier, UDID, or name")
    parser.add_argument("--app", type=Path,
                        help="the automation .app bundle to install first")
    parser.add_argument("--rootfs-filename", default="rootfs.erofs",
                        help="the image in the application's Documents")
    workload = parser.add_mutually_exclusive_group(required=True)
    workload.add_argument("--geekbench", action="store_true",
                          help="run guest/geekbench.sh")
    workload.add_argument("--script", type=Path,
                          help="run this POSIX shell script")
    parser.add_argument(
        "--geekbench-workloads", metavar="LIST",
        help="only these comma-separated single-core Geekbench workloads",
    )
    parser.add_argument(
        "--file", action="append", default=[], metavar="GUEST=HOST",
        help="also install a host file at a guest path, keeping its mode",
    )
    parser.add_argument(
        "--upper", choices=("ext4", "tmpfs"), default="ext4",
        help="the writable layer; tmpfs leaves the overlay disk to the script",
    )
    parser.add_argument("--cpus", type=int, default=2)
    parser.add_argument("--memory-mib", type=int, default=1536)
    parser.add_argument("--engine", choices=("gadgets", "jit"),
                        default="gadgets")
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--stall-timeout", type=int, default=900)
    parser.add_argument("--install-timeout", type=int, default=600)
    parser.add_argument("--artifact-root", type=Path,
                        default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--initramfs-tool", type=Path,
                        default=kernarium.DEFAULT_INITRAMFS_TOOL)
    options = parser.parse_args(argv)
    if options.geekbench_workloads is not None and (
        not options.geekbench
        or WORKLOAD_LIST.fullmatch(options.geekbench_workloads) is None
    ):
        parser.error("--geekbench-workloads takes --geekbench and numbers")
    if options.cpus < 1 or options.memory_mib < 512:
        parser.error("guest CPU and memory settings are invalid")
    if min(options.timeout, options.stall_timeout,
           options.install_timeout) < 1:
        parser.error("timeouts must be positive")
    return options


def build_payload(options: argparse.Namespace) -> kernarium.Payload:
    files = kernarium.guest_files(options.file)
    if options.geekbench:
        script = GEEKBENCH.read_bytes()
        if options.geekbench_workloads is not None:
            if GEEKBENCH_PARAMETERS in files:
                raise kernarium.KernariumError(
                    f"{GEEKBENCH_PARAMETERS} is named twice"
                )
            files[GEEKBENCH_PARAMETERS] = guest.GuestFile(
                "UML_GEEKBENCH_WORKLOADS="
                f"{options.geekbench_workloads}\n".encode()
            )
    else:
        script = options.script.read_bytes()
    return kernarium.workload_payload(
        kernarium.root_type(options.rootfs_filename),
        script,
        files=files,
        upper=options.upper,
    )


def main(argv: list[str] | None = None) -> int:
    options = parse_arguments(argv)
    artifact = options.artifact_root.resolve() / time.strftime(
        "%Y%m%d-%H%M%S"
    )
    artifact.mkdir(parents=True, exist_ok=False)
    written = build_payload(options).write(
        artifact / "payload.cpio", options.initramfs_tool
    )
    name = kernarium.payload_name(written)
    if options.app is not None:
        app = options.app.resolve()
        kernarium.require_automation_app(app)
        ios_device.install_app(options.device, app, options.install_timeout)
    ios_device.copy_to_device(
        options.device, kernarium.APPLICATION, written, name,
        options.install_timeout,
    )
    settings = kernarium.settings_for(
        rootfs=options.rootfs_filename,
        payload=name,
        cpus=options.cpus,
        memory_mib=options.memory_mib,
        engine=options.engine,
        # The one launch below is the only use of the copy just made.
        remove_payload=True,
    )
    log = artifact / "console.log"
    started = time.monotonic()
    transcript = ios_device.run_device_console(
        options.device,
        kernarium.APPLICATION,
        settings,
        log,
        timeout=options.timeout,
        stall_timeout=options.stall_timeout,
        label="the device workload",
    )
    elapsed = time.monotonic() - started
    kernarium.require_automation_boot(transcript)
    kernarium.report_payload_removal(transcript, name)
    status = kernarium.session_exit(transcript)
    runtime = kernarium.runtime_result(transcript)
    summary = {
        "schema_version": 1,
        "device": options.device,
        "rootfs": options.rootfs_filename,
        "payload": name,
        "guest": {
            "cpus": options.cpus,
            "memory_mib": options.memory_mib,
            "engine": options.engine,
            "page_size": 4096,
            "upper": options.upper,
        },
        "workload_status": status,
        "runtime_result": runtime,
        "seconds": round(elapsed, 1),
    }
    (artifact / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"workload status {status}, runtime returned {runtime}, "
          f"{elapsed:.0f} s, cpus={options.cpus}; console: {log}", flush=True)
    return 0 if status == 0 and runtime == 0 else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (kernarium.KernariumError, ios_device.DeviceError) as error:
        print(f"run_ios_device_workload.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
