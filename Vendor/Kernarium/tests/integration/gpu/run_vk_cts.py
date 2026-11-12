#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the Vulkan CTS development and receipt-backed derivative lanes.

The current EROFS image remains intentionally limited to the 19 installed
``dEQP-VK.info.*`` cases.  Its CTS build has invalid CPU/release metadata and
therefore cannot produce qualification evidence.  The separately authorized
ext4 derivative has its own checked receipt, patched downstream guest Mesa,
native-arm64 CTS build, and immutable image identity.  Receipt-backed results
are project qualification evidence, never a Khronos conformance claim.

Artifact framing, QPA parsing, guest cleanup, and the authenticated read-only
lower bootstrap are shared with the maintained VK-GL-CTS driver.  Vulkan mode
selection and identity policy remain separate and fail closed.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import sys
import time
from typing import Any, Sequence


REPO_ROOT = Path(__file__).resolve().parents[3]
INTEGRATION_GPU = Path(__file__).resolve().parent
IDENTITY_PATH = INTEGRATION_GPU / "vulkan_cts_identity.json"
CAMPAIGNS_PATH = INTEGRATION_GPU / "vulkan_cts_development_campaigns.json"
FUTURE_RECEIPT_SCHEMA_PATH = (
    INTEGRATION_GPU / "vulkan_cts_future_receipt.schema.json"
)
RECEIPT_PATH = INTEGRATION_GPU / "vulkan_cts_receipt.json"
RECEIPT_MANDATORY_PATH = (
    INTEGRATION_GPU / "vulkan_cts_mandatory_1_4_6_2.txt"
)
DEFAULT_ROOTFS = REPO_ROOT / "rootfs.erofs"
DEFAULT_RECEIPT_ROOTFS = REPO_ROOT / ".build/rootfs/rootfs-vulkan-venus.ext4"
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/vulkan-cts"
DEFAULT_EMULATOR = REPO_ROOT / ".build/presets/macos-integration/bin/uml-darwin"
CASE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]*\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
FORBIDDEN_SOFTWARE_ICD = re.compile(
    r"(?:lavapipe|llvmpipe|\blvp\b|softpipe|swrast|software rasterizer)",
    re.IGNORECASE,
)
VIRTIO_IDENTITY = re.compile(r"(?:virtio|venus)", re.IGNORECASE)
CTS_SERVICE_PATH = "/usr/local/libexec/uml-vulkan-cts"
# A harness installs the selected cases here as a file, whatever their count.
GUEST_CASE_LIST_PATH = "/usr/local/share/uml-vulkan-cts/selected.txt"
CTS_WSI_SERVICE_NAME = "uml-vulkan-cts.service"
CTS_WSI_SERVICE_PATH = f"/etc/systemd/system/{CTS_WSI_SERVICE_NAME}"
CTS_WSI_SERVICE = """[Unit]
Description=Darwin UML Vulkan WSI CTS campaign
After=systemd-user-sessions.service
Wants=systemd-user-sessions.service

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/uml-vulkan-cts
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
"""
# Keep each acknowledged frame below the 4 KiB virtio-console queue buffer.
ARTIFACT_BASE64_LINE_LENGTH = 3 * 1024

sys.path.insert(0, str(INTEGRATION_GPU))
import run_vk_gl_cts as common  # noqa: E402
import run_gpu_guest as gpu_guest  # noqa: E402
import vulkan_cts_receipt as receipt_contract  # noqa: E402

CampaignError = common.CampaignError


