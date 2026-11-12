#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure batched and fragmented UBD discard on a physical iPad.

Each mode is one boot of guest/ubd-discard.sh as a device workload
(run_ios_device_workload.py), under a tmpfs writable layer so the script owns
the overlay disk.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
from pathlib import Path
import re
import statistics
import sys
import time


REPOSITORY = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
import guest  # noqa: E402
import ios_device  # noqa: E402
import kernarium  # noqa: E402

WORKLOAD = Path(__file__).resolve().parent / "guest" / "ubd-discard.sh"
PARAMETERS = "/etc/uml-discard.env"
DEFAULT_APP = (
    REPOSITORY
    / ".build/kernarium-device-derived/Build/Products/Release-iphoneos"
    / "Kernarium.app"
)
DEFAULT_ARTIFACT_ROOT = REPOSITORY / ".build/ios-device-discard-bench"
MODES = ("batched-128m", "fragmented-32k")
EXPECTED_REQUESTS = {"batched-128m": 4, "fragmented-32k": 16384}
SAMPLE = re.compile(
    r"UML_DISCARD_SAMPLE:"
    r"(batched-128m|fragmented-32k):"
    r"([1-7]):([0-9]+):([0-9]+):([0-9]+):([0-9]+)"
)
READY = re.compile(
    r"UML_DISCARD_READY:"
    r"(batched-128m|fragmented-32k):([0-9]+)"
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--app", type=Path, default=DEFAULT_APP)
    parser.add_argument("--rootfs-filename", default="rootfs.erofs")
    parser.add_argument("--cpus", type=int, default=2)
    parser.add_argument("--memory-mib", type=int, default=1024)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--initramfs-tool", type=Path,
                        default=kernarium.DEFAULT_INITRAMFS_TOOL)
    parser.add_argument("--maximum-batched-ms", type=float, default=500.0)
    parser.add_argument("--minimum-speedup", type=float, default=2.0)
    return parser.parse_args()


def launch(
    arguments: argparse.Namespace, mode: str, directory: Path
) -> list[dict[str, int | str]]:
    payload = kernarium.workload_payload(
        kernarium.root_type(arguments.rootfs_filename),
        WORKLOAD.read_bytes(),
        files={
            PARAMETERS: guest.GuestFile(f"UML_DISCARD_MODE={mode}\n".encode())
        },
        upper="tmpfs",
    )
    written = payload.write(
        directory / f"{mode}.cpio", arguments.initramfs_tool
    )
    name = kernarium.payload_name(written)
    ios_device.copy_to_device(
        arguments.device, kernarium.APPLICATION, written, name, 300
    )
    log = directory / f"{mode}.log"
    transcript = ios_device.run_device_console(
        arguments.device,
        kernarium.APPLICATION,
        kernarium.settings_for(
            rootfs=arguments.rootfs_filename,
            payload=name,
            cpus=arguments.cpus,
            memory_mib=arguments.memory_mib,
            engine="gadgets",
            # Each mode copies its own payload and launches it once.
            remove_payload=True,
            extra={"KERNARIUM_OVERLAY_SIZE_GIB": "1"},
        ),
        log,
        timeout=arguments.timeout,
        stall_timeout=arguments.timeout,
        label=f"iPad discard benchmark {mode}",
    )
    kernarium.require_automation_boot(transcript)
    kernarium.report_payload_removal(transcript, name)
    if kernarium.runtime_result(transcript) != 0:
        raise RuntimeError(f"{mode} runtime did not return zero; see {log}")
    if kernarium.session_exit(transcript) != 0:
        raise RuntimeError(f"{mode} workload did not exit zero; see {log}")
    ready = READY.findall(transcript)
    if len(ready) != 1 or ready[0][0] != mode:
        raise RuntimeError(f"{mode} emitted an invalid ready record; see {log}")
    samples = [
        {
            "mode": match.group(1),
            "run": int(match.group(2)),
            "nanoseconds": int(match.group(3)),
            "requests": int(match.group(4)),
            "sectors": int(match.group(5)),
            "ticks_ms": int(match.group(6)),
        }
        for match in SAMPLE.finditer(transcript)
        if match.group(1) == mode
    ]
    if [sample["run"] for sample in samples] != list(range(1, 8)):
        raise RuntimeError(f"{mode} emitted an incomplete sample set; see {log}")
    for sample in samples:
        if (
            sample["requests"] != EXPECTED_REQUESTS[mode]
            or sample["sectors"] != 1048576
        ):
            raise RuntimeError(
                f"{mode} emitted unexpected block statistics: {sample}"
            )
    return samples


def main() -> int:
    arguments = parse_arguments()
    if arguments.cpus < 1 or arguments.memory_mib < 512:
        raise ValueError("guest CPU and memory settings are invalid")
    if arguments.timeout < 1:
        raise ValueError("--timeout must be positive")
    if arguments.maximum_batched_ms <= 0 or arguments.minimum_speedup <= 1:
        raise ValueError("performance thresholds are invalid")
    app = arguments.app.resolve()
    if not app.is_dir():
        raise ValueError(f"Kernarium app is unavailable: {app}")
    kernarium.require_automation_app(app)
    ios_device.install_app(arguments.device, app, 300)

    artifact = arguments.artifact_root.resolve() / time.strftime(
        "%Y%m%d-%H%M%S"
    )
    artifact.mkdir(parents=True, exist_ok=False)
    results: dict[str, list[dict[str, int | str]]] = {}
    for mode in MODES:
        results[mode] = launch(arguments, mode, artifact)

    medians_ms = {
        mode: statistics.median(
            sample["nanoseconds"] for sample in samples
        )
        / 1_000_000
        for mode, samples in results.items()
    }
    speedup = medians_ms["fragmented-32k"] / medians_ms["batched-128m"]
    summary = {
        "schema_version": 1,
        "device": arguments.device,
        "app": str(app),
        "guest": {
            "cpus": arguments.cpus,
            "memory_mib": arguments.memory_mib,
            "overlay_gib": 1,
            "page_size": 4096,
            "engine": "gadgets",
            "discard_bytes": 536870912,
        },
        "medians_ms": medians_ms,
        "speedup": speedup,
        "samples": results,
    }
    (artifact / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"batched-128m median: {medians_ms['batched-128m']:.3f} ms; "
        f"fragmented-32k median: {medians_ms['fragmented-32k']:.3f} ms; "
        f"speedup: {speedup:.2f}x",
        flush=True,
    )
    print(f"Artifacts: {artifact}", flush=True)
    if medians_ms["batched-128m"] > arguments.maximum_batched_ms:
        raise RuntimeError("batched discard exceeded the wall-time threshold")
    if speedup < arguments.minimum_speedup:
        raise RuntimeError("batching did not meet the minimum speedup")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