# The Vulkan campaigns a Kernarium automation run can boot. The payload
# builder (kernarium_campaigns.py) and the lanes that name a campaign read them
# from here, so a campaign cannot be built under a name no runner accepts.
APPLE_DEVELOPMENT_CAMPAIGN = "vulkan-info-development-v1"
APPLE_RECEIPT_SMOKE_CAMPAIGN = "vulkan-info-receipt-smoke-v1"
# The released CTS permits at most sixteen native fractions, and a device runs
# one campaign per launch, so the finest sharding the receipt allows is also
# the one that gives a slow device the shortest resumable unit.
APPLE_MUSTPASS_FRACTIONS = 16
# A fraction of the released mustpass is far longer than any GL campaign, and
# the application cannot resume one, so the guest-side bound is a whole day and
# the progress guard is what actually ends a wedged run.
APPLE_MUSTPASS_TIMEOUT = 86400
APPLE_MUSTPASS_PROGRESS_TIMEOUT = 1800
# Guest memory the ordinary baked device campaigns are sized for. The
# application's ceiling is the platform jetsam budget, not a choice: raising
# it trades the guest reaper for the host one. Fraction 03 has a narrower
# profile below because its large render-to-image transition reaches the host
# budget before it reaches the guest budget.
APPLE_MUSTPASS_GUEST_MEMORY_MIB = 1536
APPLE_MUSTPASS_FRACTION03_GUEST_MEMORY_MIB = 1280
APPLE_MUSTPASS_FRACTION03_RESTART_BEFORE = (
    "dEQP-VK.pipeline.fast_linked_library.render_to_image.core.2d_array."
    "huge.width_height_layers.r8g8b8a8_unorm"
)
# The prefix process's host graphics memory is released after it exits, and
# nothing in the guest shows when. The suffix therefore starts after a fixed
# pause, never past the shard's deadline.
APPLE_MUSTPASS_FRACTION03_RESTART_PAUSE_SECONDS = 20
# dEQP ends its whole session at the first ResourceError, so one such case
# otherwise costs a fraction every case after it. The guest continues the
# remainder in a fresh process, at most this many processes in all.
VULKAN_CTS_SESSION_LIMIT = 8
CONTINUATION_CASELIST = "continue.txt"
EXCLUSION_REASON = re.compile(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*\Z")


@dataclass(frozen=True)
class DeviceExclusion:
    """Cases a baked device campaign does not run, and why.

    ``patterns`` are group prefixes, written with a trailing dot, or exact case
    names. ``reason`` is the cause, not the cases: it names what the device
    cannot do, so a later run on hardware without that limit can select the
    same cases back from the run's own ``skipped.txt``.
    """

    reason: str
    patterns: tuple[str, ...]

    def matches(self, case: str) -> bool:
        return any(
            case.startswith(pattern)
            if pattern.endswith(".")
            else case == pattern
            for pattern in self.patterns
        )

    @property
    def row(self) -> str:
        """The reason as it appears in a summary row name."""

        return self.reason.replace("-", "_")


@dataclass(frozen=True)
class AppleMustpassProfile:
    """The immutable launch contract for one baked device fraction."""

    index: int
    identifier: str
    guest_memory_mib: int
    restart_before: str | None = None
    restart_pause_seconds: int = 0


# What a baked device campaign leaves out, by cause. Nothing here is a verdict:
# an excluded case is neither a pass nor a failure, the guest transports exactly
# which cases it left out and under which reason, and only the baked device
# campaigns exclude anything at all -- every macOS lane runs the released
# selection unchanged.
#
# guest-memory: ``dEQP-VK.reconvergence`` simulates each subgroup's control flow
# on the CPU. Measured on macOS at the device's own guest profile, fraction 00's
# 380 reconvergence cases all pass in a fresh process and cost 665 MiB of peak
# resident size; that peak is additive with the 1.1 GiB the process has already
# grown to by the time a fraction reaches them, and 1536 MiB cannot hold both.
DEVICE_EXCLUSIONS: tuple[DeviceExclusion, ...] = (
    DeviceExclusion("guest-memory", ("dEQP-VK.reconvergence.",)),
)


def apple_mustpass_profile(index: int) -> AppleMustpassProfile:
    """Return the baked identity, memory, and process plan for a fraction.

    The version is part of the selection's identity: ``v2`` is the first
    generation that applies ``DEVICE_EXCLUSIONS``, and ``v3`` the first that
    continues a fraction whose CTS session ended before the traversal did.
    Fraction 03's ``v4`` profile is the first to use 1280 MiB and one planned
    CTS restart before its large render-to-image block. How long the guest
    pauses at that restart for host memory is pacing inside the same plan, not
    part of the identity: a ``v4`` result means the same thing whichever pause
    produced it.
    """

    if not 0 <= index < APPLE_MUSTPASS_FRACTIONS:
        raise CampaignError(f"Vulkan mustpass fraction out of range: {index}")
    version = 4 if index == 3 else 3
    return AppleMustpassProfile(
        index=index,
        identifier=(
            f"vulkan-mustpass-{index:02d}"
            f"of{APPLE_MUSTPASS_FRACTIONS:02d}-v{version}"
        ),
        guest_memory_mib=(
            APPLE_MUSTPASS_FRACTION03_GUEST_MEMORY_MIB
            if index == 3
            else APPLE_MUSTPASS_GUEST_MEMORY_MIB
        ),
        restart_before=(
            APPLE_MUSTPASS_FRACTION03_RESTART_BEFORE
            if index == 3
            else None
        ),
        restart_pause_seconds=(
            APPLE_MUSTPASS_FRACTION03_RESTART_PAUSE_SECONDS
            if index == 3
            else 0
        ),
    )


def apple_mustpass_campaign(index: int) -> str:
    """Name the campaign that runs fraction ``index`` of the released list."""

    return apple_mustpass_profile(index).identifier


def apple_mustpass_profile_for_campaign(
    identifier: str,
) -> AppleMustpassProfile | None:
    """Resolve a baked mustpass identifier without accepting stale versions."""

    for index in range(APPLE_MUSTPASS_FRACTIONS):
        profile = apple_mustpass_profile(index)
        if identifier == profile.identifier:
            return profile
    return None


def device_exclusion_partition(
    cases: Sequence[str],
    exclusions: Sequence[DeviceExclusion] = DEVICE_EXCLUSIONS,
) -> tuple[tuple[str, ...], dict[str, tuple[str, ...]]]:
    """Split a case list into what a device campaign runs and what it skips.

    The skipped cases are grouped by reason, in the order the exclusions are
    declared, and the first matching reason owns a case.
    """

    selected: list[str] = []
    skipped: dict[str, list[str]] = {
        exclusion.reason: [] for exclusion in exclusions
    }
    for case in cases:
        for exclusion in exclusions:
            if exclusion.matches(case):
                skipped[exclusion.reason].append(case)
                break
        else:
            selected.append(case)
    return tuple(selected), {
        reason: tuple(cases) for reason, cases in skipped.items()
    }


def validate_device_exclusions(
    exclusions: Sequence[DeviceExclusion],
) -> tuple[DeviceExclusion, ...]:
    """Check that every exclusion names a cause, cases, and no mandatory case."""

    seen: set[str] = set()
    for exclusion in exclusions:
        if EXCLUSION_REASON.fullmatch(exclusion.reason) is None:
            raise CampaignError(
                f"a device exclusion reason is not a cause: {exclusion.reason}"
            )
        if exclusion.reason in seen:
            raise CampaignError(
                f"a device exclusion reason repeats: {exclusion.reason}"
            )
        seen.add(exclusion.reason)
        if not exclusion.patterns:
            raise CampaignError(
                f"device exclusion {exclusion.reason} names no cases"
            )
        for pattern in exclusion.patterns:
            if not pattern.startswith("dEQP-VK."):
                raise CampaignError(
                    f"a device exclusion must name dEQP-VK cases: {pattern}"
                )
            # A pattern names at least one group below the module, so no
            # exclusion can be written that takes the whole suite.
            if (
                not pattern[len("dEQP-VK.") :].rstrip(".")
                or CASE_NAME.fullmatch(pattern.rstrip(".")) is None
            ):
                raise CampaignError(
                    f"a device exclusion is not a case or group: {pattern}"
                )
    _, mandatory = device_exclusion_partition(
        receipt_mandatory_cases(), exclusions
    )
    for reason, cases in mandatory.items():
        if cases:
            raise CampaignError(
                f"device exclusion {reason} covers the mandatory case "
                f"{cases[0]}"
            )
    return tuple(exclusions)


def _exclusion_expression(pattern: str) -> str:
    """Return the anchored basic regular expression matching one pattern."""

    escaped = pattern.replace(".", "[.]")
    return f"^{escaped}" if pattern.endswith(".") else f"^{escaped}$"


def apple_campaigns() -> tuple[str, ...]:
    """Every Vulkan campaign a Kernarium automation run can boot."""

    return (
        APPLE_DEVELOPMENT_CAMPAIGN,
        APPLE_RECEIPT_SMOKE_CAMPAIGN,
        *(
            apple_mustpass_campaign(index)
            for index in range(APPLE_MUSTPASS_FRACTIONS)
        ),
    )


@dataclass(frozen=True)
class Fraction:
    index: int
    count: int

    def select(self, cases: Sequence[str]) -> tuple[str, ...]:
        selected = tuple(cases[self.index :: self.count])
        if not selected:
            raise CampaignError("CTS fraction selects no cases")
        return selected

    def argument(self) -> str:
        return f"{self.index},{self.count}"


@dataclass(frozen=True)
class DevelopmentSelection:
    mode: str
    source_cases: tuple[str, ...]
    expected_cases: tuple[str, ...]
    fraction: Fraction


@dataclass(frozen=True)
class ReceiptSelection:
    mode: str
    source_cases: tuple[str, ...]
    expected_cases: tuple[str, ...]
    fraction: Fraction
    generated_expected: bool


def case_needs_wsi(case: str) -> bool:
    # WSI cases are not confined to the dEQP-VK.wsi group: the
    # image_compression_control swapchain family creates real surfaces and
    # swapchains from dEQP-VK.api, and reports NotSupported without a display.
    return case.startswith("dEQP-VK.wsi.") or ".swapchain." in case


def case_needs_wayland(case: str) -> bool:
    return case_needs_wsi(case) and ".wayland." in case


def receipt_selection_uses_wsi(selection: ReceiptSelection) -> bool:
    # The authenticated top-level list includes WSI. Its native fractions are
    # enumerated in the guest, so generated selections have no source_cases yet.
    return selection.generated_expected or any(
        case_needs_wsi(case) for case in selection.source_cases
    )


def receipt_selection_uses_wayland(selection: ReceiptSelection) -> bool:
    return selection.generated_expected or any(
        case_needs_wayland(case) for case in selection.source_cases
    )


def validate_wsi_operation(
    selection: ReceiptSelection,
    results: dict[str, str],
    renderer: str,
) -> None:
    if not receipt_selection_uses_wsi(selection):
        return
    try:
        common.validate_native_x11_probe(renderer)
    except CampaignError as error:
        raise CampaignError(f"Vulkan WSI evidence rejected: {error}") from error
    if receipt_selection_uses_wayland(selection):
        evidence = renderer.splitlines()
        if "wayland_server=kwin-x11" not in evidence:
            raise CampaignError(
                "Vulkan WSI evidence omits the nested Wayland server"
            )
        # A compositor that locks its idle session stops painting its clients,
        # and a surface the compositor is not painting is owed no frame
        # callback: every FIFO presentation after that waits out its bound.
        if "wayland_lockscreen=off" not in evidence:
            raise CampaignError(
                "Vulkan WSI evidence omits the compositor lock screen state"
            )
    if not any(
        ".incremental_present." in case for case in selection.source_cases
    ):
        return
    reference_passes = {
        case.removesuffix(".reference")
        for case, status in results.items()
        if case.endswith(".reference") and status == "Pass"
    }
    incremental_passes = {
        case.removesuffix(".incremental_present")
        for case, status in results.items()
        if case.endswith(".incremental_present") and status == "Pass"
    }
    if not reference_passes & incremental_passes:
        raise CampaignError(
            "Vulkan WSI selection lacks a matching reference/incremental Pass pair"
        )


def validate_headless_wsi_results(results: dict[str, str]) -> None:
    """Require the headless device lane to reject every WSI-dependent case."""

    # These two leaves use device-group queries or VK_NULL_HANDLE; neither
    # creates a native window or a live VkSurfaceKHR. Their verdicts still go
    # through the same released CTS status oracle as every other result.
    without_surface = re.compile(
        r"dEQP-VK\.wsi\.[^.]+\.surface\."
        r"(?:query_devgroup_present_capabilities|destroy_null_handle)\Z"
    )
    unexpected = [
        (case, status)
        for case, status in results.items()
        if case_needs_wsi(case)
        and without_surface.fullmatch(case) is None
        and status != "NotSupported"
    ]
    if unexpected:
        case, status = unexpected[0]
        raise CampaignError(
            f"headless Vulkan WSI case returned {case}={status}"
        )


RECEIPT_QPA_CASE_ORDER = (
    "dEQP-VK.info.build",
    "dEQP-VK.info.device",
    "dEQP-VK.info.platform",
    "dEQP-VK.info.memory_limits",
    "dEQP-VK.info.physical_devices",
    "dEQP-VK.info.physical_device_groups",
    "dEQP-VK.info.instance_layers",
    "dEQP-VK.info.instance_extensions",
    "dEQP-VK.info.instance_extension_dependencies",
    "dEQP-VK.info.instance_extension_device_functions",
    "dEQP-VK.info.device_features",
    "dEQP-VK.info.device_properties",
    "dEQP-VK.info.device_queue_family_properties",
    "dEQP-VK.info.device_memory_properties",
    "dEQP-VK.info.device_layers",
    "dEQP-VK.info.device_extensions",
    "dEQP-VK.info.device_extension_dependencies",
    "dEQP-VK.info.device_no_khx_extensions",
    "dEQP-VK.info.device_memory_budget",
    "dEQP-VK.info.device_mandatory_features",
    "dEQP-VK.info.device_group_peer_memory_features",
)


def receipt_mandatory_cases() -> tuple[str, ...]:
    try:
        cases = tuple(
            line
            for line in RECEIPT_MANDATORY_PATH.read_text(
                encoding="utf-8"
            ).splitlines()
            if line and not line.startswith("#")
        )
    except OSError as error:
        raise CampaignError(f"cannot read receipt mandatory cases: {error}") from error
    if (
        len(cases) != 21
        or len(set(cases)) != 21
        or set(cases) != set(RECEIPT_QPA_CASE_ORDER)
        or any(CASE_NAME.fullmatch(case) is None for case in cases)
        or any(not case.startswith("dEQP-VK.info.") for case in cases)
    ):
        raise CampaignError("receipt mandatory case list changed")
    return cases


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=common.reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise CampaignError(f"cannot read JSON contract {path}: {error}") from error
    if not isinstance(value, dict):
        raise CampaignError(f"JSON contract is not an object: {path}")
    return value


def _positive_integer(value: Any, field: str) -> int:
    if type(value) is not int or value < 1:
        raise CampaignError(f"{field} must be a positive integer")
    return value


def _sha256(value: Any, field: str) -> str:
    if not isinstance(value, str) or SHA256.fullmatch(value) is None:
        raise CampaignError(f"{field} is not a SHA-256 digest")
    return value


def _absolute_guest_path(value: Any, field: str) -> str:
    if (
        not isinstance(value, str)
        or not value.startswith("/")
        or "\x00" in value
        or any(part in {"", ".", ".."} for part in Path(value).parts[1:])
    ):
        raise CampaignError(f"{field} is not a normalized absolute guest path")
    return value


def load_identity(path: Path = IDENTITY_PATH) -> dict[str, Any]:
    value = load_json(path)
    if value.get("schema_version") != 1:
        raise CampaignError("unsupported Vulkan CTS identity schema")
    rootfs = value.get("rootfs")
    cts = value.get("cts")
    icd = value.get("virtio_icd")
    cases = value.get("mandatory_cases")
    baseline = value.get("development_no_device_baseline")
    oracle = value.get("released_status_oracle")
    if not all(isinstance(item, dict) for item in (rootfs, cts, icd, baseline, oracle)):
        raise CampaignError("Vulkan CTS identity is missing a contract object")
    _positive_integer(rootfs.get("size"), "rootfs.size")
    _sha256(rootfs.get("sha256"), "rootfs.sha256")
    if rootfs.get("filesystem") != "erofs":
        raise CampaignError("the development Vulkan CTS lower must be EROFS")
    binary = cts.get("binary")
    installed_list = cts.get("installed_mandatory_list")
    source_list = cts.get("source_mandatory_list")
    if not all(isinstance(item, dict) for item in (binary, installed_list, source_list)):
        raise CampaignError("Vulkan CTS binary/list identity is incomplete")
    for name, row in (
        ("cts.binary", binary),
        ("cts.installed_mandatory_list", installed_list),
        ("cts.source_mandatory_list", source_list),
    ):
        _absolute_guest_path(row.get("path"), f"{name}.path")
        _positive_integer(row.get("size"), f"{name}.size")
        _sha256(row.get("sha256"), f"{name}.sha256")
    if binary.get("build_id") != "b4652c3d3923932193c272300201985e4d438863":
        raise CampaignError("development CTS build ID changed")
    if cts.get("qualification_ready") is not False or cts.get(
        "authoritative_build_receipt"
    ) is not None:
        raise CampaignError("the original image must remain development-only")
    _absolute_guest_path(icd.get("manifest_path"), "virtio_icd.manifest_path")
    _sha256(icd.get("manifest_sha256"), "virtio_icd.manifest_sha256")
    _absolute_guest_path(
        icd.get("resolved_library_path"), "virtio_icd.resolved_library_path"
    )
    _sha256(
        icd.get("resolved_library_sha256"),
        "virtio_icd.resolved_library_sha256",
    )
    if icd.get("library_path") != "libvulkan_virtio.so":
        raise CampaignError("Vulkan CTS identity does not force the Virtio ICD")
    if (
        not isinstance(cases, list)
        or len(cases) != installed_list.get("case_count")
        or len(cases) != source_list.get("case_count")
        or len(cases) != len(set(cases))
        or any(
            not isinstance(case, str)
            or CASE_NAME.fullmatch(case) is None
            or not case.startswith("dEQP-VK.info.")
            for case in cases
        )
    ):
        raise CampaignError("the mandatory Vulkan information-case list changed")
    if installed_list.get("sha256") != source_list.get("sha256"):
        raise CampaignError("installed and source mandatory-list identities differ")
    if baseline.get("forced_driver_file") != icd.get("manifest_path"):
        raise CampaignError("no-device baseline does not force the frozen Virtio ICD")
    if (
        baseline.get("process_return_code") != 137
        or baseline.get("qpa_size") != 0
        or baseline.get("qpa_sha256") != hashlib.sha256(b"").hexdigest()
    ):
        raise CampaignError("expected no-device baseline changed")
    for field in ("required_transcript", "experimental_venus_required_transcript"):
        rows = baseline.get(field)
        if (
            not isinstance(rows, list)
            or not rows
            or len(rows) != len(set(rows))
            or any(not isinstance(row, str) or not row for row in rows)
        ):
            raise CampaignError(f"development_no_device_baseline.{field} changed")
    successful = oracle.get("successful")
    failures = oracle.get("always_failure")
    if (
        not isinstance(successful, list)
        or not isinstance(failures, list)
        or not successful
        or not failures
        or set(successful) & set(failures)
        or "Waiver" not in failures
        or oracle.get("qualification_authority") is not False
    ):
        raise CampaignError("released status oracle is not fail closed")
    return value


def load_campaigns(path: Path = CAMPAIGNS_PATH) -> dict[str, Any]:
    value = load_json(path)
    if value.get("schema_version") != 1:
        raise CampaignError("unsupported Vulkan CTS campaign schema")
    physical = value.get("physical_device")
    if not isinstance(physical, dict) or physical != {
        "state": "held",
        "launch_permitted": False,
        "initial_memory_mib": 2048,
        "swap_permitted": False,
    }:
        raise CampaignError("physical-device Vulkan lane is not explicitly held")
    return value


def parse_fraction(value: str) -> Fraction:
    match = re.fullmatch(r"([0-9]+)[,/](0*[1-9][0-9]*)", value)
    if match is None:
        raise argparse.ArgumentTypeError("must be INDEX/COUNT with COUNT positive")
    fraction = Fraction(int(match.group(1)), int(match.group(2)))
    if fraction.index >= fraction.count:
        raise argparse.ArgumentTypeError("fraction INDEX must be below COUNT")
    return fraction


def exact_case(value: str) -> str:
    if CASE_NAME.fullmatch(value) is None:
        raise argparse.ArgumentTypeError("must be one exact CTS case name")
    return value


def resolve_selection(
    identity: dict[str, Any],
    mode: str,
    exact_cases: Sequence[str],
    family: str | None,
    fraction: Fraction,
) -> DevelopmentSelection:
    mandatory = tuple(identity["mandatory_cases"])
    if mode in {"frozen-regression", "current-must-pass", "capability-inventory"}:
        raise CampaignError(
            f"{mode} is dormant until a separately authorized receipt-backed "
            "Vulkan derivative exists"
        )
    if mode == "smoke":
        if exact_cases or family is not None:
            raise CampaignError("smoke owns the exact 19-case information list")
        source = mandatory
    elif mode == "exact-family":
        if bool(exact_cases) == (family is not None):
            raise CampaignError(
                "exact-family requires either one or more --case values or one --family"
            )
        if exact_cases:
            if len(exact_cases) != len(set(exact_cases)):
                raise CampaignError("exact-family contains a duplicate case")
            unknown = [case for case in exact_cases if case not in mandatory]
            if unknown:
                raise CampaignError(
                    "pre-authorization exact cases must come from the 19-case "
                    f"information list: {unknown[0]}"
                )
            source = tuple(exact_cases)
        else:
            if CASE_NAME.fullmatch(family or "") is None:
                raise CampaignError("family is not an exact CTS name prefix")
            source = tuple(case for case in mandatory if case.startswith(family or ""))
            if not source:
                raise CampaignError("family selects no mandatory information cases")
    else:
        raise CampaignError(f"unsupported Vulkan CTS mode: {mode}")
    return DevelopmentSelection(mode, source, fraction.select(source), fraction)


def resolve_receipt_selection(
    mode: str,
    exact_cases: Sequence[str],
    family: str | None,
    fraction: Fraction,
) -> ReceiptSelection:
    mandatory = receipt_mandatory_cases()
    if mode == "smoke":
        if fraction != Fraction(0, 1):
            raise CampaignError(
                "receipt-backed smoke uses the complete native mandatory list"
            )
        if exact_cases or family is not None:
            raise CampaignError("receipt-backed smoke owns the complete mandatory list")
        source = mandatory
    elif mode == "exact-family":
        if fraction != Fraction(0, 1):
            raise CampaignError("receipt-backed exact-family cannot be fractioned")
        if bool(exact_cases) == (family is not None):
            raise CampaignError(
                "receipt-backed exact-family requires --case values or one --family"
            )
        if exact_cases:
            if len(exact_cases) != len(set(exact_cases)):
                raise CampaignError("receipt-backed exact-family repeats a case")
            invalid = [
                case for case in exact_cases
                if CASE_NAME.fullmatch(case) is None
            ]
            if invalid:
                raise CampaignError(
                    f"receipt-backed exact-family has an invalid case: {invalid[0]}"
                )
            source = tuple(exact_cases)
        else:
            source = tuple(case for case in mandatory if case.startswith(family or ""))
            if not source:
                raise CampaignError("receipt-backed family selects no mandatory cases")
    elif mode == "current-must-pass":
        if exact_cases or family is not None:
            raise CampaignError(
                "current-must-pass owns the authenticated released top-level list"
            )
        if fraction.count > 16:
            raise CampaignError("released CTS permits at most 16 native fractions")
        return ReceiptSelection(mode, (), (), fraction, True)
    elif mode in {
        "frozen-regression",
        "capability-inventory",
    }:
        raise CampaignError(
            f"{mode} remains gated until mandatory enumeration, capability "
            "inventory, and exact released-manifest sharding pass"
        )
    else:
        raise CampaignError(f"unsupported receipt-backed Vulkan CTS mode: {mode}")
    # The released CTS walks its test hierarchy when executing a caselist; it
    # does not preserve the caselist file's alphabetical order. Keep both
    # orders explicit so the guest consumes the receipt-authenticated file
    # order while QPA validation remains strict about the released traversal.
    if exact_cases and any(case not in mandatory for case in source):
        expected = source
    else:
        expected = tuple(case for case in RECEIPT_QPA_CASE_ORDER if case in source)
    if len(expected) != len(source):
        raise CampaignError("receipt-backed QPA traversal order is incomplete")
    return ReceiptSelection(mode, source, expected, fraction, False)


def receipt_deqp_arguments(
    receipt: dict[str, Any],
    selection: ReceiptSelection,
    output: str,
    *,
    resume_after: int = 0,
    preselected: bool = False,
    caselist: str = "selected.txt",
) -> tuple[str, ...]:
    install = receipt["cts"]["install_prefix"]
    arguments = [
        receipt["cts"]["executable"]["path"],
        f"--deqp-caselist-file={output}/{caselist}",
        f"--deqp-archive-dir={install}",
    ]
    # A resumed session executes an explicit suffix of the fraction the guest
    # already enumerated, and a device-memory selection an explicit subset of
    # it, so in both cases the released fraction filter must not run again.
    if selection.generated_expected and not resume_after and not preselected:
        arguments.extend(
            (
                f"--deqp-fraction={selection.fraction.argument()}",
                "--deqp-fraction-mandatory-caselist-file="
                f"{receipt['mustpass']['mandatory']['path']}",
            )
        )
    arguments.extend(
        (
            f"--deqp-log-filename={output}/results.qpa",
            "--deqp-log-images=disable",
            "--deqp-log-shader-sources=disable",
            "--deqp-test-oom=disable",
            "--deqp-watchdog=disable",
            "--deqp-crashhandler=disable",
            "--deqp-visibility=hidden",
        )
    )
    return tuple(arguments)


def receipt_list_arguments(
    receipt: dict[str, Any], selection: ReceiptSelection, output: str
) -> tuple[str, ...]:
    if not selection.generated_expected:
        raise CampaignError("only a released-manifest fraction needs enumeration")
    install = receipt["cts"]["install_prefix"]
    return (
        receipt["cts"]["executable"]["path"],
        "--deqp-runmode=txt-caselist",
        f"--deqp-caselist-file={output}/selected.txt",
        f"--deqp-archive-dir={install}",
        f"--deqp-fraction={selection.fraction.argument()}",
        "--deqp-fraction-mandatory-caselist-file="
        f"{receipt['mustpass']['mandatory']['path']}",
        "--deqp-log-filename=/dev/null",
        "--deqp-watchdog=disable",
        "--deqp-crashhandler=disable",
    )


def deqp_arguments(
    identity: dict[str, Any], selection: DevelopmentSelection, output: str
) -> tuple[str, ...]:
    cts = identity["cts"]
    arguments = [
        cts["binary"]["path"],
        f"--deqp-caselist-file={output}/selected.txt",
        f"--deqp-archive-dir={cts['archive_directory']}",
    ]
    if selection.fraction != Fraction(0, 1):
        arguments.append(f"--deqp-fraction={selection.fraction.argument()}")
    arguments.extend(
        (
            f"--deqp-log-filename={output}/results.qpa",
            "--deqp-log-images=disable",
            "--deqp-log-shader-sources=disable",
            "--deqp-test-oom=disable",
            "--deqp-watchdog=disable",
            "--deqp-crashhandler=disable",
            "--deqp-visibility=hidden",
        )
    )
    return tuple(arguments)


def validate_released_results(
    results: dict[str, str], oracle: dict[str, Any]
) -> None:
    rejected = classify_released_results(results, oracle)
    if rejected:
        case, status = rejected[0]
        raise CampaignError(
            f"released Vulkan CTS status policy rejects {case}={status}"
        )


def classify_released_results(
    results: dict[str, str], oracle: dict[str, Any]
) -> tuple[tuple[str, str], ...]:
    successful = set(oracle["successful"])
    failures = set(oracle["always_failure"])
    known = successful | failures
    unknown = [
        (case, status) for case, status in results.items() if status not in known
    ]
    if unknown:
        case, status = unknown[0]
        raise CampaignError(f"released Vulkan CTS returned unknown status {case}={status}")
    return tuple(
        (case, status)
        for case, status in results.items()
        if status not in successful
    )


def _parse_case_artifact(payload: bytes, label: str) -> tuple[str, ...]:
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise CampaignError(f"{label} is not UTF-8") from error
    if text and not text.endswith("\n"):
        raise CampaignError(f"{label} lacks its final newline")
    cases = tuple(text.splitlines())
    if (
        not cases
        or len(cases) != len(set(cases))
        or any(CASE_NAME.fullmatch(case) is None for case in cases)
    ):
        raise CampaignError(f"{label} is not a unique exact-case list")
    return cases


def _status_rows_digest(rows: Sequence[tuple[str, str]]) -> str:
    payload = "".join(f"{case}\t{status}\n" for case, status in rows).encode()
    return hashlib.sha256(payload).hexdigest()


def validate_virtio_qpa_identity(qpa: str) -> None:
    if FORBIDDEN_SOFTWARE_ICD.search(qpa):
        raise CampaignError("Vulkan QPA selected lavapipe or another software ICD")
    if VIRTIO_IDENTITY.search(qpa) is None:
        raise CampaignError("Vulkan QPA does not identify a Virtio/Venus device")


def _selected_payload(selection: DevelopmentSelection | ReceiptSelection) -> str:
    return "".join(f"{case}\n" for case in selection.source_cases)


def selected_list_setup(
    selection: DevelopmentSelection | ReceiptSelection, case_list: str | None
) -> str:
    """Shell that writes the selected cases to ``$selected`` for the CTS user.

    Every harness installs the list as the guest file ``case_list``, whatever
    its length, and the launcher checks it against its digest.
    """
    if case_list is None:
        raise CampaignError("a selected case list must reach the guest as a file")
    payload = _selected_payload(selection).encode()
    digest = hashlib.sha256(payload).hexdigest()
    return (
        f'install -m 0644 -o uml -g uml {shlex.quote(case_list)} "$selected"\n'
        '[ "$(sha256sum < "$selected" | cut -d \' \' -f 1)" = '
        f"{digest} ] || {{\n"
        "    echo 'UML_VK_CTS_ERROR: installed case list identity changed'\n"
        "    exit 1\n"
        "}"
    )


def no_device_required_transcript(
    identity: dict[str, Any], *, experimental_venus: bool
) -> tuple[str, ...]:
    field = (
        "experimental_venus_required_transcript"
        if experimental_venus
        else "required_transcript"
    )
    return tuple(identity["development_no_device_baseline"][field])


def build_guest_launcher(
    identity: dict[str, Any],
    selection: DevelopmentSelection,
    *,
    timeout: int,
    experimental_venus: bool = False,
    case_list: str | None = None,
) -> str:
    cts = identity["cts"]
    icd = identity["virtio_icd"]
    selected_setup = selected_list_setup(selection, case_list)
    command = deqp_arguments(identity, selection, "/run/uml-vk-cts")
    command_separator = " " + chr(92) + "\n    "
    quoted_command = command_separator.join(
        shlex.quote(argument) for argument in command
    )
    required_transcript = no_device_required_transcript(
        identity, experimental_venus=experimental_venus
    )
    required_rows = "\n".join(
        "printf '%s\\n' "
        + shlex.quote(f"required_transcript_{index}={row}")
        + ' >> "$summary"'
        for index, row in enumerate(required_transcript)
    )
    transport_functions = common.artifact_transport_program(
        error_prefix="UML_VK_CTS_ERROR:",
        base64_line_length=ARTIFACT_BASE64_LINE_LENGTH,
        missing_source_status=1,
    )
    return f"""#!/bin/sh
set -eu
output=/run/uml-vk-cts
selected="$output/selected.txt"
qpa="$output/results.qpa"
renderer="$output/renderer.txt"
summary="$output/summary.txt"
channel={shlex.quote(common.VIRTIO_SERIAL_GUEST_DEVICE)}
channel_ready=0
artifact_framed=0
status=1

{transport_functions}
finish()
{{
    trap - EXIT HUP INT TERM
    # The exit path never stops early: it is the only place the host's final
    # frame can come from.
    set +e
    printf 'cts_rc=%s\n' "$status" > "$summary"
    printf 'mode=%s\n' {shlex.quote(selection.mode)} >> "$summary"
    printf 'expected_case_count=%s\n' {len(selection.expected_cases)} >> "$summary"
    printf 'forced_driver_file=%s\n' {shlex.quote(icd['manifest_path'])} >> "$summary"
    {required_rows}
    transfer=0
    prepare_channel || transfer=1
    for name in renderer.txt selected.txt results.qpa; do
        emit_artifact "$name" "$output/$name" || transfer=1
    done
    # Sent last, and never withheld because an earlier artifact failed:
    # the host's wait ends on this frame.
    emit_artifact summary.txt "$summary" || transfer=1
    if [ "$channel_ready" -eq 1 ]; then
        exec 3>&-
    fi
    [ "$transfer" -eq 0 ] || echo 'UML_VK_CTS_ERROR: artifact transfer failed'
    sync
}}
trap finish EXIT HUP INT TERM

install -d -m 0755 -o uml -g uml "$output"
{selected_setup}
: > "$qpa"
chown uml:uml "$qpa"
actual_page_size=$(getconf PAGESIZE)
[ "$actual_page_size" -eq {gpu_guest.GUEST_PAGE_SIZE} ] || {{
    echo "UML_VK_CTS_ERROR: guest page size $actual_page_size does not match {gpu_guest.GUEST_PAGE_SIZE}"
    exit 1
}}
printf 'guest_page_size=%s\n' "$actual_page_size" > "$renderer"
printf 'vk_driver_files=%s\n' {shlex.quote(icd['manifest_path'])} >> "$renderer"
dpkg-query -W -f='${{Package}}=${{Version}}\\n' \
    mesa-vulkan-drivers libvulkan1 libdrm2 >> "$renderer"
cat {shlex.quote(icd['manifest_path'])} >> "$renderer"

verify_file()
{{
    file="$1"
    expected_size="$2"
    expected_sha="$3"
    actual_size=$(wc -c < "$file" | tr -d ' ')
    [ "$actual_size" -eq "$expected_size" ] || return 1
    actual_sha=$(sha256sum "$file" | cut -d ' ' -f 1)
    [ "$actual_sha" = "$expected_sha" ]
}}

verify_file {shlex.quote(cts['binary']['path'])} \
    {cts['binary']['size']} {shlex.quote(cts['binary']['sha256'])} || {{
    echo 'UML_VK_CTS_ERROR: CTS executable identity changed'
    exit 1
}}
verify_file {shlex.quote(cts['installed_mandatory_list']['path'])} \
    {cts['installed_mandatory_list']['size']} \
    {shlex.quote(cts['installed_mandatory_list']['sha256'])} || {{
    echo 'UML_VK_CTS_ERROR: installed mandatory list changed'
    exit 1
}}
verify_file {shlex.quote(icd['manifest_path'])} \
    {icd['manifest_size']} {shlex.quote(icd['manifest_sha256'])} || {{
    echo 'UML_VK_CTS_ERROR: Virtio ICD manifest changed'
    exit 1
}}
verify_file {shlex.quote(icd['resolved_library_path'])} \
    {icd['resolved_library_size']} \
    {shlex.quote(icd['resolved_library_sha256'])} || {{
    echo 'UML_VK_CTS_ERROR: Virtio ICD library changed'
    exit 1
}}

cd {shlex.quote(cts['archive_directory'])}
set +e
runuser -u uml -- env \
    HOME=/home/uml \
    VK_DRIVER_FILES={shlex.quote(icd['manifest_path'])} \
    timeout --signal=TERM --kill-after=30 {timeout} \
    {quoted_command}
status=$?
set -e
printf 'UML_VK_CTS_STATUS|rc=%s\n' "$status"
exit "$status"
"""


def _receipt_file(receipt: dict[str, Any], path: str) -> dict[str, Any]:
    records = [
        receipt["cts"]["executable"],
        receipt["mustpass"]["top_level"],
        receipt["mustpass"]["mandatory"],
        receipt["runner_oracle"]["file"],
        receipt["guest_receipt"],
        receipt["vulkan_loader"]["runtime"],
        *receipt["mesa"]["files"],
    ]
    matching = [row for row in records if row["path"] == path]
    if len(matching) != 1:
        raise CampaignError(f"receipt does not name exactly one file at {path}")
    return matching[0]


def build_receipt_guest_launcher(
    receipt: dict[str, Any],
    selection: ReceiptSelection,
    *,
    timeout: int,
    progress_timeout: int = 0,
    use_wsi: bool = False,
    receipt_path: Path = RECEIPT_PATH,
    resume_after: int = 0,
    checkpoint_bytes: int = common.QPA_CHECKPOINT_BYTES,
    progress_interval: int = 15,
    relax_diagnostics: bool = False,
    device_exclusions: Sequence[DeviceExclusion] = (),
    case_list: str | None = None,
) -> str:
    if resume_after < 0 or checkpoint_bytes < 1 or progress_interval < 1:
        raise CampaignError("Vulkan shard resumption inputs are invalid")
    exclusions = validate_device_exclusions(device_exclusions)
    device_profile: AppleMustpassProfile | None = None
    # A fraction whose first session ends early is continued in fresh dEQP
    # processes. The bound is on sessions, not on time: the whole run shares
    # one deadline, so a continuation can only use what the first left.
    continuation_limit = VULKAN_CTS_SESSION_LIMIT
    if exclusions and not selection.generated_expected:
        raise CampaignError(
            "device exclusions apply to an enumerated released fraction"
        )
    if exclusions and resume_after:
        raise CampaignError(
            "a device exclusion and a resumed shard are not combined"
        )
    if exclusions:
        if selection.fraction.count != APPLE_MUSTPASS_FRACTIONS:
            raise CampaignError(
                "a baked device profile requires the native 16-way fraction"
            )
        device_profile = apple_mustpass_profile(selection.fraction.index)
        if bool(device_profile.restart_before) != bool(
            device_profile.restart_pause_seconds
        ):
            raise CampaignError("a planned device restart profile is invalid")
    # Venus reports a stuck wait on the CTS process's own stderr, which is
    # redirected into a guest file and never leaves the guest. Surfacing it
    # names what the guest is waiting on at the moment a shard stalls, which
    # no host-side evidence can currently show. Off by default: the generated
    # script is an input to every recorded harness command, so changing it
    # unconditionally would invalidate the attempts a campaign already has.
    relax_watch = """
    if [ "$stalled_seconds" -gt 0 ]; then
        relax_size=$(wc -c < "$cts_output" | tr -d ' ')
        if [ "$relax_size" -gt "$relax_offset" ]; then
            tail -c "+$((relax_offset + 1))" "$cts_output" 2>/dev/null | \
                grep -a 'stuck in' | tail -n 5 | \
                while IFS= read -r relax_line; do
                    printf 'UML_VK_CTS_RELAX|%s\\n' "$relax_line"
                done
            relax_offset=$relax_size
        fi
    fi
""" if relax_diagnostics else ""
    relax_state = "relax_offset=0\n" if relax_diagnostics else ""
    # Measured on the device: a mustpass fraction's results reach ~217 MB and
    # the CTS process's own output another ~90 MB, in a tmpfs that starts the
    # run with ~257 MB free. That output is the only thing here nobody reads --
    # no lane transports it -- so it is what gives way, rather than the results.
    # The watch above is its one reader, and reads it by offset, so the two are
    # not generated together.
    spare_cts_output = "" if relax_diagnostics else """    if [ "$output_free_kb" -lt 65536 ]; then
        : > "$cts_output"
        echo 'UML_VK_CTS_NOTE: released the CTS output to keep room for results'
    fi
"""
    output = "/run/uml-vk-cts"
    icd_path = "/usr/share/vulkan/icd.d/virtio_icd.json"
    library_path = "/usr/lib/aarch64-linux-gnu/libvulkan_virtio.so"
    cts = receipt["cts"]
    loader_runtime = receipt["vulkan_loader"]["runtime"]
    arguments = receipt_deqp_arguments(
        receipt,
        selection,
        output,
        resume_after=resume_after,
        preselected=bool(exclusions),
    )
    separator = " " + chr(92) + "\n        "
    quoted_command = separator.join(shlex.quote(argument) for argument in arguments)
    # A continuation runs an explicit remainder of the traversal the guest has
    # already enumerated, so the released fraction's own filter must not run
    # again over it.
    continuation_command = separator.join(
        shlex.quote(argument)
        for argument in receipt_deqp_arguments(
            receipt,
            selection,
            output,
            preselected=True,
            caselist=CONTINUATION_CASELIST,
        )
    )
    planned_restart_setup = ""
    planned_restart_state = ""
    first_session_condition = '[ "$cts_sessions" -eq 1 ]'
    continuation_selection = """    tail -n +$((session_cases + 1)) "$run_list" > "$continuation.next" || break
    mv "$continuation.next" "$continuation" || break
    chown uml:uml "$continuation"
    run_list="$continuation"
"""
    continuation_note = (
        "    printf 'UML_VK_CTS_NOTE: continuing %s cases in a fresh session\n' "
        '        "$((selected_count - cts_cases))"\n'
    )
    # The file whose order the guest's results follow. Only a released
    # fraction that dEQP itself filters leaves that order in the enumerated
    # list rather than in the selection handed to it.
    run_list = (
        '"$expected"'
        if selection.generated_expected and not exclusions and not resume_after
        else '"$selected"'
    )
    selected_count_source = '"$run_list"'
    if device_profile is not None and device_profile.restart_before is not None:
        restart_case = shlex.quote(device_profile.restart_before)
        planned_restart_state = """planned_restart_pending=1
planned_prefix_count=0
"""
        first_session_condition += ' && [ "$planned_restart_pending" -eq 0 ]'
        run_list = '"$continuation"'
        selected_count_source = '"$selected"'
        planned_restart_setup = f"""restart_matches=$(grep -n -F -x -- {restart_case} "$selected" || :)
restart_match_count=$(printf '%s\n' "$restart_matches" | grep -c . || :)
[ "$restart_match_count" -eq 1 ] || {{
    echo 'UML_VK_CTS_ERROR: planned restart case is not unique in the selection'
    exit 1
}}
restart_line=${{restart_matches%%:*}}
[ "$restart_line" -gt 1 ] || {{
    echo 'UML_VK_CTS_ERROR: planned restart has no preceding cases'
    exit 1
}}
planned_prefix_count=$((restart_line - 1))
head -n "$planned_prefix_count" "$selected" > "$continuation"
chown uml:uml "$continuation"
[ -s "$continuation" ] || {{
    echo 'UML_VK_CTS_ERROR: planned restart prefix is empty'
    exit 1
}}
"""
        continuation_selection = """    planned_restart=0
    if [ "$planned_restart_pending" -eq 1 ]; then
        [ "$cts_cases" -le "$planned_prefix_count" ] || break
    fi
    if [ "$planned_restart_pending" -eq 1 ] && \\
       [ "$cts_cases" -eq "$planned_prefix_count" ]; then
        tail -n +$((planned_prefix_count + 1)) "$selected" \\
            > "$continuation.next" || break
        planned_restart_pending=0
        planned_restart=1
    else
        tail -n +$((session_cases + 1)) "$run_list" \\
            > "$continuation.next" || break
    fi
    mv "$continuation.next" "$continuation" || break
    chown uml:uml "$continuation"
    run_list="$continuation"
"""
        continuation_note = f"""    if [ "$planned_restart" -eq 1 ]; then
        pause_start=$(date +%s)
        pause_limit=$((pause_start + {device_profile.restart_pause_seconds}))
        [ "$pause_limit" -le "$deadline" ] || pause_limit=$deadline
        while [ "$(date +%s)" -lt "$pause_limit" ]; do
            sleep 1
        done
        printf 'UML_VK_CTS_NOTE: planned restart before %s; paused %s s for host graphics memory; continuing %s cases\\n' \\
            {restart_case} "$(($(date +%s) - pause_start))" \\
            "$((selected_count - cts_cases))"
    else
        printf 'UML_VK_CTS_NOTE: continuing %s cases in a fresh session\\n' \\
            "$((selected_count - cts_cases))"
    fi
"""
    if selection.generated_expected:
        selected_setup = (
            "cp "
            + shlex.quote(receipt["mustpass"]["top_level"]["path"])
            + ' "$selected"\nchown uml:uml "$selected"'
        )
        list_arguments = receipt_list_arguments(receipt, selection, output)
        quoted_list_command = separator.join(
            shlex.quote(argument) for argument in list_arguments
        )
        expected_setup = f"""cd \"$output\"
set +e
runuser -u uml -- env \\
    HOME=/home/uml \\
    VK_DRIVER_FILES={shlex.quote(icd_path)} \\
    timeout --signal=TERM --kill-after=30 300 \\
    {quoted_list_command}
list_status=$?
set -e
[ \"$list_status\" -eq 0 ] || {{
    echo \"UML_VK_CTS_ERROR: released case enumeration failed: $list_status\"
    exit 1
}}
grep '^TEST: ' \"$output/dEQP-VK-cases.txt\" | cut -c 7- > \"$expected\"
rm -f \"$output/dEQP-VK-cases.txt\"
[ -s \"$expected\" ] || {{
    echo 'UML_VK_CTS_ERROR: released fraction selected no cases'
    exit 1
}}
chown uml:uml \"$expected\"
"""
        if resume_after:
            expected_setup += f"""tail -n +{resume_after + 1} \"$expected\" > \"$selected\"
chown uml:uml \"$selected\"
[ -s \"$selected\" ] || {{
    echo 'UML_VK_CTS_ERROR: resumed fraction selected no cases'
    exit 1
}}
"""
        # The count is a snapshot of what this guest set out to run, taken
        # before any continuation rewrites the remaining selection.
        expected_count = '"${selected_count:-0}"'
        prerun_artifact_names = "renderer.txt selected.txt expected.txt"
        fraction_summary = (
            "    printf 'fraction=%s\\n' "
            + shlex.quote(selection.fraction.argument())
            + ' >> "$summary"\n'
        )
        if exclusions:
            # The enumerated fraction is split here rather than filtered by
            # dEQP, so the guest runs exactly the remainder and keeps the
            # excluded names against their reason. An empty skip list is a
            # normal outcome for a fraction no exclusion reaches.
            every = " ".join(
                "-e " + shlex.quote(_exclusion_expression(pattern))
                for exclusion in exclusions
                for pattern in exclusion.patterns
            )
            expected_setup += f"""grep -v {every} \"$expected\" > \"$selected\" || :
chown uml:uml \"$selected\"
[ -s \"$selected\" ] || {{
    echo 'UML_VK_CTS_ERROR: device exclusions selected no cases'
    exit 1
}}
"""
            for exclusion in exclusions:
                patterns = " ".join(
                    "-e " + shlex.quote(_exclusion_expression(pattern))
                    for pattern in exclusion.patterns
                )
                expected_setup += (
                    f"grep {patterns} \"$expected\" | "
                    f"awk -v reason={shlex.quote(exclusion.reason)} "
                    "'{ print reason \"\\t\" $0 }' >> \"$skipped\" || :\n"
                    f"skipped_{exclusion.row}=$(grep -c {patterns} "
                    "\"$expected\" || :)\n"
                )
            expected_setup += 'chown uml:uml "$skipped"\n'
            expected_count = '"${selected_count:-0}"'
            prerun_artifact_names += " skipped.txt"
            fraction_summary += (
                "    printf 'device_excluded=%s\\n' "
                '"$(wc -l < "$skipped" | tr -d \' \')" >> "$summary"\n'
            )
            for exclusion in exclusions:
                fraction_summary += (
                    f"    printf 'device_excluded_{exclusion.row}=%s\\n' "
                    f'"${{skipped_{exclusion.row}}}" >> "$summary"\n'
                    f"    printf 'device_exclusion_{exclusion.row}=%s\\n' "
                    + shlex.quote(" ".join(exclusion.patterns))
                    + ' >> "$summary"\n'
                )
            fraction_summary += (
                "    printf 'device_memory_mib=%s\\n' "
                f"{device_profile.guest_memory_mib}"
                ' >> "$summary"\n'
            )
    else:
        selected_setup = selected_list_setup(selection, case_list)
        expected_setup = ""
        if resume_after:
            expected_setup = (
                f'tail -n +{resume_after + 1} "$selected" > "$selected.resume"\n'
                'mv "$selected.resume" "$selected"\n'
                'chown uml:uml "$selected"\n'
            )
        if len(selection.expected_cases) - resume_after < 1:
            raise CampaignError("shard resumption selects no remaining cases")
        expected_count = '"${selected_count:-0}"'
        prerun_artifact_names = "renderer.txt selected.txt"
        fraction_summary = ""
    verified_files = (
        cts["executable"],
        receipt["mustpass"]["mandatory"],
        receipt["mustpass"]["top_level"],
        _receipt_file(receipt, icd_path),
        _receipt_file(receipt, library_path),
        loader_runtime,
        receipt["runner_oracle"]["file"],
        receipt["guest_receipt"],
    )
    verification_rows = "\n".join(
        "verify_file "
        + shlex.quote(row["path"])
        + f" {row['size']} "
        + shlex.quote(row["sha256"])
        + " || { echo 'UML_VK_CTS_ERROR: receipt file identity changed: "
        + row["path"]
        + "'; exit 1; }"
        for row in verified_files
    )
    receipt_digest = common.sha256_file(receipt_path)
    # The skip list and its per-reason counts exist from the first line, so the
    # exit trap can report them even when the enumeration itself failed.
    skipped_state = (
        'skipped="$output/skipped.txt"\n'
        + "".join(f"skipped_{item.row}=0\n" for item in exclusions)
        if exclusions
        else ""
    )
    skipped_creation = (
        ': > "$skipped"\nchown uml:uml "$skipped"\n' if exclusions else ""
    )
    # Linux truncates a thread name to fifteen characters plus its terminator.
    cts_comm = PurePosixPath(cts["executable"]["path"]).name[:15]
    use_wayland = use_wsi and receipt_selection_uses_wayland(selection)
    wsi_setup = common.native_x11_setup(wayland=use_wayland) if use_wsi else ""
    wsi_environment = ""
    wsi_cleanup = ""
    poweroff = ""
    if use_wsi:
        wsi_environment = """    DISPLAY="$xdisplay" \\
    XAUTHORITY="$xauthority" \\
    XDG_RUNTIME_DIR="$runtime" \\
"""
        if use_wayland:
            wsi_environment += """    WAYLAND_DISPLAY=wayland-0 \\
    DBUS_SESSION_BUS_ADDRESS="$dbus_address" \\
"""
        wsi_cleanup = """    if command -v native_display_cleanup >/dev/null 2>&1; then
        native_display_cleanup || echo 'UML_VK_CTS_ERROR: native display cleanup failed'
    fi
"""
        poweroff = (
            "    systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1\n"
        )
    transport_functions = common.artifact_transport_program(
        error_prefix="UML_VK_CTS_ERROR:",
        base64_line_length=ARTIFACT_BASE64_LINE_LENGTH,
        missing_source_status=1,
    )
    checkpoint_function = common.qpa_checkpoint_program()
    continuation_caselist = CONTINUATION_CASELIST
    return f"""#!/bin/sh
set -eu
output={output}
selected="$output/selected.txt"
expected="$output/expected.txt"
continuation="$output/{continuation_caselist}"
{skipped_state}qpa="$output/results.qpa"
renderer="$output/renderer.txt"
summary="$output/summary.txt"
cts_output="$output/cts-output.txt"
channel={shlex.quote(common.VIRTIO_SERIAL_GUEST_DEVICE)}
channel_ready=0
checkpoint_bytes={checkpoint_bytes}
progress_timeout={progress_timeout}
progress_interval={progress_interval}
prerun_emitted=0
artifact_framed=0
{relax_state}qpa_offset=0
qpa_segments=0
checkpoint_failures=0
progress_timed_out=0
status=1
cts_sessions=0
cts_cases=0
selected_count=0
{planned_restart_state}
{transport_functions}
{checkpoint_function}
emit_prerun()
{{
    [ "$prerun_emitted" -eq 0 ] || return 0
    for name in {prerun_artifact_names}; do
        emit_artifact "$name" "$output/$name" || return 1
    done
    prerun_emitted=1
}}

# The CTS process is a grandchild of this script through runuser and timeout,
# so its resident size is found by name.  A guest killed by the out-of-memory
# reaper leaves no other evidence of how close it was: the progress line is
# the only memory reading the device lane can ever see.
find_cts_process()
{{
    if [ -n "$cts_process" ] && [ -r "/proc/$cts_process/status" ]; then
        return 0
    fi
    cts_process=
    for entry in /proc/[0-9]*; do
        [ -r "$entry/comm" ] || continue
        read -r entry_comm < "$entry/comm" 2>/dev/null || continue
        [ "$entry_comm" = {shlex.quote(cts_comm)} ] || continue
        cts_process=${{entry#/proc/}}
        return 0
    done
    return 0
}}

cts_process_field()
{{
    field_value=
    if [ -n "$cts_process" ]; then
        field_value=$(sed -n \
            "s/^$1:[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p" \
            "/proc/$cts_process/status" 2>/dev/null)
    fi
    [ -n "$field_value" ] || field_value=0
    printf '%s' "$field_value"
}}

finish()
{{
    trap - EXIT HUP INT TERM
    # Nothing below may stop the exit path early. This is the only place the
    # host's final frame can come from, and errexit here once turned a single
    # failed write on a full guest filesystem into a fraction that ran every
    # case and delivered no summary at all.
    set +e
    transfer=0
    qpa_transfer=unsent
    prepare_channel || transfer=1
    if [ "$channel_ready" -eq 1 ]; then
        emit_prerun || transfer=1
        if flush_qpa 1; then
            qpa_transfer=complete
        elif [ "$artifact_framed" -eq 0 ]; then
            # Nothing of that segment reached the host, so its name is still
            # free. The growing QPA and the CTS process's own output share the
            # guest's tmpfs and that output is never transported: release it
            # and ask for the remaining results once more.
            : > "$cts_output" 2>/dev/null
            if flush_qpa 1; then
                qpa_transfer=recovered
            else
                qpa_transfer=failed
            fi
        else
            qpa_transfer=failed
        fi
    fi
    qpa_missing=0
    if [ -f "$qpa" ]; then
        qpa_missing=$(($(wc -c < "$qpa" | tr -d ' ') - qpa_offset))
        [ "$qpa_missing" -ge 0 ] || qpa_missing=0
    fi
    case "$qpa_transfer" in
    complete|recovered) ;;
    *) transfer=1 ;;
    esac
    # Every transported segment is on the host and nothing in the guest reads
    # these again: release the space the summary needs before writing it.
    : > "$cts_output" 2>/dev/null
    : > "$qpa" 2>/dev/null
    printf 'cts_rc=%s\n' "$status" > "$summary"
    printf 'mode=%s\n' {shlex.quote(selection.mode)} >> "$summary"
    printf 'expected_case_count=%s\n' {expected_count} >> "$summary"
    printf 'cts_sessions=%s\n' "${{cts_sessions:-0}}" >> "$summary"
    printf 'cts_cases_run=%s\n' "${{cts_cases:-0}}" >> "$summary"
{fraction_summary}    printf 'forced_driver_file=%s\n' {shlex.quote(icd_path)} >> "$summary"
    printf 'receipt_sha256=%s\n' {shlex.quote(receipt_digest)} >> "$summary"
    printf 'formal_conformance=false\n' >> "$summary"
    printf 'qpa_transfer=%s\n' "$qpa_transfer" >> "$summary"
    if [ "$qpa_transfer" != complete ]; then
        printf 'qpa_missing_bytes=%s\n' "$qpa_missing" >> "$summary"
    fi
    # Sent last, and never withheld because a segment failed: a fraction
    # that lost results says so in its own verdict rather than ending in
    # silence.
    if [ "$channel_ready" -eq 1 ]; then
        emit_artifact summary.txt "$summary" || transfer=1
        exec 3>&-
    fi
    [ "$transfer" -eq 0 ] || echo 'UML_VK_CTS_ERROR: artifact transfer failed'
{wsi_cleanup}    sync
{poweroff}}}
trap finish EXIT HUP INT TERM

install -d -m 0755 -o uml -g uml "$output"
{selected_setup}
: > "$qpa"
chown uml:uml "$qpa"
{skipped_creation}actual_page_size=$(getconf PAGESIZE)
[ "$actual_page_size" -eq {gpu_guest.GUEST_PAGE_SIZE} ] || {{
    echo "UML_VK_CTS_ERROR: guest page size $actual_page_size does not match {gpu_guest.GUEST_PAGE_SIZE}"
    exit 1
}}
printf 'guest_page_size=%s\n' "$actual_page_size" > "$renderer"
printf 'vk_driver_files=%s\n' {shlex.quote(icd_path)} >> "$renderer"
printf 'receipt_sha256=%s\n' {shlex.quote(receipt_digest)} >> "$renderer"
printf 'vulkan_loader=%s\n' \
    {shlex.quote(loader_runtime['path'])} >> "$renderer"
printf 'vulkan_loader_sha256=%s\n' \
    {shlex.quote(loader_runtime['sha256'])} >> "$renderer"
dpkg-query -W -f='${{Package}}=${{Version}}\\n' \
    mesa-vulkan-drivers libvulkan1 libdrm2 >> "$renderer"
cat {shlex.quote(icd_path)} >> "$renderer"

verify_file()
{{
    file="$1"
    expected_size="$2"
    expected_sha="$3"
    actual_size=$(wc -c < "$file" | tr -d ' ')
    [ "$actual_size" -eq "$expected_size" ] || return 1
    actual_sha=$(sha256sum "$file" | cut -d ' ' -f 1)
    [ "$actual_sha" = "$expected_sha" ]
}}

{verification_rows}

{wsi_setup}
{expected_setup}{planned_restart_setup}
: > "$cts_output"
chown uml:uml "$cts_output"
cd {shlex.quote(cts['install_prefix'])}
prepare_channel || {{
    echo 'UML_VK_CTS_ERROR: artifact transfer failed'
    exit 1
}}
emit_prerun || {{
    echo 'UML_VK_CTS_ERROR: artifact transfer failed'
    exit 1
}}

run_list={run_list}
selected_count=$(wc -l < {selected_count_source} | tr -d ' ')
[ "$selected_count" -gt 0 ] || {{
    echo 'UML_VK_CTS_ERROR: the guest selection is empty'
    exit 1
}}
set +e
deadline=$(( $(date +%s) + {timeout} ))
while :; do
    session_timeout=$(( deadline - $(date +%s) ))
    [ "$session_timeout" -gt 0 ] || break
    cts_sessions=$((cts_sessions + 1))
    if {first_session_condition}; then
        runuser -u uml -- env \
            HOME=/home/uml \
{wsi_environment}            VK_DRIVER_FILES={shlex.quote(icd_path)} \
            timeout --signal=TERM --kill-after=30 "$session_timeout" \
            {quoted_command} >> "$cts_output" 2>&1 &
    else
        runuser -u uml -- env \
            HOME=/home/uml \
{wsi_environment}            VK_DRIVER_FILES={shlex.quote(icd_path)} \
            timeout --signal=TERM --kill-after=30 "$session_timeout" \
            {continuation_command} >> "$cts_output" 2>&1 &
    fi
cts_pid=$!
last_qpa_size=0
stalled_seconds=0
cts_process=
while kill -0 "$cts_pid" 2>/dev/null; do
    sleep "$progress_interval"
    current_qpa_size=$(wc -c < "$qpa" | tr -d ' ')
    current_case=$(tail -c 1048576 "$qpa" | \
        sed -n 's/^#beginTestCaseResult //p' | tail -n 1)
    if [ "$current_qpa_size" -ne "$last_qpa_size" ]; then
        last_qpa_size="$current_qpa_size"
        stalled_seconds=0
    else
        stalled_seconds=$((stalled_seconds + progress_interval))
    fi
    find_cts_process
    cts_rss_kb=$(cts_process_field VmRSS)
    cts_peak_kb=$(cts_process_field VmHWM)
    mem_available_kb=$(sed -n \
        's/^MemAvailable:[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p' /proc/meminfo)
    [ -n "$mem_available_kb" ] || mem_available_kb=0
    # The whole growing QPA lives in the guest's own tmpfs, so the room left
    # there is a run-ending limit of its own and nothing outside the guest can
    # see it.
    output_free_kb=$(df -kP "$output" 2>/dev/null | awk 'NR==2 {{print $4}}')
    [ -n "$output_free_kb" ] || output_free_kb=0
    printf 'UML_VK_CTS_PROGRESS|qpa_bytes=%s|stalled_seconds=%s|cts_rss_kb=%s|cts_peak_kb=%s|mem_available_kb=%s|output_free_kb=%s|case=%s\n' \
        "$current_qpa_size" "$stalled_seconds" "$cts_rss_kb" \
        "$cts_peak_kb" "$mem_available_kb" "$output_free_kb" "$current_case"
{relax_watch}{spare_cts_output}    # A checkpoint that fails before it frames anything keeps its bytes
    # pending, so the loop stays up and the next one carries them. One that
    # fails with frames already sent is done for: the host cannot be given
    # that name twice, so nothing further is attempted for it.
    if [ $((current_qpa_size - qpa_offset)) -ge "$checkpoint_bytes" ] && \
       [ "$checkpoint_failures" -lt 3 ]; then
        if flush_qpa 0; then
            checkpoint_failures=0
        else
            checkpoint_failures=$((checkpoint_failures + 1))
            [ "$artifact_framed" -eq 0 ] || checkpoint_failures=3
            echo 'UML_VK_CTS_ERROR: QPA checkpoint transfer failed'
        fi
    fi
    if [ "$progress_timeout" -gt 0 ] && \
       [ "$stalled_seconds" -ge "$progress_timeout" ]; then
        # Never signal a wedged case from inside the guest: a Venus hang can
        # leave it unkillable and take the whole session down with it. The
        # checkpointed prefix is already on the host, which owns recovery.
        progress_timed_out=1
        break
    fi
done
    if [ "$progress_timed_out" -eq 1 ]; then
        session_status=124
    else
        wait "$cts_pid"
        session_status=$?
    fi
    session_cases=$(grep -c '^#endTestCaseResult' "$qpa")
    [ -n "$session_cases" ] || session_cases=0
    cts_cases=$((cts_cases + session_cases))
    # The first non-zero status owns the fraction, so a continuation that runs
    # clean never hides the session that did not.
    if [ "$cts_sessions" -eq 1 ] || [ "$status" -eq 0 ]; then
        status=$session_status
    fi
    [ "$progress_timed_out" -eq 0 ] || break
    [ "$cts_cases" -lt "$selected_count" ] || break
    [ "$cts_sessions" -lt {continuation_limit} ] || break
    # A session that transported no new result cannot be continued past
    # whatever stopped it, so the shard ends rather than looping on it.
    [ "$session_cases" -gt 0 ] || break
    # dEQP ends its whole session at a resource error, so every case after one
    # goes unrun. Send this session's results, release its log, and run the
    # remainder of the same traversal in a fresh process: nothing is re-run,
    # nothing is invented, and the host receives one ordered result stream.
    flush_qpa 0
    qpa_size_now=$(wc -c < "$qpa" | tr -d ' ')
    [ "$qpa_offset" -eq "$qpa_size_now" ] || break
{continuation_selection}    [ -s "$run_list" ] || break
    : > "$qpa"
    chown uml:uml "$qpa"
    qpa_offset=0
{continuation_note}done
set -e
printf 'UML_VK_CTS_STATUS|rc=%s\n' "$status"
exit "$status"
"""


CAPABILITY_REPORT_ENVIRONMENT = ("env", "UML_GPU_CAPABILITY_REPORT=1")


def _guest_command(
    options: argparse.Namespace,
    artifact_directory: Path,
    guest_files: gpu_guest.StagedGuestFiles,
    *,
    rootfs_type: str,
    command: str,
) -> list[str]:
    """The run_gpu_guest.py command that installs ``guest_files`` and runs the
    shell ``command`` in a guest on a ``rootfs_type`` image, with the run's
    console and artifact sockets named after ``artifact_directory``."""
    run_identity = hashlib.sha256(
        os.fsencode(artifact_directory.resolve())
    ).hexdigest()[:20]
    serial_socket_path = (
        Path("/private/tmp") / f"uml-vk-cts-console-{run_identity}.sock"
    )
    return [
        *CAPABILITY_REPORT_ENVIRONMENT,
        sys.executable,
        str(INTEGRATION_GPU / "run_gpu_guest.py"),
        "--emulator",
        str(options.emulator),
        "--rootfs",
        str(options.rootfs),
        "--rootfs-type",
        rootfs_type,
        "--memory",
        options.memory,
        "--unit",
        "multi-user.target",
        "--timeout",
        str(options.guest_timeout),
        "--artifact-root",
        str(artifact_directory / "guest"),
        "--serial-socket",
        str(serial_socket_path),
        f"--virtio-serial-socket={common.artifact_socket_path(artifact_directory)}",
        *guest_files.arguments,
        "--command",
        command,
        "--",
        f"umid=vkcts-{run_identity}",
        "sysrq_always_enabled",
        f"ncpus={options.cpus}",
        f"--execution-engine={options.engine}",
        "--gpu",
        "--gpu-display=vnc",
        "--gpu-vnc-bind=127.0.0.1",
        "--gpu-vnc-port=0",
        "--gpu-width=1280",
        "--gpu-height=720",
    ]


def plan_harness_command(
    options: argparse.Namespace,
    identity: dict[str, Any],
    selection: DevelopmentSelection,
    artifact_directory: Path,
) -> tuple[list[str], gpu_guest.StagedGuestFiles]:
    """The development harness command and the files it installs, unwritten."""
    guest_files = gpu_guest.StagedGuestFiles(
        artifact_directory / gpu_guest.GUEST_FILE_DIRECTORY
    )
    guest_files.add(GUEST_CASE_LIST_PATH, _selected_payload(selection))
    guest_files.add(
        CTS_SERVICE_PATH,
        build_guest_launcher(
            identity,
            selection,
            timeout=options.shard_timeout,
            experimental_venus=options.experimental_venus,
            case_list=GUEST_CASE_LIST_PATH,
        ),
        0o755,
    )
    expected_status = identity["development_no_device_baseline"][
        "process_return_code"
    ]
    run_and_check = (
        f"set +e; {CTS_SERVICE_PATH}; cts_status=$?; set -e; "
        f'[ "$cts_status" -eq {expected_status} ]'
    )
    command = _guest_command(
        options,
        artifact_directory,
        guest_files,
        rootfs_type="erofs",
        command=run_and_check,
    )
    return command, guest_files


def build_harness_command(
    options: argparse.Namespace,
    identity: dict[str, Any],
    selection: DevelopmentSelection,
    artifact_directory: Path,
) -> list[str]:
    """The recorded development harness command; building it writes nothing."""
    return plan_harness_command(options, identity, selection, artifact_directory)[0]


def plan_receipt_harness_command(
    options: argparse.Namespace,
    receipt: dict[str, Any],
    selection: ReceiptSelection,
    artifact_directory: Path,
    *,
    resume_after: int = 0,
) -> tuple[list[str], gpu_guest.StagedGuestFiles]:
    """The qualification harness command and the files it installs, unwritten.

    An explicit case list reaches the guest as its own file, so no count of
    cases lengthens a command line."""
    uses_wsi = receipt_selection_uses_wsi(selection)
    guest_files = gpu_guest.StagedGuestFiles(
        artifact_directory / gpu_guest.GUEST_FILE_DIRECTORY
    )
    case_list = None
    if not selection.generated_expected:
        case_list = GUEST_CASE_LIST_PATH
        guest_files.add(case_list, _selected_payload(selection))
    guest_files.add(
        CTS_SERVICE_PATH,
        build_receipt_guest_launcher(
            receipt,
            selection,
            timeout=options.shard_timeout,
            progress_timeout=options.console_stall_timeout,
            use_wsi=uses_wsi,
            resume_after=resume_after,
            relax_diagnostics=getattr(options, "relax_diagnostics", False),
            case_list=case_list,
        ),
        0o755,
    )
    if uses_wsi:
        guest_files.add(CTS_WSI_SERVICE_PATH, CTS_WSI_SERVICE)
        command = common.native_wsi_harness_command(
            options, artifact_directory, CTS_WSI_SERVICE_NAME,
            guest_files=guest_files,
        )
        return [*CAPABILITY_REPORT_ENVIRONMENT, *command], guest_files
    # The launcher records the CTS status in summary.txt and transfers every
    # artifact from its EXIT trap.  Keep the outer guest harness successful so
    # a failing CTS result reaches the host validator instead of being reduced
    # to an uninspectable guest-command failure.
    command = _guest_command(
        options,
        artifact_directory,
        guest_files,
        rootfs_type="ext4",
        command=f"set +e; {CTS_SERVICE_PATH}; set -e; :",
    )
    return command, guest_files


def build_receipt_harness_command(
    options: argparse.Namespace,
    receipt: dict[str, Any],
    selection: ReceiptSelection,
    artifact_directory: Path,
    *,
    resume_after: int = 0,
) -> list[str]:
    """The recorded qualification harness command; building it writes nothing."""
    return plan_receipt_harness_command(
        options, receipt, selection, artifact_directory, resume_after=resume_after
    )[0]


def prepare_receipt_harness_command(
    options: argparse.Namespace,
    receipt: dict[str, Any],
    selection: ReceiptSelection,
    artifact_directory: Path,
    *,
    resume_after: int = 0,
) -> list[str]:
    """Write the files a qualification harness installs and return its command."""
    command, guest_files = plan_receipt_harness_command(
        options, receipt, selection, artifact_directory, resume_after=resume_after
    )
    guest_files.write()
    return command


def _summary(payload: bytes) -> dict[str, str]:
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise CampaignError("Vulkan CTS summary is not UTF-8") from error
    values: dict[str, str] = {}
    for line in text.splitlines():
        key, separator, value = line.partition("=")
        if not separator or not key or key in values:
            raise CampaignError("Vulkan CTS summary is malformed")
        values[key] = value
    return values


def validate_attempt(
    identity: dict[str, Any],
    selection: DevelopmentSelection,
    transcript: str,
    artifacts: dict[str, bytes],
    *,
    experimental_venus: bool = False,
) -> dict[str, Any]:
    required = {"renderer.txt", "selected.txt", "results.qpa", "summary.txt"}
    if set(artifacts) != required:
        raise CampaignError("Vulkan CTS attempt has an incomplete artifact set")
    selected = artifacts["selected.txt"].decode("utf-8", errors="strict")
    if selected != _selected_payload(selection):
        raise CampaignError("Vulkan CTS selected list drifted")
    summary = _summary(artifacts["summary.txt"])
    baseline = identity["development_no_device_baseline"]
    expected_summary = {
        "cts_rc": str(baseline["process_return_code"]),
        "mode": selection.mode,
        "expected_case_count": str(len(selection.expected_cases)),
        "forced_driver_file": identity["virtio_icd"]["manifest_path"],
    }
    required_transcript = no_device_required_transcript(
        identity, experimental_venus=experimental_venus
    )
    expected_summary.update(
        {
            f"required_transcript_{index}": row
            for index, row in enumerate(required_transcript)
        }
    )
    if summary != expected_summary:
        raise CampaignError("Vulkan CTS summary changed")
    if any(row not in transcript for row in required_transcript):
        raise CampaignError("Vulkan CTS no-device failure signature changed")
    qpa = artifacts["results.qpa"]
    if (
        len(qpa) != baseline["qpa_size"]
        or hashlib.sha256(qpa).hexdigest() != baseline["qpa_sha256"]
    ):
        if not qpa:
            raise CampaignError("Vulkan CTS QPA identity changed")
        text = common.decode_qpa(qpa)
        results = common.validate_qpa_union(text, selection.expected_cases)
        validate_virtio_qpa_identity(text)
        validate_released_results(results, identity["released_status_oracle"])
        raise CampaignError(
            "a Vulkan device unexpectedly became visible before derivative authorization"
        )
    renderer = artifacts["renderer.txt"].decode("utf-8", errors="strict")
    if f"vk_driver_files={identity['virtio_icd']['manifest_path']}" not in renderer:
        raise CampaignError("renderer evidence does not force the Virtio ICD")
    if FORBIDDEN_SOFTWARE_ICD.search(renderer):
        raise CampaignError("renderer evidence selected a software ICD")
    return {
        "outcome": "expected-no-device",
        "cts_return_code": baseline["process_return_code"],
        "qpa_sha256": baseline["qpa_sha256"],
        "selected_case_count": len(selection.expected_cases),
        "qualification": False,
    }


def validate_receipt_attempt(
    receipt: dict[str, Any],
    selection: ReceiptSelection,
    transcript: str,
    artifacts: dict[str, bytes],
    *,
    session_count: int = 1,
    device_exclusions: Sequence[DeviceExclusion] = (),
    headless_wsi: bool = False,
) -> dict[str, Any]:
    if session_count < 1:
        raise CampaignError("a shard needs at least one guest session")
    exclusions = validate_device_exclusions(device_exclusions)
    if exclusions and not selection.generated_expected:
        raise CampaignError(
            "device exclusions require an enumerated released fraction"
        )
    device_profile: AppleMustpassProfile | None = None
    if exclusions:
        if selection.fraction.count != APPLE_MUSTPASS_FRACTIONS:
            raise CampaignError(
                "a baked device profile requires the native 16-way fraction"
            )
        device_profile = apple_mustpass_profile(selection.fraction.index)
    required = {"renderer.txt", "selected.txt", "results.qpa", "summary.txt"}
    if selection.generated_expected:
        required.add("expected.txt")
    if exclusions:
        required.add("skipped.txt")
    if set(artifacts) != required:
        raise CampaignError("receipt-backed Vulkan CTS artifact set is incomplete")
    selected_payload = artifacts["selected.txt"]
    if selection.generated_expected:
        enumerated_cases = _parse_case_artifact(
            artifacts["expected.txt"], "receipt-backed enumerated fraction"
        )
        if exclusions:
            expected_cases, skipped = device_exclusion_partition(
                enumerated_cases, exclusions
            )
            expected_selected = "".join(
                f"{case}\n" for case in expected_cases
            ).encode()
            if selected_payload != expected_selected:
                raise CampaignError(
                    "device CTS selection does not match its exclusions"
                )
            expected_skipped = "".join(
                f"{reason}\t{case}\n"
                for reason, cases in skipped.items()
                for case in cases
            ).encode()
            if artifacts["skipped.txt"] != expected_skipped:
                raise CampaignError("device CTS exclusion report drifted")
        else:
            selected_contract = receipt["mustpass"]["top_level"]
            if (
                len(selected_payload) != selected_contract["size"]
                or hashlib.sha256(selected_payload).hexdigest()
                != selected_contract["sha256"]
            ):
                raise CampaignError(
                    "receipt-backed top-level CTS selection drifted"
                )
            expected_cases = enumerated_cases
    else:
        selected = selected_payload.decode("utf-8", errors="strict")
        expected_selected = "".join(
            f"{case}\n" for case in selection.source_cases
        )
        if selected != expected_selected:
            raise CampaignError("receipt-backed CTS selection drifted")
        expected_cases = selection.expected_cases
    receipt_digest = common.sha256_file(RECEIPT_PATH)
    summary = _summary(artifacts["summary.txt"])
    expected_summary = {
        "mode": selection.mode,
        "expected_case_count": str(len(expected_cases)),
        "forced_driver_file": "/usr/share/vulkan/icd.d/virtio_icd.json",
        "receipt_sha256": receipt_digest,
        "formal_conformance": "false",
    }
    if exclusions:
        expected_summary["device_excluded"] = str(
            sum(len(cases) for cases in skipped.values())
        )
        for exclusion in exclusions:
            cases = skipped[exclusion.reason]
            expected_summary[f"device_excluded_{exclusion.row}"] = str(
                len(cases)
            )
            expected_summary[f"device_exclusion_{exclusion.row}"] = " ".join(
                exclusion.patterns
            )
        expected_summary["device_memory_mib"] = str(
            device_profile.guest_memory_mib
        )
    else:
        # The guest states whether every QPA byte it produced reached the host.
        # Anything but a complete transfer means results were lost, which no
        # attempt may be accepted with, however the cases themselves went.
        expected_summary["qpa_transfer"] = "complete"
    if selection.generated_expected:
        expected_summary["fraction"] = selection.fraction.argument()
    if session_count > 1:
        # A resumed shard re-ran the cases its dead guests never transported,
        # so the merge is stated in the summary but no case is host-recorded.
        expected_summary["provenance"] = "host-merged"
        expected_summary["sessions"] = str(session_count)
    cts_rc = summary.pop("cts_rc", None)
    # dEQP ends its whole session at a resource error, so a guest continues
    # the remainder of the same ordered caselist in a fresh CTS process. How
    # many processes that took, and how many cases they produced, is evidence
    # about this guest rather than a fixed expectation: the union against the
    # enumerated fraction is what decides completeness, and a shard the host
    # resumed carries only its last guest's counters here.
    cts_sessions = summary.pop("cts_sessions", None)
    cts_cases_run = summary.pop("cts_cases_run", None)
    # Device campaigns recorded before these diagnostic counters were added are
    # still complete when their QPA proves the exact selected union. The v4
    # profile requires counters and at least two sessions for its planned
    # restart. A single device launch must account for every selected case.
    minimum_sessions = (
        2 if device_profile is not None and device_profile.restart_before else 1
    )
    if (cts_sessions is None) != (cts_cases_run is None):
        raise CampaignError("receipt-backed CTS guest counters are incomplete")
    if cts_sessions is None:
        if not exclusions or minimum_sessions > 1:
            raise CampaignError(
                "receipt-backed CTS states no guest session count"
            )
    else:
        if not cts_sessions.isdigit() or not (
            minimum_sessions <= int(cts_sessions) <= VULKAN_CTS_SESSION_LIMIT
        ):
            raise CampaignError(
                "receipt-backed CTS states no guest session count"
            )
        if not cts_cases_run.isdigit() or not (
            0 < int(cts_cases_run) <= len(expected_cases)
        ):
            raise CampaignError("receipt-backed CTS states no guest case count")
        if exclusions and int(cts_cases_run) != len(expected_cases):
            raise CampaignError(
                "device CTS guest case count is not the complete selection"
            )
    if exclusions:
        transfer = summary.pop("qpa_transfer", None)
        missing = summary.pop("qpa_missing_bytes", None)
        if transfer == "complete":
            if missing is not None:
                raise CampaignError(
                    "complete device QPA transfer reports missing bytes"
                )
        elif transfer == "recovered":
            if missing != "0":
                raise CampaignError(
                    "recovered device QPA transfer still misses bytes"
                )
        else:
            raise CampaignError("device QPA transfer is incomplete")
    allowed_return_codes = (
        {"0", "1"} if selection.generated_expected else {"0"}
    )
    if summary != expected_summary or cts_rc not in allowed_return_codes:
        raise CampaignError("receipt-backed CTS summary changed")
    emitted_error = "UML_VK_CTS_ERROR:"
    if any(
        line.strip().startswith(emitted_error) or
        f": {emitted_error}" in line
        for line in transcript.splitlines()
    ):
        raise CampaignError(
            f"receipt-backed CTS transcript contains {emitted_error}"
        )
    if not selection.generated_expected:
        for failure in (
            "VK_ERROR_INCOMPATIBLE_DRIVER",
            "VK_ERROR_INITIALIZATION_FAILED",
            "VK_ERROR_DEVICE_LOST",
        ):
            if failure in transcript:
                raise CampaignError(f"receipt-backed CTS transcript contains {failure}")
    qpa_payload = artifacts["results.qpa"]
    if not qpa_payload:
        raise CampaignError("receipt-backed CTS produced an empty QPA")
    qpa_text = common.decode_qpa(qpa_payload)
    results = common.validate_qpa_union(qpa_text, expected_cases)
    validate_virtio_qpa_identity(qpa_text)
    rejected = classify_released_results(results, receipt["runner_oracle"])
    if not selection.generated_expected:
        validate_released_results(results, receipt["runner_oracle"])
    if (bool(rejected) and cts_rc != "1") or (not rejected and cts_rc != "0"):
        raise CampaignError("CTS exit status disagrees with its complete QPA")
    renderer = artifacts["renderer.txt"].decode("utf-8", errors="strict")
    for expected in (
        "vk_driver_files=/usr/share/vulkan/icd.d/virtio_icd.json",
        f"receipt_sha256={receipt_digest}",
        f"vulkan_loader={receipt['vulkan_loader']['runtime']['path']}",
        f"vulkan_loader_sha256={receipt['vulkan_loader']['runtime']['sha256']}",
        '"library_path": "libvulkan_virtio.so"',
    ):
        if expected not in renderer:
            raise CampaignError(
                f"receipt-backed renderer evidence omits {expected}"
            )
    if '"is_portability_driver"' in renderer:
        raise CampaignError(
            "receipt-backed renderer evidence selected a portability ICD"
        )
    if FORBIDDEN_SOFTWARE_ICD.search(renderer):
        raise CampaignError("receipt-backed renderer evidence selected a software ICD")
    if headless_wsi:
        validate_headless_wsi_results(results)
    else:
        validate_wsi_operation(selection, results, renderer)
    if not selection.generated_expected:
        return {
            "outcome": "receipt-backed-cts",
            "cts_return_code": 0,
            "qpa_sha256": hashlib.sha256(qpa_payload).hexdigest(),
            "selected_case_count": len(expected_cases),
            "qualification": True,
            "formal_conformance": False,
        }
    status_counts: dict[str, int] = {}
    for status in results.values():
        status_counts[status] = status_counts.get(status, 0) + 1
    not_supported = tuple(
        (case, status)
        for case, status in results.items()
        if status == "NotSupported"
    )
    return {
        "outcome": (
            "receipt-backed-cts-failures" if rejected else "receipt-backed-cts"
        ),
        "cts_return_code": int(cts_rc),
        "qpa_sha256": hashlib.sha256(qpa_payload).hexdigest(),
        "selected_case_count": len(expected_cases),
        "status_counts": dict(sorted(status_counts.items())),
        "rejected_status_count": len(rejected),
        "rejected_results_sha256": _status_rows_digest(rejected),
        "not_supported_count": len(not_supported),
        "not_supported_results_sha256": _status_rows_digest(not_supported),
        "first_rejected_result": (
            {"case": rejected[0][0], "status": rejected[0][1]}
            if rejected
            else None
        ),
        "qualification": not rejected,
        "formal_conformance": False,
    }


def verify_staged_kosmickrisp(emulator: Path) -> str:
    return common.verify_staged_kosmickrisp(emulator)


def verify_only(path: Path, identity: dict[str, Any]) -> dict[str, Any]:
    metadata = load_json(path / "run.json")
    schema_version = metadata.get("schema_version")
    if schema_version == 2 or (
        schema_version == 3 and metadata.get("qualification") is True
    ):
        receipt = receipt_contract.validate_receipt(RECEIPT_PATH)
        return verify_receipt_only(path, metadata, receipt)
    if schema_version not in (1, 3) or metadata.get("qualification") is not False:
        raise CampaignError("Vulkan CTS run metadata is not development-only")
    experimental_venus = metadata.get("experimental_venus", False)
    if type(experimental_venus) is not bool:
        raise CampaignError("Vulkan CTS run has an invalid Venus state")
    if schema_version == 3:
        provider_sha256 = metadata.get("kosmickrisp_sha256")
        if experimental_venus:
            _sha256(provider_sha256, "KosmicKrisp provider identity")
        elif provider_sha256 is not None:
            raise CampaignError(
                "non-Venus Vulkan CTS run records a KosmicKrisp provider"
            )
    selection_data = metadata.get("selection")
    if not isinstance(selection_data, dict):
        raise CampaignError("Vulkan CTS run omits its frozen selection")
    fraction_value = selection_data.get("fraction")
    if (
        not isinstance(fraction_value, list)
        or len(fraction_value) != 2
        or any(type(item) is not int for item in fraction_value)
    ):
        raise CampaignError("Vulkan CTS run has an invalid fraction")
    selection = resolve_selection(
        identity,
        selection_data.get("mode"),
        selection_data.get("exact_cases", []),
        selection_data.get("family"),
        Fraction(*fraction_value),
    )
    artifact_dir = path / "artifacts"
    artifacts: dict[str, bytes] = {}
    digests = metadata.get("artifact_sha256")
    if not isinstance(digests, dict):
        raise CampaignError("Vulkan CTS run omits artifact digests")
    for name in ("renderer.txt", "selected.txt", "results.qpa", "summary.txt"):
        artifact = artifact_dir / name
        try:
            payload = artifact.read_bytes()
        except OSError as error:
            raise CampaignError(f"cannot read Vulkan CTS artifact {name}: {error}") from error
        if digests.get(name) != hashlib.sha256(payload).hexdigest():
            raise CampaignError(f"Vulkan CTS artifact digest changed: {name}")
        artifacts[name] = payload
    console = path / "console.log"
    if metadata.get("console_sha256") != common.sha256_file(console):
        raise CampaignError("Vulkan CTS console digest changed")
    result = validate_attempt(
        identity,
        selection,
        console.read_text(encoding="utf-8", errors="replace"),
        artifacts,
        experimental_venus=experimental_venus,
    )
    if metadata.get("result") != result:
        raise CampaignError("Vulkan CTS verified result drifted")
    return result


def verify_receipt_only(
    path: Path,
    metadata: dict[str, Any],
    receipt: dict[str, Any],
) -> dict[str, Any]:
    if (
        metadata.get("schema_version") not in (2, 3)
        or metadata.get("qualification") is not True
        or metadata.get("formal_conformance") is not False
        or metadata.get("experimental_venus") is not True
        or metadata.get("receipt_sha256") != common.sha256_file(RECEIPT_PATH)
        or metadata.get("rootfs_sha256") != receipt["derivative"]["sha256"]
    ):
        raise CampaignError("receipt-backed Vulkan CTS metadata changed")
    if metadata["schema_version"] == 3:
        _sha256(
            metadata.get("kosmickrisp_sha256"),
            "KosmicKrisp provider identity",
        )
        # Older schema-3 receipts predate emulator binding. Preserve their
        # historical verification, but validate every newly recorded identity;
        # the graphics coordinator requires an exact candidate hash match.
        if "emulator_sha256" in metadata:
            _sha256(metadata["emulator_sha256"], "emulator identity")
    selection_data = metadata.get("selection")
    if not isinstance(selection_data, dict):
        raise CampaignError("receipt-backed run omits its frozen selection")
    fraction_value = selection_data.get("fraction")
    if (
        not isinstance(fraction_value, list)
        or len(fraction_value) != 2
        or any(type(item) is not int for item in fraction_value)
    ):
        raise CampaignError("receipt-backed run has an invalid fraction")
    selection = resolve_receipt_selection(
        selection_data.get("mode"),
        selection_data.get("exact_cases", []),
        selection_data.get("family"),
        Fraction(*fraction_value),
    )
    if selection_data.get("expected_cases") != list(selection.expected_cases):
        raise CampaignError("receipt-backed expected-case list changed")
    digests = metadata.get("artifact_sha256")
    if not isinstance(digests, dict):
        raise CampaignError("receipt-backed run omits artifact digests")
    artifact_dir = path / "artifacts"
    artifacts: dict[str, bytes] = {}
    artifact_names = ["renderer.txt", "selected.txt", "results.qpa", "summary.txt"]
    if selection.generated_expected:
        artifact_names.append("expected.txt")
    for name in artifact_names:
        try:
            payload = (artifact_dir / name).read_bytes()
        except OSError as error:
            raise CampaignError(f"cannot read receipt-backed artifact {name}: {error}") from error
        if digests.get(name) != hashlib.sha256(payload).hexdigest():
            raise CampaignError(f"receipt-backed artifact digest changed: {name}")
        artifacts[name] = payload
    console = path / "console.log"
    if metadata.get("console_sha256") != common.sha256_file(console):
        raise CampaignError("receipt-backed console digest changed")
    sessions = metadata.get("sessions")
    transcript = console.read_text(encoding="utf-8", errors="replace")
    session_count = 1
    if sessions is not None:
        expected_cases = (
            common.parse_case_list(
                artifacts["expected.txt"].decode("utf-8", errors="strict")
            )
            if selection.generated_expected
            else list(selection.expected_cases)
        )
        common.validate_session_provenance(
            path, metadata, artifacts, len(expected_cases)
        )
        session_count = len(sessions)
        transcript = "".join(
            (path / str(row["console"])).read_text(
                encoding="utf-8", errors="replace"
            )
            for row in sessions
        )
    result = validate_receipt_attempt(
        receipt,
        selection,
        transcript,
        artifacts,
        session_count=session_count,
    )
    if metadata.get("result") != result:
        raise CampaignError("receipt-backed verified result drifted")
    return result


def run_receipt_campaign(
    options: argparse.Namespace,
    receipt: dict[str, Any],
    selection: ReceiptSelection,
) -> int:
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifact_directory = options.artifact_root.resolve() / (
        f"{timestamp}-{os.getpid()}-4k-receipt-{selection.mode}"
    )
    command = build_receipt_harness_command(
        options, receipt, selection, artifact_directory
    )
    if options.dry_run:
        print(shlex.join(command))
        return 0
    emulator = options.emulator.resolve()
    rootfs = options.rootfs.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        raise CampaignError(f"emulator is not executable: {emulator}")
    provider_sha256 = verify_staged_kosmickrisp(emulator)
    try:
        receipt_contract.verify_image(rootfs, receipt)
    except receipt_contract.ReceiptError as error:
        raise CampaignError(str(error)) from error

    artifact_directory.mkdir(parents=True, exist_ok=False)
    metadata: dict[str, Any] = {
        "schema_version": 3,
        "qualification": True,
        "formal_conformance": False,
        "experimental_venus": True,
        "emulator_sha256": common.sha256_file(emulator),
        "kosmickrisp_sha256": provider_sha256,
        "receipt_sha256": common.sha256_file(RECEIPT_PATH),
        "git": common.git_state(),
        "page_profile": "4k",
        "engine": options.engine,
        "memory": options.memory,
        "cpus": options.cpus,
        "shard_timeout": options.shard_timeout,
        "guest_timeout": options.guest_timeout,
        "console_stall_timeout": options.console_stall_timeout,
        "rootfs_sha256": receipt["derivative"]["sha256"],
        "cts_sha256": receipt["cts"]["executable"]["sha256"],
        "virtio_icd_sha256": _receipt_file(
            receipt, "/usr/share/vulkan/icd.d/virtio_icd.json"
        )["sha256"],
        "selection": {
            "mode": selection.mode,
            "exact_cases": list(options.case),
            "family": options.family,
            "fraction": [selection.fraction.index, selection.fraction.count],
            "expected_cases": list(selection.expected_cases),
        },
        "command": command,
        "state": "started",
    }
    common.write_json(artifact_directory / "run.json", metadata)
    console_path = artifact_directory / "console.log"
    serial_path = artifact_directory / common.ARTIFACT_TRANSPORT_LOG
    first: dict[str, bytes] = {}

    def resolve_expected(found: dict[str, bytes]) -> tuple[str, ...]:
        first.update(found)
        if not selection.generated_expected:
            return selection.expected_cases
        if "expected.txt" not in found:
            raise CampaignError(
                "the first Vulkan session transported no enumerated fraction"
            )
        return tuple(
            common.parse_case_list(
                found["expected.txt"].decode("utf-8", errors="strict")
            )
        )

    def check_session(index: int, resume_after: int, found: dict[str, bytes]) -> None:
        if index == 0:
            return
        # A continuation is only meaningful against the same ordered list, so
        # the resumed guest must re-derive an identical enumeration.
        if selection.generated_expected and found.get("expected.txt") != first.get(
            "expected.txt"
        ):
            raise CampaignError(
                "resumed Vulkan session enumerated a different fraction"
            )
        source = (
            first.get("expected.txt")
            if selection.generated_expected
            else first.get("selected.txt")
        )
        if source is None:
            raise CampaignError("resumed Vulkan session has no ordered source list")
        remainder = "".join(
            f"{case}\n"
            for case in common.parse_case_list(
                source.decode("utf-8", errors="strict")
            )[resume_after:]
        ).encode("utf-8")
        if found.get("selected.txt") not in (None, remainder):
            raise CampaignError(
                "resumed Vulkan session did not select the remaining cases"
            )

    def record_sessions(rows: tuple[common.SessionEvidence, ...]) -> None:
        metadata.update(
            {
                "session_count": len(rows),
                "max_resumptions": options.max_resumptions,
                "artifacts_provenance": common.shard_provenance(rows),
                "sessions": common.session_records(rows, artifact_directory),
            }
        )
        common.write_json(artifact_directory / "run.json", metadata)

    sessions, expected_cases, merged = common.run_shard_sessions(
        artifact_directory,
        build_command=lambda resume, guest: prepare_receipt_harness_command(
            options, receipt, selection, guest, resume_after=resume
        ),
        resolve_expected=resolve_expected,
        check_session=check_session,
        guest_timeout=options.guest_timeout,
        console_stall_timeout=options.console_stall_timeout,
        max_resumptions=options.max_resumptions,
        on_sessions=record_sessions,
    )
    final = sessions[-1]
    transcript = "".join(
        (artifact_directory / session.console).read_text(
            encoding="utf-8", errors="replace"
        )
        for session in sessions
    )
    common.validate_host_backend(transcript)
    final_transcript = (artifact_directory / final.console).read_text(
        encoding="utf-8", errors="replace"
    )
    if final.shutdown_stall:
        # The guest transported every acknowledged frame and then failed to
        # power off inside its shutdown grace. Only the memory-manager
        # contract still applies to a reaped guest's transcript.
        common.validate_drm_shutdown(final_transcript)
    else:
        common.validate_guest_shutdown(final_transcript)
    artifact_path = artifact_directory / "artifacts"
    if common.shard_provenance(sessions) == "single-session-guest":
        artifacts = {
            item.name: item.read_bytes()
            for item in sorted(
                (artifact_directory / final.artifacts_directory).iterdir()
            )
        }
    else:
        artifacts = common.merged_shard_artifacts(
            sessions, artifact_directory, len(expected_cases)
        )
        common.write_artifacts(artifacts, artifact_path)
    if list(merged) != list(expected_cases):
        raise CampaignError("merged Vulkan shard results left the released order")
    record_sessions(sessions)
    immutable_evidence = {
        "console_sha256": common.sha256_file(console_path),
        "serial_sha256": common.sha256_file(serial_path),
        "artifact_sha256": {
            name: hashlib.sha256(payload).hexdigest()
            for name, payload in sorted(artifacts.items())
        },
    }
    try:
        result = validate_receipt_attempt(
            receipt,
            selection,
            transcript,
            artifacts,
            session_count=len(sessions),
        )
    except CampaignError as error:
        metadata.update(
            {
                "state": "failed",
                "validation_error": str(error),
                **immutable_evidence,
            }
        )
        common.write_json(artifact_directory / "run.json", metadata)
        raise
    metadata.update(
        {
            "state": (
                "completed-with-failures"
                if result["outcome"] == "receipt-backed-cts-failures"
                else "verified"
            ),
            **immutable_evidence,
            "result": result,
        }
    )
    common.write_json(artifact_directory / "run.json", metadata)
    print(f"Receipt-backed Vulkan CTS attempt preserved at {artifact_directory}")
    return 1 if result["outcome"] == "receipt-backed-cts-failures" else 0


HELP_EPILOG = """\
--qualification binds a run to vulkan_cts_receipt.json: the sealed image's
receipt, the CTS and Mesa source locks, and the staged KosmicKrisp provider
digest, which must match the configured verified prefix so a run cannot
silently use an older provider. --mode exact-family runs named --case values
or one --family, never fractioned or repeated, and validates the results in
released traversal order. current-must-pass runs a fraction of the released
list. Accepted NotSupported and warning results stay distinct from Pass.

The summary states qpa_transfer, with qpa_missing_bytes unless it is complete.
Only complete is accepted, and on a baked device fraction also a transfer
recovered with no missing bytes: a run that lost results says so rather than
reporting a short one. dEQP ends its whole session at the first
ResourceError, so the guest runs the rest of the order it enumerated in a
fresh CTS process, at most eight processes in all within the shard deadline.
Nothing is re-run or invented: summary.txt states cts_sessions and
cts_cases_run, and the union of the sessions must be the enumerated
selection, in order, each case once.

--relax-diagnostics changes the generated guest script, so a campaign enables
it only for a new generation. QPA transport, stall handling and resumption
after an emulator exit are shared with run_vk_gl_cts.py; see its --help.
"""


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="run a frozen Vulkan CTS development or receipt-backed lane",
        epilog=HELP_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--relax-diagnostics",
        action="store_true",
        help="report Venus stuck-wait lines from the CTS process while a shard "
             "is stalled; changes the generated guest script, so a campaign "
             "may only enable it for a new generation",
    )
    parser.add_argument(
        "--mode",
        choices=(
            "smoke",
            "exact-family",
            "frozen-regression",
            "current-must-pass",
            "capability-inventory",
        ),
        default="smoke",
    )
    parser.add_argument("--case", action="append", type=exact_case, default=[])
    parser.add_argument("--family", type=exact_case)
    parser.add_argument("--fraction", type=parse_fraction, default=Fraction(0, 1))
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument("--shard-timeout", type=int, default=900)
    parser.add_argument("--guest-timeout", type=int, default=1500)
    parser.add_argument(
        "--console-stall-timeout",
        type=int,
        default=0,
        help=(
            "stop a guest whose console and artifact transport have made no "
            "progress for this many seconds; zero disables the progress guard"
        ),
    )
    parser.add_argument("--emulator", type=Path, default=DEFAULT_EMULATOR)
    parser.add_argument("--rootfs", type=Path)
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--verify-only", type=Path)
    parser.add_argument(
        "--qualification",
        action="store_true",
        help=(
            "use the exact receipt-backed derivative for project CTS "
            "qualification; this never claims formal Khronos conformance"
        ),
    )
    parser.add_argument("--experimental-venus", action="store_true")
    parser.add_argument(
        "--max-resumptions",
        type=int,
        default=common.DEFAULT_MAX_RESUMPTIONS,
        help=(
            "fresh guests a stalled or aborted shard may continue in "
            f"(default: {common.DEFAULT_MAX_RESUMPTIONS}; 0 keeps one session)"
        ),
    )
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args(arguments)
    if (
        options.cpus < 1
        or options.shard_timeout < 1
        or options.guest_timeout < 1
        or options.console_stall_timeout < 0
    ):
        parser.error("CPU and timeout values must be positive")
    if options.max_resumptions < 0:
        parser.error("--max-resumptions must not be negative")
    if options.verify_only is not None and (
        options.mode != "smoke"
        or options.case
        or options.family is not None
        or options.fraction != Fraction(0, 1)
        or options.experimental_venus
        or options.qualification
        or options.dry_run
    ):
        parser.error("--verify-only accepts only its recorded run directory")
    if options.rootfs is None:
        options.rootfs = (
            DEFAULT_RECEIPT_ROOTFS if options.qualification else DEFAULT_ROOTFS
        )
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    identity = load_identity()
    load_campaigns()
    if options.verify_only is not None:
        result = verify_only(options.verify_only.resolve(), identity)
        print(
            "Verified Vulkan CTS attempt: "
            f"{result['outcome']} ({result['selected_case_count']} selected cases)"
        )
        return 0
    if options.qualification:
        try:
            receipt = receipt_contract.validate_receipt(RECEIPT_PATH)
            selection = resolve_receipt_selection(
                options.mode,
                options.case,
                options.family,
                options.fraction,
            )
        except (CampaignError, receipt_contract.ReceiptError) as error:
            raise SystemExit(str(error)) from error
        return run_receipt_campaign(options, receipt, selection)
    try:
        selection = resolve_selection(
            identity,
            options.mode,
            options.case,
            options.family,
            options.fraction,
        )
    except CampaignError as error:
        raise SystemExit(str(error)) from error
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifact_directory = options.artifact_root.resolve() / (
        f"{timestamp}-{os.getpid()}-4k-{selection.mode}"
    )
    command, guest_files = plan_harness_command(
        options, identity, selection, artifact_directory
    )
    if options.dry_run:
        print(shlex.join(command))
        return 0
    emulator = options.emulator.resolve()
    rootfs = options.rootfs.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        raise CampaignError(f"emulator is not executable: {emulator}")
    provider_sha256 = (
        verify_staged_kosmickrisp(emulator)
        if options.experimental_venus
        else None
    )
    if not rootfs.is_file() or rootfs.stat().st_size != identity["rootfs"]["size"]:
        raise CampaignError("rootfs size does not match the Vulkan CTS identity")
    subprocess.run(
        [
            sys.executable,
            str(REPO_ROOT / "tools/rootfs/verify.py"),
            "--image",
            str(rootfs),
            "--manifest",
            str(REPO_ROOT / "tools/rootfs/manifest.json"),
            "--quiet",
        ],
        check=True,
    )
    artifact_directory.mkdir(parents=True, exist_ok=False)
    metadata: dict[str, Any] = {
        "schema_version": 3,
        "qualification": False,
        "experimental_venus": options.experimental_venus,
        "git": common.git_state(),
        "page_profile": "4k",
        "engine": options.engine,
        "memory": options.memory,
        "cpus": options.cpus,
        "shard_timeout": options.shard_timeout,
        "guest_timeout": options.guest_timeout,
        "console_stall_timeout": options.console_stall_timeout,
        "rootfs_sha256": identity["rootfs"]["sha256"],
        "cts_sha256": identity["cts"]["binary"]["sha256"],
        "virtio_icd_sha256": identity["virtio_icd"]["manifest_sha256"],
        "selection": {
            "mode": selection.mode,
            "exact_cases": list(options.case),
            "family": options.family,
            "fraction": [selection.fraction.index, selection.fraction.count],
            "expected_cases": list(selection.expected_cases),
        },
        "command": command,
        "state": "started",
    }
    if provider_sha256 is not None:
        metadata["kosmickrisp_sha256"] = provider_sha256
    common.write_json(artifact_directory / "run.json", metadata)
    console_path = artifact_directory / "console.log"
    serial_path = artifact_directory / common.ARTIFACT_TRANSPORT_LOG
    socket_path = common.artifact_socket_path(artifact_directory)
    guest_files.write()
    with console_path.open("wb") as console:
        with common.guest_process.managed_child(
            command, stop=common.stop_guest, stdout=console, stderr=subprocess.STDOUT,
        ) as process:
            transcript, serial = common.wait_for_guest(
                process,
                console_path,
                options.guest_timeout,
                socket_path,
                console_stall_timeout=options.console_stall_timeout,
            )
    if process.returncode != 0:
        raise CampaignError(f"guest harness exited with status {process.returncode}")
    if os.path.lexists(socket_path):
        raise CampaignError("emulator did not remove the Vulkan artifact socket")
    common.validate_host_backend(transcript)
    common.validate_guest_shutdown(transcript)
    serial_path.write_text(serial, encoding="ascii")
    artifacts = common.decode_artifacts(serial)
    artifact_path = artifact_directory / "artifacts"
    common.write_artifacts(artifacts, artifact_path)
    result = validate_attempt(
        identity,
        selection,
        transcript,
        artifacts,
        experimental_venus=options.experimental_venus,
    )
    metadata.update(
        {
            "state": "verified",
            "console_sha256": common.sha256_file(console_path),
            "serial_sha256": common.sha256_file(serial_path),
            "artifact_sha256": {
                name: hashlib.sha256(payload).hexdigest()
                for name, payload in sorted(artifacts.items())
            },
            "result": result,
        }
    )
    common.write_json(artifact_directory / "run.json", metadata)
    print(f"Development Vulkan CTS baseline preserved at {artifact_directory}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(common.report_campaign_leaf_exit(main()))
    except (CampaignError, TimeoutError, subprocess.CalledProcessError) as error:
        print(f"Vulkan CTS campaign failed: {error}", file=sys.stderr)
        raise SystemExit(common.report_campaign_leaf_exit(1)) from error
