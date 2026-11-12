#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import base64
import contextlib
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


INTEGRATION_GPU = Path(__file__).resolve().parents[2] / "integration/gpu"
sys.path.insert(0, str(INTEGRATION_GPU))
import run_vk_cts as cts  # noqa: E402
import run_ios_device_vk_cts as device  # noqa: E402
import receipt_fixtures  # noqa: E402


# The guest refuses any page size but 4 KiB and records the one it saw.
PAGE_SIZE_CHECK = (
    'actual_page_size=$(getconf PAGESIZE)\n'
    '[ "$actual_page_size" -eq 4096 ] || {\n'
    '    echo "UML_VK_CTS_ERROR: guest page size $actual_page_size does not '
    'match 4096"\n'
    '    exit 1\n'
    '}\n'
    "printf 'guest_page_size=%s\n' \"$actual_page_size\" > \"$renderer\"\n"
)


def setUpModule() -> None:
    receipt_fixtures.bind_vulkan_receipt(cts)


def qpa_result(case: str, status: str = "Pass", identity: str = "") -> str:
    return f"""#beginTestCaseResult {case}
<?xml version="1.0" encoding="UTF-8"?>
<TestCaseResult Version="0.3.4" CasePath="{case}">
 <Text>{identity}</Text>
 <Result StatusCode="{status}">result</Result>
</TestCaseResult>
#endTestCaseResult
"""


def qpa(*results: str) -> str:
    return "#sessionInfo releaseName 1.4.5.5\n#beginSession\n" + "".join(
        results
    ) + "#endSession\n"


def framed(name: str, payload: bytes) -> str:
    compressed = gzip.compress(payload, mtime=0)
    digest = hashlib.sha256(compressed).hexdigest()
    encoded = base64.b64encode(compressed).decode("ascii")
    return "\n".join(
        (
            f"UML_CTS_ARTIFACT_BEGIN|{name}|gzip+base64|{len(compressed)}|{digest}",
            f"UML_CTS_ARTIFACT_CHUNK|{name}|0|{encoded}",
            f"UML_CTS_ARTIFACT_END|{name}|1",
        )
    ) + "\n"


def baseline_artifacts(
    identity: dict[str, object],
    selection: cts.DevelopmentSelection,
    *,
    experimental_venus: bool = False,
) -> dict[str, bytes]:
    baseline = identity["development_no_device_baseline"]
    icd = identity["virtio_icd"]
    required = cts.no_device_required_transcript(
        identity, experimental_venus=experimental_venus
    )
    required_rows = "".join(
        f"required_transcript_{index}={row}\n"
        for index, row in enumerate(required)
    )
    summary = (
        f"cts_rc={baseline['process_return_code']}\n"
        f"mode={selection.mode}\n"
        f"expected_case_count={len(selection.expected_cases)}\n"
        f"forced_driver_file={icd['manifest_path']}\n"
        f"{required_rows}"
    ).encode()
    return {
        "renderer.txt": (
            f"guest_page_size=4096\n"
            f"vk_driver_files={icd['manifest_path']}\n"
            "mesa-vulkan-drivers=25.0.7-2+deb13u1\n"
        ).encode(),
        "selected.txt": cts._selected_payload(selection).encode(),
        "results.qpa": b"",
        "summary.txt": summary,
    }


# The display stack every receipt-backed WSI fraction reports first.
WSI_RENDERER = (
    "x11_server=xorg\n"
    "x11_dri3=1\n"
    "x11_present=1\n"
    "x11_glamor=1\n"
    "x11_connected=1\n"
    "x11_drm_fd=1\n"
    "x11_blanking=off\n"
    "wayland_server=kwin-x11\n"
    "wayland_lockscreen=off\n"
    "device=/dev/dri/card0\n"
    "device=/dev/dri/renderD128\n"
)


def receipt_renderer(receipt: dict[str, object]) -> bytes:
    """The renderer report of a receipt-backed fraction run from ``receipt``."""
    loader = receipt["vulkan_loader"]["runtime"]
    return (
        WSI_RENDERER
        + "vk_driver_files=/usr/share/vulkan/icd.d/virtio_icd.json\n"
        + f"receipt_sha256={cts.common.sha256_file(cts.RECEIPT_PATH)}\n"
        + f"vulkan_loader={loader['path']}\n"
        + f"vulkan_loader_sha256={loader['sha256']}\n"
        + '"library_path": "libvulkan_virtio.so"\n'
    ).encode()


def write_run(
    root: Path,
    selection: cts.DevelopmentSelection,
    artifacts: dict[str, bytes],
    transcript: str,
    result: dict[str, object],
    **fields: object,
) -> dict[str, object]:
    """Lay out a schema-1 smoke run the way verify_only() reads it back."""
    artifact_root = root / "artifacts"
    artifact_root.mkdir()
    for name, payload in artifacts.items():
        (artifact_root / name).write_bytes(payload)
    (root / "console.log").write_text(transcript)
    metadata = {
        "schema_version": 1,
        "qualification": False,
        **fields,
        "selection": {
            "mode": "smoke",
            "exact_cases": [],
            "family": None,
            "fraction": [0, 1],
            "expected_cases": list(selection.expected_cases),
        },
        "console_sha256": hashlib.sha256(transcript.encode()).hexdigest(),
        "artifact_sha256": {
            name: hashlib.sha256(payload).hexdigest()
            for name, payload in artifacts.items()
        },
        "result": result,
    }
    (root / "run.json").write_text(json.dumps(metadata))
    return metadata


class VulkanCtsToolingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.identity = cts.load_identity()
        self.receipt = cts.receipt_contract.validate_receipt(cts.RECEIPT_PATH)
        self.selection = cts.resolve_selection(
            self.identity, "smoke", (), None, cts.Fraction(0, 1)
        )

    def _device_fraction_artifacts(
        self, status: str = "Fail", *, fraction: int = 0
    ) -> tuple[tuple[str, ...], dict[str, bytes]]:
        profile = cts.apple_mustpass_profile(fraction)
        cases = (
            "dEQP-VK.info.build",
            "dEQP-VK.reconvergence.maximal.compute.nesting4.0.1",
            profile.restart_before or "dEQP-VK.api.version_check.entry_points",
            "dEQP-VK.wsi.xlib.synthetic",
        )
        selected = (cases[0], cases[2], cases[3])
        receipt_digest = cts.common.sha256_file(cts.RECEIPT_PATH)
        artifacts = {
            "renderer.txt": receipt_renderer(self.receipt),
            "selected.txt": "".join(
                f"{case}\n" for case in selected
            ).encode(),
            "expected.txt": "".join(
                f"{case}\n" for case in cases
            ).encode(),
            "skipped.txt": (
                f"guest-memory\t{cases[1]}\n"
            ).encode(),
            "results.qpa": qpa(
                qpa_result(
                    selected[0], identity="deviceName = Virtio GPU Venus"
                ),
                qpa_result(selected[1], status=status),
                qpa_result(selected[2], status="NotSupported"),
            ).encode(),
            # Historical v3 attempts omitted counters. The v4 profile must
            # account for its planned restart and every selected case.
            "summary.txt": (
                f"cts_rc={'0' if status != 'Fail' else '1'}\n"
                "mode=current-must-pass\n"
                f"expected_case_count={len(selected)}\n"
                f"fraction={fraction},16\n"
                "device_excluded=1\n"
                "device_excluded_guest_memory=1\n"
                "device_exclusion_guest_memory=dEQP-VK.reconvergence.\n"
                f"device_memory_mib={profile.guest_memory_mib}\n"
                "forced_driver_file=/usr/share/vulkan/icd.d/virtio_icd.json\n"
                f"receipt_sha256={receipt_digest}\n"
                "formal_conformance=false\n"
                "qpa_transfer=complete\n"
            ).encode(),
        }
        if profile.restart_before is not None:
            artifacts["summary.txt"] += (
                f"cts_sessions=2\ncts_cases_run={len(selected)}\n"
            ).encode()
        return selected, artifacts

    def test_frozen_identity_and_physical_hold_are_exact(self) -> None:
        campaigns = cts.load_campaigns()
        self.assertEqual(len(self.identity["mandatory_cases"]), 19)
        self.assertFalse(self.identity["cts"]["qualification_ready"])
        self.assertIsNone(self.identity["cts"]["authoritative_build_receipt"])
        self.assertEqual(campaigns["physical_device"]["state"], "held")
        self.assertFalse(campaigns["physical_device"]["launch_permitted"])

    def test_identity_rejects_duplicate_json_keys(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "duplicate.json"
            path.write_text('{"schema_version":1,"schema_version":1}\n')
            with self.assertRaisesRegex(cts.CampaignError, "duplicate JSON key"):
                cts.load_json(path)

    def test_smoke_and_exact_family_selection_are_typed(self) -> None:
        self.assertEqual(len(self.selection.expected_cases), 19)
        exact = cts.resolve_selection(
            self.identity,
            "exact-family",
            ("dEQP-VK.info.device", "dEQP-VK.info.platform"),
            None,
            cts.Fraction(1, 2),
        )
        self.assertEqual(exact.expected_cases, ("dEQP-VK.info.platform",))
        family = cts.resolve_selection(
            self.identity,
            "exact-family",
            (),
            "dEQP-VK.info.device",
            cts.Fraction(0, 1),
        )
        self.assertGreater(len(family.expected_cases), 1)
        with self.assertRaisesRegex(cts.CampaignError, "19-case"):
            cts.resolve_selection(
                self.identity,
                "exact-family",
                ("dEQP-VK.compute.foo",),
                None,
                cts.Fraction(0, 1),
            )

    def test_positive_modes_are_dormant_without_receipt(self) -> None:
        for mode in (
            "frozen-regression",
            "current-must-pass",
            "capability-inventory",
        ):
            with self.subTest(mode=mode):
                with self.assertRaisesRegex(cts.CampaignError, "dormant"):
                    cts.resolve_selection(
                        self.identity, mode, (), None, cts.Fraction(0, 1)
                    )

    def test_receipt_backed_selection_is_exact_and_staged(self) -> None:
        selection = cts.resolve_receipt_selection(
            "smoke", (), None, cts.Fraction(0, 1)
        )
        self.assertEqual(len(selection.expected_cases), 21)
        self.assertEqual(
            selection.expected_cases[0], "dEQP-VK.info.build"
        )
        self.assertEqual(
            selection.source_cases[-1], "dEQP-VK.info.platform"
        )
        self.assertEqual(
            selection.expected_cases[-1],
            "dEQP-VK.info.device_group_peer_memory_features",
        )
        diagnostic = cts.resolve_receipt_selection(
            "exact-family",
            ("dEQP-VK.api.version_check.unavailable_entry_points",),
            None,
            cts.Fraction(0, 1),
        )
        self.assertEqual(
            diagnostic.expected_cases,
            ("dEQP-VK.api.version_check.unavailable_entry_points",),
        )
        change_local = (
            "dEQP-VK.api.version_check.unavailable_entry_points",
            "dEQP-VK.api.object_management.multithreaded_per_thread_resources.event",
        )
        self.assertEqual(
            cts.resolve_receipt_selection(
                "exact-family",
                change_local,
                None,
                cts.Fraction(0, 1),
            ).expected_cases,
            change_local,
        )
        broad = cts.resolve_receipt_selection(
            "current-must-pass", (), None, cts.Fraction(3, 16)
        )
        self.assertTrue(broad.generated_expected)
        self.assertEqual(broad.fraction, cts.Fraction(3, 16))
        self.assertEqual(broad.source_cases, ())
        self.assertEqual(broad.expected_cases, ())
        broad_arguments = cts.receipt_deqp_arguments(
            self.receipt, broad, "/run/uml-vk-cts"
        )
        self.assertIn("--deqp-fraction=3,16", broad_arguments)
        self.assertIn(
            "--deqp-fraction-mandatory-caselist-file="
            + self.receipt["mustpass"]["mandatory"]["path"],
            broad_arguments,
        )
        with self.assertRaisesRegex(cts.CampaignError, "at most 16"):
            cts.resolve_receipt_selection(
                "current-must-pass", (), None, cts.Fraction(0, 17)
            )
        with self.assertRaisesRegex(cts.CampaignError, "complete native"):
            cts.resolve_receipt_selection(
                "smoke", (), None, cts.Fraction(0, 2)
            )

    def test_deqp_argv_forces_icd_and_disables_unsafe_handlers(self) -> None:
        arguments = cts.deqp_arguments(
            self.identity, self.selection, "/run/uml-vk-cts"
        )
        self.assertEqual(arguments[0], "/opt/vk-gl-cts/vulkan/deqp-vk")
        self.assertIn("--deqp-watchdog=disable", arguments)
        self.assertIn("--deqp-crashhandler=disable", arguments)
        self.assertIn("--deqp-test-oom=disable", arguments)
        launcher = cts.build_guest_launcher(
            self.identity, self.selection, timeout=900,
            case_list=cts.GUEST_CASE_LIST_PATH,
        )
        self.assertIn(PAGE_SIZE_CHECK, launcher)
        self.assertIn(
            "VK_DRIVER_FILES=/usr/share/vulkan/icd.d/virtio_icd.json",
            launcher,
        )
        self.assertIn("runuser -u uml", launcher)
        self.assertIn("/dev/virtio-ports/org.uml.host-serial.0", launcher)
        self.assertNotIn("lvp_icd.json", launcher)
        self.assertNotIn("LD_PRELOAD", launcher)
        self.assertNotIn("apt-get", launcher)

    def test_qpa_policy_accepts_virtio_and_rejects_software_or_waiver(self) -> None:
        cases = self.identity["mandatory_cases"][:2]
        payload = qpa(
            qpa_result(cases[0], identity="deviceName = Virtio GPU Venus"),
            qpa_result(cases[1], status="NotSupported"),
        )
        results = cts.common.validate_qpa_union(payload, cases)
        cts.validate_virtio_qpa_identity(payload)
        cts.validate_released_results(
            results, self.identity["released_status_oracle"]
        )
        with self.assertRaisesRegex(cts.CampaignError, "software ICD"):
            cts.validate_virtio_qpa_identity(payload.replace("Virtio GPU Venus", "llvmpipe (LLVM)"))
        waiver = dict(results)
        waiver[cases[0]] = "Waiver"
        with self.assertRaisesRegex(cts.CampaignError, "Waiver"):
            cts.validate_released_results(
                waiver, self.identity["released_status_oracle"]
            )

    def test_qpa_parser_rejects_truncation_duplicate_and_reordering(self) -> None:
        cases = self.identity["mandatory_cases"][:2]
        ordered = qpa(qpa_result(cases[0]), qpa_result(cases[1]))
        cts.common.validate_qpa_union(ordered, cases)
        with self.assertRaisesRegex(cts.CampaignError, "order"):
            cts.common.validate_qpa_union(
                qpa(qpa_result(cases[1]), qpa_result(cases[0])), cases
            )
        with self.assertRaisesRegex(cts.CampaignError, "duplicate"):
            cts.common.parse_qpa(qpa(qpa_result(cases[0]), qpa_result(cases[0])))
        with self.assertRaisesRegex(cts.CampaignError, "no complete session"):
            cts.common.parse_qpa(ordered.removesuffix("#endSession\n"))
        # A guest whose CTS session ended early continues the same traversal in
        # a fresh session, so the transported QPA is a sequence of documents.
        continued = ordered.removesuffix("#endSession\n") + qpa(
            qpa_result(self.identity["mandatory_cases"][2])
        )
        self.assertEqual(
            list(cts.common.parse_qpa(continued)),
            list(self.identity["mandatory_cases"][:3]),
        )

    def test_acknowledged_artifacts_reject_truncation_and_digest_drift(self) -> None:
        transcript = "".join(
            framed(name, payload)
            for name, payload in baseline_artifacts(
                self.identity, self.selection
            ).items()
        )
        decoded = cts.common.decode_artifacts(transcript)
        self.assertEqual(decoded["results.qpa"], b"")
        with self.assertRaisesRegex(cts.CampaignError, "missing its trailer"):
            cts.common.decode_artifacts(transcript.rsplit("\n", 2)[0] + "\n")
        mutated = transcript.replace("|0|H", "|0|I", 1)
        with self.assertRaises(cts.CampaignError):
            cts.common.decode_artifacts(mutated)

    def test_expected_no_device_is_not_a_cts_pass(self) -> None:
        artifacts = baseline_artifacts(self.identity, self.selection)
        transcript = (
            "target implementation = 'Default'\n"
            "VK_ERROR_INITIALIZATION_FAILED\n"
        )
        result = cts.validate_attempt(
            self.identity, self.selection, transcript, artifacts
        )
        self.assertEqual(result["outcome"], "expected-no-device")
        self.assertFalse(result["qualification"])
        bad = dict(artifacts)
        bad["results.qpa"] = qpa(
            *(
                qpa_result(case, identity="Virtio GPU Venus")
                for case in self.selection.expected_cases
            )
        ).encode()
        with self.assertRaisesRegex(cts.CampaignError, "unexpectedly became visible"):
            cts.validate_attempt(self.identity, self.selection, transcript, bad)

    def test_receipt_backed_qpa_is_project_qualification_not_conformance(self) -> None:
        selection = cts.resolve_receipt_selection(
            "smoke", (), None, cts.Fraction(0, 1)
        )
        results = []
        for index, case in enumerate(selection.expected_cases):
            results.append(
                qpa_result(
                    case,
                    identity="deviceName = Virtio GPU Venus" if index == 0 else "",
                )
            )
        qpa_payload = qpa(*results).encode()
        receipt_digest = cts.common.sha256_file(cts.RECEIPT_PATH)
        artifacts = {
            "renderer.txt": (
                "vk_driver_files=/usr/share/vulkan/icd.d/virtio_icd.json\n"
                f"receipt_sha256={receipt_digest}\n"
                "vulkan_loader="
                f"{self.receipt['vulkan_loader']['runtime']['path']}\n"
                "vulkan_loader_sha256="
                f"{self.receipt['vulkan_loader']['runtime']['sha256']}\n"
                '"library_path": "libvulkan_virtio.so"\n'
            ).encode(),
            "selected.txt": "".join(
                f"{case}\n" for case in selection.source_cases
            ).encode(),
            "results.qpa": qpa_payload,
            "summary.txt": (
                "cts_rc=0\n"
                "mode=smoke\n"
                f"expected_case_count={len(selection.expected_cases)}\n"
                "cts_sessions=1\n"
                f"cts_cases_run={len(selection.expected_cases)}\n"
                "forced_driver_file=/usr/share/vulkan/icd.d/virtio_icd.json\n"
                f"receipt_sha256={receipt_digest}\n"
                "formal_conformance=false\n"
                "qpa_transfer=complete\n"
            ).encode(),
        }
        transcript = (
            "verify_file ... || { echo 'UML_VK_CTS_ERROR: not emitted'; }\n"
            "target implementation = 'Default'\n"
        )
        result = cts.validate_receipt_attempt(
            self.receipt, selection, transcript, artifacts
        )
        self.assertTrue(result["qualification"])
        self.assertFalse(result["formal_conformance"])
        with self.assertRaisesRegex(
            cts.CampaignError, "transcript contains UML_VK_CTS_ERROR"
        ):
            cts.validate_receipt_attempt(
                self.receipt,
                selection,
                "[ 1.250000] uml-vulkan-cts-run[42]: "
                "UML_VK_CTS_ERROR: receipt identity changed\n",
                artifacts,
            )

    def test_receipt_fraction_retains_complete_failure_inventory(self) -> None:
        receipt = json.loads(json.dumps(self.receipt))
        selection = cts.resolve_receipt_selection(
            "current-must-pass", (), None, cts.Fraction(3, 16)
        )
        selected_payload = b"vk-default.txt\n"
        receipt["mustpass"]["top_level"]["size"] = len(selected_payload)
        receipt["mustpass"]["top_level"]["sha256"] = hashlib.sha256(
            selected_payload
        ).hexdigest()
        cases = (
            "dEQP-VK.info.build",
            "dEQP-VK.api.version_check.entry_points",
        )
        qpa_payload = qpa(
            qpa_result(
                cases[0], identity="deviceName = Virtio GPU Venus"
            ),
            qpa_result(cases[1], status="Fail"),
        ).encode()
        receipt_digest = cts.common.sha256_file(cts.RECEIPT_PATH)
        artifacts = {
            "renderer.txt": receipt_renderer(receipt),
            "selected.txt": selected_payload,
            "expected.txt": "".join(f"{case}\n" for case in cases).encode(),
            "results.qpa": qpa_payload,
            "summary.txt": (
                "cts_rc=1\n"
                "mode=current-must-pass\n"
                f"expected_case_count={len(cases)}\n"
                "cts_sessions=2\n"
                f"cts_cases_run={len(cases)}\n"
                "fraction=3,16\n"
                "forced_driver_file=/usr/share/vulkan/icd.d/virtio_icd.json\n"
                f"receipt_sha256={receipt_digest}\n"
                "formal_conformance=false\n"
                "qpa_transfer=complete\n"
            ).encode(),
        }
        result = cts.validate_receipt_attempt(receipt, selection, "", artifacts)
        self.assertEqual(result["outcome"], "receipt-backed-cts-failures")
        self.assertEqual(result["selected_case_count"], 2)
        self.assertEqual(result["status_counts"], {"Fail": 1, "Pass": 1})
        self.assertEqual(result["rejected_status_count"], 1)
        self.assertEqual(
            result["first_rejected_result"],
            {"case": cases[1], "status": "Fail"},
        )
        self.assertFalse(result["qualification"])

        transported = cts.common.decode_artifacts(
            "".join(framed(name, payload) for name, payload in artifacts.items())
        )
        self.assertEqual(transported, artifacts)

        artifacts["summary.txt"] = artifacts["summary.txt"].replace(
            b"cts_rc=1", b"cts_rc=0"
        )
        with self.assertRaisesRegex(cts.CampaignError, "exit status disagrees"):
            cts.validate_receipt_attempt(receipt, selection, "", artifacts)

    def test_experimental_no_device_signature_is_product_specific(self) -> None:
        accepted_artifacts = baseline_artifacts(self.identity, self.selection)
        experimental_artifacts = baseline_artifacts(
            self.identity, self.selection, experimental_venus=True
        )
        accepted_transcript = (
            "target implementation = 'Default'\n"
            "VK_ERROR_INITIALIZATION_FAILED\n"
        )
        experimental_transcript = (
            "target implementation = 'Default'\n"
            "VK_ERROR_INCOMPATIBLE_DRIVER\n"
        )
        cts.validate_attempt(
            self.identity,
            self.selection,
            experimental_transcript,
            experimental_artifacts,
            experimental_venus=True,
        )
        with self.assertRaisesRegex(cts.CampaignError, "summary changed"):
            cts.validate_attempt(
                self.identity,
                self.selection,
                accepted_transcript,
                accepted_artifacts,
                experimental_venus=True,
            )
        with self.assertRaisesRegex(cts.CampaignError, "summary changed"):
            cts.validate_attempt(
                self.identity,
                self.selection,
                experimental_transcript,
                experimental_artifacts,
            )

    def test_verify_only_reparses_every_artifact_and_rejects_tamper(self) -> None:
        artifacts = baseline_artifacts(self.identity, self.selection)
        transcript = (
            "target implementation = 'Default'\n"
            "VK_ERROR_INITIALIZATION_FAILED\n"
        )
        result = cts.validate_attempt(
            self.identity, self.selection, transcript, artifacts
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_run(root, self.selection, artifacts, transcript, result)
            self.assertEqual(cts.verify_only(root, self.identity), result)
            (root / "artifacts/summary.txt").write_bytes(b"tampered\n")
            with self.assertRaisesRegex(cts.CampaignError, "digest changed"):
                cts.verify_only(root, self.identity)

    def test_provider_identity_schema_preserves_legacy_runs(self) -> None:
        artifacts = baseline_artifacts(
            self.identity, self.selection, experimental_venus=True
        )
        transcript = (
            "target implementation = 'Default'\n"
            "VK_ERROR_INCOMPATIBLE_DRIVER\n"
        )
        result = cts.validate_attempt(
            self.identity,
            self.selection,
            transcript,
            artifacts,
            experimental_venus=True,
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            metadata = write_run(root, self.selection, artifacts, transcript,
                                 result, experimental_venus=True)
            self.assertEqual(cts.verify_only(root, self.identity), result)

            metadata["schema_version"] = 3
            (root / "run.json").write_text(json.dumps(metadata))
            with self.assertRaisesRegex(cts.CampaignError, "provider identity"):
                cts.verify_only(root, self.identity)
            metadata["kosmickrisp_sha256"] = "a" * 64
            (root / "run.json").write_text(json.dumps(metadata))
            self.assertEqual(cts.verify_only(root, self.identity), result)

    def test_future_receipt_schema_is_fail_closed(self) -> None:
        schema = cts.load_json(cts.FUTURE_RECEIPT_SCHEMA_PATH)
        required = set(schema["required"])
        self.assertTrue(
            {
                "base_rootfs",
                "derivative",
                "claim_scope",
                "protocol",
                "mesa",
                "vulkan_headers",
                "vulkan_loader",
                "cts",
                "toolchain",
                "install_closure",
                "mustpass",
                "runner_oracle",
                "exclusions",
            }.issubset(required)
        )
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(
            schema["properties"]["cts"]["allOf"][1]["properties"][
                "reported_cpu"
            ]["const"],
            "DE_CPU_ARM_64",
        )

    def test_an_explicit_case_list_is_a_guest_file_not_an_argument(self) -> None:
        # A 7,891-case list once overflowed the emulator's argument space
        # (E2BIG) before the guest booted, because the list travelled inside
        # the launcher on the command line.
        cases = tuple(
            f"dEQP-VK.api.info.format_properties.synthetic_{index:05d}"
            for index in range(8000)
        )
        selection = cts.ReceiptSelection(
            "exact-family", cases, cases, cts.Fraction(0, 1), False
        )
        options = cts.parse_arguments(("--qualification", "--dry-run"))
        with tempfile.TemporaryDirectory() as directory:
            artifacts = Path(directory) / "run"
            command, guest_files = cts.plan_receipt_harness_command(
                options, self.receipt, selection, artifacts
            )
            self.assertEqual(
                command,
                cts.build_receipt_harness_command(
                    options, self.receipt, selection, artifacts
                ),
            )
            self.assertLess(sum(len(argument) + 1 for argument in command), 8192)
            self.assertFalse(artifacts.exists(), "building the command wrote files")
            staged = {
                argument.split("=", 2)[1]: Path(argument.split("=", 2)[2])
                for argument in command
                if argument.startswith("--guest-file=")
            }
            self.assertEqual(
                set(staged), {cts.GUEST_CASE_LIST_PATH, cts.CTS_SERVICE_PATH}
            )
            self.assertEqual(
                cts.prepare_receipt_harness_command(
                    options, self.receipt, selection, artifacts
                ),
                command,
            )
            payload = "".join(f"{case}\n" for case in cases).encode()
            case_list = staged[cts.GUEST_CASE_LIST_PATH]
            self.assertEqual(case_list.read_bytes(), payload)
            self.assertEqual(case_list.stat().st_mode & 0o777, 0o644)
            launcher = staged[cts.CTS_SERVICE_PATH]
            self.assertEqual(launcher.stat().st_mode & 0o777, 0o755)
            text = launcher.read_text()
            self.assertIn(
                f"install -m 0644 -o uml -g uml {cts.GUEST_CASE_LIST_PATH}", text
            )
            self.assertIn(hashlib.sha256(payload).hexdigest(), text)
            self.assertNotIn(cases[0], text)
            self.assertNotIn("base64 -d", text)
            self.assertNotIn("base64 -d", " ".join(command))

    def test_harness_commands_boot_the_launcher_they_stage(self) -> None:
        artifacts = Path("/artifacts/run")
        run = hashlib.sha256(os.fsencode(artifacts.resolve())).hexdigest()[:20]
        arguments = (
            "--dry-run", "--engine", "gadgets", "--cpus", "6", "--memory",
            "6G", "--guest-timeout", "1300",
        )
        development = cts.parse_arguments(arguments)
        qualification = cts.parse_arguments(("--qualification", *arguments))

        def harness(options, rootfs_type, staged, command_line):
            return [
                "env", "UML_GPU_CAPABILITY_REPORT=1", sys.executable,
                str(INTEGRATION_GPU / "run_gpu_guest.py"),
                "--emulator", str(options.emulator),
                "--rootfs", str(options.rootfs),
                "--rootfs-type", rootfs_type,
                "--memory", "6G",
                "--unit", "multi-user.target",
                "--timeout", "1300",
                "--artifact-root", str(artifacts / "guest"),
                "--serial-socket",
                f"/private/tmp/uml-vk-cts-console-{run}.sock",
                f"--virtio-serial-socket=/private/tmp/uml-cts-artifact-{run}.sock",
                *staged.arguments,
                "--command", command_line,
                "--",
                f"umid=vkcts-{run}", "sysrq_always_enabled", "ncpus=6",
                "--execution-engine=gadgets", "--gpu", "--gpu-display=vnc",
                "--gpu-vnc-bind=127.0.0.1", "--gpu-vnc-port=0",
                "--gpu-width=1280", "--gpu-height=720",
            ]

        def staged_guest_paths(staged):
            return [
                argument.split("=", 2)[1] for argument in staged.arguments
            ]

        # The development lane checks the CTS status against the no-device
        # baseline, on the frozen EROFS image.
        command, staged = cts.plan_harness_command(
            development, self.identity, self.selection, artifacts
        )
        baseline = self.identity["development_no_device_baseline"]
        self.assertEqual(
            command,
            harness(
                development, "erofs", staged,
                "set +e; /usr/local/libexec/uml-vulkan-cts; cts_status=$?; "
                f'set -e; [ "$cts_status" -eq {baseline["process_return_code"]} ]',
            ),
        )
        self.assertEqual(
            staged_guest_paths(staged),
            [cts.GUEST_CASE_LIST_PATH, cts.CTS_SERVICE_PATH],
        )
        self.assertEqual(
            command,
            cts.build_harness_command(
                development, self.identity, self.selection, artifacts
            ),
        )
        # The receipt lane always succeeds, so the validator sees the result.
        case = ("dEQP-VK.api.info.a",)
        selection = cts.ReceiptSelection(
            "exact-family", case, case, cts.Fraction(0, 1), False
        )
        command, staged = cts.plan_receipt_harness_command(
            qualification, self.receipt, selection, artifacts
        )
        self.assertEqual(
            command,
            harness(
                qualification, "ext4", staged,
                "set +e; /usr/local/libexec/uml-vulkan-cts; set -e; :",
            ),
        )
        self.assertEqual(
            staged_guest_paths(staged),
            [cts.GUEST_CASE_LIST_PATH, cts.CTS_SERVICE_PATH],
        )
        # A WSI selection boots the campaign target instead, which starts the
        # staged service.
        case = ("dEQP-VK.wsi.a",)
        selection = cts.ReceiptSelection(
            "exact-family", case, case, cts.Fraction(0, 1), False
        )
        command, staged = cts.plan_receipt_harness_command(
            qualification, self.receipt, selection, artifacts
        )
        self.assertEqual(
            command,
            [
                "env", "UML_GPU_CAPABILITY_REPORT=1", sys.executable,
                str(INTEGRATION_GPU / "run_gpu_guest.py"),
                "--emulator", str(qualification.emulator),
                "--rootfs", str(qualification.rootfs),
                "--rootfs-type", "ext4", "--root-device", "/dev/ubda",
                "--memory", "6G", "--unit", "uml-cts.target",
                "--timeout", "0", "--no-auto-poweroff",
                "--artifact-root", str(artifacts / "guest"),
                f"--virtio-serial-socket=/private/tmp/uml-cts-artifact-{run}.sock",
                *staged.arguments,
                "--", "ncpus=6", "--execution-engine=gadgets", "ip=off",
                "--gpu", "--gpu-display=vnc", "--gpu-width=1280",
                "--gpu-height=720", "--gpu-vnc-bind=127.0.0.1",
                "--gpu-vnc-port=0",
            ],
        )
        self.assertEqual(
            staged_guest_paths(staged),
            [
                cts.GUEST_CASE_LIST_PATH, cts.CTS_SERVICE_PATH,
                cts.CTS_WSI_SERVICE_PATH, cts.common.CTS_TARGET_PATH,
            ],
        )
        self.assertFalse(artifacts.exists(), "planning a command wrote files")

    def test_an_installed_case_list_must_match_its_digest(self) -> None:
        selection = cts.ReceiptSelection(
            "exact-family", ("dEQP-VK.a",), ("dEQP-VK.a",), cts.Fraction(0, 1),
            False,
        )
        setup = cts.selected_list_setup(selection, "/installed/list")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "bin").mkdir()
            # install(1) here only needs to copy; ownership belongs to the guest.
            (root / "bin" / "install").write_text(
                '#!/bin/sh\nwhile [ "$#" -gt 2 ]; do shift; done\ncp "$1" "$2"\n'
            )
            (root / "bin" / "install").chmod(0o755)
            script = setup.replace("/installed/list", str(root / "list"))
            for content, status in ((b"dEQP-VK.a\n", 0), (b"dEQP-VK.b\n", 1)):
                (root / "list").write_bytes(content)
                result = subprocess.run(
                    ["sh", "-eu", "-c", f'selected={root}/selected\n{script}'],
                    env={"PATH": f"{root}/bin:/usr/bin:/bin:/usr/sbin:/sbin"},
                    capture_output=True,
                    text=True,
                    timeout=10,
                    check=False,
                )
                self.assertEqual(result.returncode, status, result.stdout)
        # Every harness installs the list as a file; none embeds it.
        with self.assertRaisesRegex(cts.CampaignError, "as a file"):
            cts.selected_list_setup(selection, None)

    def test_cli_selects_only_the_checked_derivative_for_qualification(self) -> None:
        options = cts.parse_arguments(("--qualification", "--dry-run"))
        self.assertEqual(options.rootfs, cts.DEFAULT_RECEIPT_ROOTFS)
        self.assertTrue(options.qualification)
        selection = cts.resolve_receipt_selection(
            "smoke", (), None, cts.Fraction(0, 1)
        )
        command = cts.build_receipt_harness_command(
            options,
            self.receipt,
            selection,
            Path("/tmp/receipt-test"),
        )
        joined = " ".join(command)
        self.assertIn("--rootfs-type ext4", joined)
        self.assertIn("--serial-socket", command)
        self.assertIn("umid=vkcts-", joined)
        self.assertIn("sysrq_always_enabled", command)
        self.assertEqual(command[:2], ["env", "UML_GPU_CAPABILITY_REPORT=1"])
        self.assertNotIn("UML_GPU_VENUS_METAL_ARGUMENT_BUFFERS", command)
        self.assertIn(
            "set +e; /usr/local/libexec/uml-vulkan-cts; set -e; :", joined
        )
        launcher = cts.build_receipt_guest_launcher(
            self.receipt,
            selection,
            timeout=900,
            progress_timeout=600,
            case_list=cts.GUEST_CASE_LIST_PATH,
        )
        self.assertIn(PAGE_SIZE_CHECK, launcher)
        self.assertEqual(cts.ARTIFACT_BASE64_LINE_LENGTH, 3 * 1024)
        self.assertIn(
            f'base64 -w {cts.ARTIFACT_BASE64_LINE_LENGTH} "$compressed"',
            launcher,
        )
        self.assertIn('runuser -u uml -- env', launcher)
        self.assertIn('> "$cts_output" 2>&1 &', launcher)
        self.assertIn(
            "UML_VK_CTS_PROGRESS|qpa_bytes=%s|stalled_seconds=%s"
            "|cts_rss_kb=%s|cts_peak_kb=%s|mem_available_kb=%s"
            "|output_free_kb=%s|case=%s",
            launcher,
        )
        # The growing QPA lives in the guest's own tmpfs, and a fraction that
        # runs it out of room loses results no host evidence can recover, so
        # the room left there is reported alongside the memory -- and the CTS
        # process's own output, which no lane transports, gives way first.
        self.assertIn('df -kP "$output"', launcher)
        self.assertIn('[ "$output_free_kb" -lt 65536 ]', launcher)
        relaxed = cts.build_receipt_guest_launcher(
            self.receipt,
            selection,
            timeout=900,
            progress_timeout=600,
            relax_diagnostics=True,
            case_list=cts.GUEST_CASE_LIST_PATH,
        )
        # The watch reads that output by offset, so the two never coexist.
        self.assertNotIn('[ "$output_free_kb" -lt 65536 ]', relaxed)
        self.assertIn("relax_offset", relaxed)
        # The guest reaper leaves no other record of how close a fraction was,
        # so the progress line carries the CTS process's resident and peak size.
        self.assertIn("cts_process_field VmRSS", launcher)
        self.assertIn("cts_process_field VmHWM", launcher)
        self.assertIn('[ "$entry_comm" = deqp-vk ]', launcher)
        self.assertIn('tail -c 1048576 "$qpa"', launcher)
        self.assertIn('[ "$stalled_seconds" -ge "$progress_timeout" ]', launcher)
        self.assertIn("progress_timeout=600", launcher)
        self.assertIn('status=124', launcher)
        # The guest never signals a wedged case: a Venus hang can leave it
        # unkillable, so the host owns recovery from the checkpointed prefix.
        self.assertNotIn("kill -TERM", launcher)
        self.assertNotIn("kill -KILL", launcher)
        self.assertIn(self.receipt["vulkan_loader"]["runtime"]["path"], launcher)
        self.assertIn(
            self.receipt["vulkan_loader"]["runtime"]["sha256"], launcher
        )

    def test_staged_kosmickrisp_matches_the_configured_provider(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            emulator = root / "build/bin/uml-darwin"
            staged = (
                emulator.parent
                / "Frameworks/KosmicKrisp.framework/KosmicKrisp"
            )
            source = (
                root
                / "prefix/Frameworks/KosmicKrisp.framework/KosmicKrisp"
            )
            staged.parent.mkdir(parents=True)
            source.parent.mkdir(parents=True)
            emulator.write_bytes(b"emulator")
            source.write_bytes(b"provider")
            staged.write_bytes(source.read_bytes())
            (root / "build/CMakeCache.txt").write_text(
                "UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK:INTERNAL="
                f"{source.parent}\n",
                encoding="utf-8",
            )

            self.assertEqual(
                cts.verify_staged_kosmickrisp(emulator),
                hashlib.sha256(b"provider").hexdigest(),
            )
            staged.write_bytes(b"stale")
            with self.assertRaisesRegex(cts.CampaignError, "stale"):
                cts.verify_staged_kosmickrisp(emulator)

            (root / "build/CMakeCache.txt").unlink()
            self.assertEqual(
                cts.verify_staged_kosmickrisp(emulator),
                hashlib.sha256(b"stale").hexdigest(),
            )

    def test_full_receipt_fractions_use_wsi_without_changing_selection(self) -> None:
        renderer = WSI_RENDERER
        for index in range(8):
            with self.subTest(fraction=index):
                selection = cts.resolve_receipt_selection(
                    "current-must-pass", (), None, cts.Fraction(index, 8)
                )
                self.assertTrue(selection.generated_expected)
                self.assertEqual(selection.source_cases, ())
                self.assertEqual(selection.expected_cases, ())
                self.assertTrue(cts.receipt_selection_uses_wsi(selection))
                self.assertTrue(cts.receipt_selection_uses_wayland(selection))
                for arguments in (
                    cts.receipt_deqp_arguments(self.receipt, selection, "/run/uml-vk-cts"),
                    cts.receipt_list_arguments(self.receipt, selection, "/run/uml-vk-cts"),
                ):
                    self.assertIn(f"--deqp-fraction={index},8", arguments)
                    self.assertIn("--deqp-caselist-file=/run/uml-vk-cts/selected.txt", arguments)
                    self.assertIn(
                        "--deqp-fraction-mandatory-caselist-file="
                        + self.receipt["mustpass"]["mandatory"]["path"],
                        arguments,
                    )
                options = cts.parse_arguments((
                    "--qualification", "--dry-run", "--mode", "current-must-pass",
                    "--fraction", f"{index},8", "--cpus", "4", "--memory", "4G",
                ))
                command = cts.build_receipt_harness_command(
                    options, self.receipt, selection, Path("/tmp/receipt-full-wsi-test")
                )
                self.assertEqual(
                    command[command.index("--unit") + 1], "uml-cts.target"
                )
                self.assertNotIn("graphical.target", command)
                self.assertIn("--no-auto-poweroff", command)
                self.assertNotIn("--command", command)
                self.assertIn(
                    "--guest-file=/etc/systemd/system/uml-cts.target=",
                    " ".join(command),
                )
                self.assertIn("ncpus=4", command)
                cts.validate_wsi_operation(selection, {}, renderer)
                for evidence in renderer.splitlines(keepends=True):
                    with self.assertRaisesRegex(cts.CampaignError, "WSI evidence"):
                        cts.validate_wsi_operation(
                            selection, {}, renderer.replace(evidence, "")
                        )

        smoke = cts.resolve_receipt_selection("smoke", (), None, cts.Fraction(0, 1))
        self.assertFalse(cts.receipt_selection_uses_wsi(smoke))

    def test_wsi_exact_family_uses_the_canonical_native_xorg_session(self) -> None:
        cases = (
            "dEQP-VK.wsi.xcb.incremental_present.scale_none.fifo.identity."
            "inherit.reference",
            "dEQP-VK.wsi.xcb.incremental_present.scale_none.fifo.identity."
            "inherit.incremental_present",
        )
        selection = cts.resolve_receipt_selection(
            "exact-family", cases, None, cts.Fraction(0, 1)
        )
        self.assertTrue(cts.receipt_selection_uses_wsi(selection))
        launcher = cts.build_receipt_guest_launcher(
            self.receipt,
            selection,
            timeout=900,
            progress_timeout=600,
            use_wsi=True,
            case_list=cts.GUEST_CASE_LIST_PATH,
        )
        self.assertFalse(cts.receipt_selection_uses_wayland(selection))
        self.assertIn(cts.common.native_x11_setup(), launcher)
        self.assertNotIn("WAYLAND_DISPLAY=", launcher)
        self.assertIn('DISPLAY="$xdisplay"', launcher)
        self.assertIn('XAUTHORITY="$xauthority"', launcher)
        self.assertIn('pgrep -x Xvfb', launcher)
        self.assertIn('systemctl poweroff --no-wall --no-block', launcher)
        syntax = subprocess.run(
            ["sh", "-n"],
            input=launcher,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(syntax.returncode, 0, syntax.stderr)

        renderer = WSI_RENDERER
        with self.assertRaisesRegex(cts.CampaignError, "Pass pair"):
            cts.validate_wsi_operation(
                selection,
                {case: "NotSupported" for case in cases},
                renderer,
            )
        cts.validate_wsi_operation(
            selection,
            {case: "Pass" for case in cases},
            renderer,
        )
        with self.assertRaisesRegex(cts.CampaignError, "forbidden"):
            cts.validate_wsi_operation(
                selection, {case: "Pass" for case in cases},
                renderer + "x11_server=xwayland\n",
            )

        options = cts.parse_arguments(
            (
                "--qualification",
                "--dry-run",
                "--cpus",
                "4",
                "--memory",
                "4G",
            )
        )
        command = cts.build_receipt_harness_command(
            options,
            self.receipt,
            selection,
            Path("/tmp/receipt-wsi-test"),
        )
        self.assertEqual(
            command[command.index("--unit") + 1], "uml-cts.target"
        )
        self.assertNotIn("graphical.target", command)
        self.assertIn("--no-auto-poweroff", command)
        self.assertNotIn("--command", command)
        self.assertIn("ncpus=4", command)
        self.assertIn("--virtio-serial-socket=", " ".join(command))

    def test_a_swapchain_family_outside_the_wsi_group_still_gets_a_display(
        self,
    ) -> None:
        """dEQP-VK.api.image_compression_control.swapchain.* creates real
        surfaces and swapchains, so an exact-family run of it needs the same
        native Xorg session and nested Wayland server the dEQP-VK.wsi group
        does. Without one every case reports NotSupported for want of a
        display and the run proves nothing."""
        wayland = (
            "dEQP-VK.api.image_compression_control.swapchain.wayland.disabled"
        )
        xcb = "dEQP-VK.api.image_compression_control.swapchain.xcb.disabled"

        self.assertTrue(cts.case_needs_wsi(wayland))
        self.assertTrue(cts.case_needs_wayland(wayland))
        self.assertTrue(cts.case_needs_wsi(xcb))
        self.assertFalse(cts.case_needs_wayland(xcb))
        self.assertFalse(
            cts.case_needs_wsi("dEQP-VK.api.info.device_properties")
        )
        self.assertFalse(
            cts.case_needs_wayland("dEQP-VK.api.info.device_properties")
        )

        selection = cts.resolve_receipt_selection(
            "exact-family", (xcb, wayland), None, cts.Fraction(0, 1)
        )
        self.assertTrue(cts.receipt_selection_uses_wsi(selection))
        self.assertTrue(cts.receipt_selection_uses_wayland(selection))

        launcher = cts.build_receipt_guest_launcher(
            self.receipt,
            selection,
            timeout=900,
            progress_timeout=600,
            use_wsi=True,
            case_list=cts.GUEST_CASE_LIST_PATH,
        )
        self.assertIn(cts.common.native_x11_setup(wayland=True), launcher)
        self.assertIn("WAYLAND_DISPLAY=wayland-0", launcher)
        self.assertIn('DISPLAY="$xdisplay"', launcher)
        syntax = subprocess.run(
            ["sh", "-n"],
            input=launcher,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(syntax.returncode, 0, syntax.stderr)

    def test_device_exclusions_carry_a_cause_and_spare_the_mandatory_list(
        self,
    ) -> None:
        self.assertEqual(
            cts.validate_device_exclusions(cts.DEVICE_EXCLUSIONS),
            cts.DEVICE_EXCLUSIONS,
        )
        cases = (
            "dEQP-VK.api.smoke.create_sampler",
            "dEQP-VK.reconvergence.maximal.compute.nesting4.0.1",
            "dEQP-VK.reconvergence_other.case",
            "dEQP-VK.pipeline.monolithic.pipeline_binary.dedicated.not_enough_space",
        )
        # A group prefix takes a whole subtree; an exact name takes one case.
        exclusions = (
            *cts.DEVICE_EXCLUSIONS,
            cts.DeviceExclusion(
                "aborts-deqp",
                (
                    "dEQP-VK.pipeline.monolithic.pipeline_binary.dedicated."
                    "not_enough_space",
                ),
            ),
        )
        selected, skipped = cts.device_exclusion_partition(cases, exclusions)
        self.assertEqual(
            selected,
            (
                "dEQP-VK.api.smoke.create_sampler",
                "dEQP-VK.reconvergence_other.case",
            ),
        )
        self.assertEqual(
            skipped,
            {
                "guest-memory": (
                    "dEQP-VK.reconvergence.maximal.compute.nesting4.0.1",
                ),
                "aborts-deqp": (
                    "dEQP-VK.pipeline.monolithic.pipeline_binary.dedicated."
                    "not_enough_space",
                ),
            },
        )
        # No exclusion means no partition, so every other lane is untouched.
        self.assertEqual(cts.device_exclusion_partition(cases, ()), (cases, {}))
        for rejected in (
            cts.DeviceExclusion("Guest Memory", ("dEQP-VK.reconvergence.",)),
            cts.DeviceExclusion("guest-memory", ()),
            cts.DeviceExclusion("guest-memory", ("reconvergence.",)),
            cts.DeviceExclusion("guest-memory", ("dEQP-VK.",)),
        ):
            with self.subTest(exclusion=rejected):
                with self.assertRaises(cts.CampaignError):
                    cts.validate_device_exclusions((rejected,))
        with self.assertRaisesRegex(cts.CampaignError, "repeats"):
            cts.validate_device_exclusions(
                (*cts.DEVICE_EXCLUSIONS, *cts.DEVICE_EXCLUSIONS)
            )
        with self.assertRaisesRegex(cts.CampaignError, "mandatory case"):
            cts.validate_device_exclusions(
                (cts.DeviceExclusion("guest-memory", ("dEQP-VK.info.",)),)
            )

    def test_only_a_device_campaign_excludes_and_it_transports_the_skip_list(
        self,
    ) -> None:
        selection = cts.resolve_receipt_selection(
            "current-must-pass", (), None, cts.Fraction(0, 16)
        )
        arguments = {
            "timeout": cts.APPLE_MUSTPASS_TIMEOUT,
            "progress_timeout": cts.APPLE_MUSTPASS_PROGRESS_TIMEOUT,
        }
        plain = cts.build_receipt_guest_launcher(
            self.receipt, selection, **arguments
        )
        excluded = cts.build_receipt_guest_launcher(
            self.receipt,
            selection,
            **arguments,
            device_exclusions=cts.DEVICE_EXCLUSIONS,
        )
        # The macOS lanes get exactly the script they had: dEQP applies the
        # released fraction filter to both the enumeration and the run, and
        # nothing is skipped.
        self.assertEqual(plain.count("--deqp-fraction=0,16"), 2)
        self.assertNotIn("skipped", plain)
        self.assertNotIn("device_excluded", plain)
        self.assertNotIn("device_memory_mib", plain)
        # The device campaign enumerates the fraction and then runs an explicit
        # subset of it, so the filter must not run a second time over that.
        self.assertEqual(excluded.count("--deqp-fraction=0,16"), 1)
        self.assertEqual(
            excluded.count("--deqp-fraction-mandatory-caselist-file"), 1
        )
        execution = cts.receipt_deqp_arguments(
            self.receipt, selection, "/run/uml-vk-cts", preselected=True
        )
        self.assertNotIn("--deqp-fraction=0,16", execution)
        self.assertIn(
            "--deqp-caselist-file=/run/uml-vk-cts/selected.txt", execution
        )
        every = " ".join(
            "-e '" + cts._exclusion_expression(pattern) + "'"
            for exclusion in cts.DEVICE_EXCLUSIONS
            for pattern in exclusion.patterns
        )
        self.assertIn(
            f'grep -v {every} "$expected" > "$selected"', excluded
        )
        for exclusion in cts.DEVICE_EXCLUSIONS:
            patterns = " ".join(
                "-e '" + cts._exclusion_expression(pattern) + "'"
                for pattern in exclusion.patterns
            )
            # Every skipped case carries its own cause, so the list says why.
            self.assertIn(
                f'grep {patterns} "$expected" | awk -v reason='
                f"{exclusion.reason} "
                '\'{ print reason "\\t" $0 }\' >> "$skipped"',
                excluded,
            )
            self.assertIn(f"device_excluded_{exclusion.row}=%s", excluded)
            self.assertIn(
                f"device_exclusion_{exclusion.row}=%s\\n' "
                + " ".join(exclusion.patterns),
                excluded,
            )
        self.assertIn("renderer.txt selected.txt expected.txt skipped.txt", excluded)
        self.assertIn("device_excluded=%s", excluded)
        self.assertIn(
            f"device_memory_mib=%s\\n' {cts.APPLE_MUSTPASS_GUEST_MEMORY_MIB}",
            excluded,
        )
        # The reported case count is what the guest will actually run, taken
        # once before any continuation rewrites the remaining selection.
        self.assertIn("expected_case_count=%s", excluded)
        self.assertIn("' \"${selected_count:-0}\" >> \"$summary\"", excluded)
        self.assertIn('selected_count=$(wc -l < "$run_list" | tr -d \' \')', excluded)
        self.assertIn("skipped.txt", cts.common.ALLOWED_ARTIFACTS)
        for script in (plain, excluded):
            syntax = subprocess.run(
                ["sh", "-n"], input=script, text=True, capture_output=True,
                check=False,
            )
            self.assertEqual(syntax.returncode, 0, syntax.stderr)
        # A selection the exclusion cannot be reasoned about is refused rather
        # than silently ignored.
        family = cts.resolve_receipt_selection(
            "exact-family", (), "dEQP-VK.info", cts.Fraction(0, 1)
        )
        with self.assertRaisesRegex(cts.CampaignError, "enumerated released"):
            cts.build_receipt_guest_launcher(
                self.receipt,
                family,
                **arguments,
                device_exclusions=cts.DEVICE_EXCLUSIONS,
            )
        with self.assertRaisesRegex(cts.CampaignError, "resumed shard"):
            cts.build_receipt_guest_launcher(
                self.receipt,
                selection,
                **arguments,
                resume_after=10,
                device_exclusions=cts.DEVICE_EXCLUSIONS,
            )

    def test_device_completion_is_independent_of_case_failures(self) -> None:
        campaign = cts.apple_mustpass_campaign(0)
        _, artifacts = self._device_fraction_artifacts()
        result = device.validate_device_mustpass_attempt(
            campaign, "", artifacts
        )
        self.assertTrue(result["traversal_complete"])
        self.assertFalse(result["accepted_statuses"])
        self.assertEqual(result["rejected_status_count"], 1)

        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            status = device.report(artifacts, "", campaign=campaign)
        self.assertEqual(status, 1)
        text = printed.getvalue()
        self.assertIn("vulkan_campaign_complete=true", text)
        self.assertIn("vulkan_statuses_accepted=false", text)
        self.assertIn("vulkan_campaign_failed=true", text)

        _, accepted = self._device_fraction_artifacts("NotSupported")
        with contextlib.redirect_stdout(io.StringIO()):
            status = device.report(accepted, "", campaign=campaign)
        self.assertEqual(status, 0)

    def test_headless_wsi_surface_independent_results_keep_their_verdicts(
        self,
    ) -> None:
        campaign = cts.apple_mustpass_campaign(0)
        for platform in ("xlib", "wayland", "xcb", "headless", "direct"):
            for leaf in (
                "query_devgroup_present_capabilities", "destroy_null_handle"
            ):
                for status in ("Pass", "Fail"):
                    case = f"dEQP-VK.wsi.{platform}.surface.{leaf}"
                    with self.subTest(case=case, status=status):
                        selected, artifacts = self._device_fraction_artifacts(
                            status
                        )
                        artifacts = {
                            name: payload.replace(
                                selected[-1].encode(), case.encode()
                            )
                            for name, payload in artifacts.items()
                        }
                        artifacts["results.qpa"] = artifacts[
                            "results.qpa"
                        ].replace(
                            b'StatusCode="NotSupported"',
                            f'StatusCode="{status}"'.encode(),
                        )
                        result = device.validate_device_mustpass_attempt(
                            campaign, "", artifacts
                        )
                        self.assertTrue(result["traversal_complete"])
                        self.assertEqual(
                            result["rejected_status_count"],
                            2 if status == "Fail" else 0,
                        )
                        self.assertEqual(
                            result["accepted_statuses"], status == "Pass"
                        )

        for leaf in (
            "query_capabilities", "query_formats", "create",
            "query_devgroup_present_capabilities_extra",
            "destroy_null_handle_extra",
        ):
            with self.subTest(rejected_leaf=leaf), self.assertRaisesRegex(
                cts.CampaignError, "headless Vulkan WSI"
            ):
                cts.validate_headless_wsi_results(
                    {f"dEQP-VK.wsi.headless.surface.{leaf}": "Pass"}
                )

    def test_device_completion_rejects_missing_duplicate_or_lost_results(
        self,
    ) -> None:
        campaign = cts.apple_mustpass_campaign(0)
        selected, artifacts = self._device_fraction_artifacts()

        missing = dict(artifacts)
        missing["results.qpa"] = qpa(
            qpa_result(
                selected[0], identity="deviceName = Virtio GPU Venus"
            )
        ).encode()
        with self.assertRaisesRegex(cts.CampaignError, "missing"):
            device.validate_device_mustpass_attempt(campaign, "", missing)

        duplicate = dict(artifacts)
        duplicate["results.qpa"] = qpa(
            qpa_result(
                selected[0], identity="deviceName = Virtio GPU Venus"
            ),
            qpa_result(selected[0], status="Fail"),
        ).encode()
        with self.assertRaisesRegex(cts.CampaignError, "duplicate QPA case"):
            device.validate_device_mustpass_attempt(campaign, "", duplicate)

        reordered = dict(artifacts)
        reordered["results.qpa"] = qpa(
            qpa_result(
                selected[0], identity="deviceName = Virtio GPU Venus"
            ),
            qpa_result(selected[2], status="NotSupported"),
            qpa_result(selected[1], status="Fail"),
        ).encode()
        with self.assertRaisesRegex(cts.CampaignError, "order"):
            device.validate_device_mustpass_attempt(campaign, "", reordered)

        presented = dict(artifacts)
        presented["results.qpa"] = presented["results.qpa"].replace(
            b'StatusCode="NotSupported"', b'StatusCode="Pass"'
        )
        with self.assertRaisesRegex(cts.CampaignError, "headless Vulkan WSI"):
            device.validate_device_mustpass_attempt(campaign, "", presented)

        lost = dict(artifacts)
        lost["summary.txt"] = lost["summary.txt"].replace(
            b"qpa_transfer=complete\n",
            b"qpa_transfer=failed\nqpa_missing_bytes=16\n",
        )
        with self.assertRaisesRegex(cts.CampaignError, "transfer is incomplete"):
            device.validate_device_mustpass_attempt(campaign, "", lost)

        recovered = dict(artifacts)
        recovered["summary.txt"] = recovered["summary.txt"].replace(
            b"qpa_transfer=complete\n",
            b"qpa_transfer=recovered\nqpa_missing_bytes=0\n",
        )
        result = device.validate_device_mustpass_attempt(
            campaign, "", recovered
        )
        self.assertTrue(result["traversal_complete"])

        still_missing = dict(recovered)
        still_missing["summary.txt"] = still_missing["summary.txt"].replace(
            b"qpa_missing_bytes=0", b"qpa_missing_bytes=1"
        )
        with self.assertRaisesRegex(cts.CampaignError, "still misses bytes"):
            device.validate_device_mustpass_attempt(
                campaign, "", still_missing
            )

        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            status = device.report(
                artifacts,
                "",
                rejected=("results.qpa.0001",),
                campaign=campaign,
            )
        self.assertEqual(status, 1)
        self.assertIn("vulkan_campaign_complete=false", printed.getvalue())

    def test_non_mustpass_report_fails_closed_on_delivered_evidence(
        self,
    ) -> None:
        campaign = cts.APPLE_RECEIPT_SMOKE_CAMPAIGN

        def reported(
            artifacts: dict[str, bytes],
            *,
            oom_report: str | None = None,
        ) -> tuple[int, str]:
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):
                status = device.report(
                    artifacts,
                    "",
                    oom_report=oom_report,
                    campaign=campaign,
                )
            text = printed.getvalue()
            self.assertNotIn("vulkan_campaign_complete=", text)
            self.assertNotIn("vulkan_statuses_accepted=", text)
            return status, text

        # Older smoke summaries did not state transfer diagnostics.  Their
        # shape remains valid, without gaining a mustpass completion marker.
        historical = {"summary.txt": b"cts_rc=0\nmode=smoke\n"}
        status, _ = reported(historical)
        self.assertEqual(status, 0)

        malformed = {
            "summary.txt": b"cts_rc=1\ncts_rc=0\nmode=smoke\n"
        }
        status, text = reported(malformed)
        self.assertEqual(status, 1)
        self.assertIn("Vulkan CTS summary is malformed", text)

        qpa_payload = qpa(
            qpa_result("dEQP-VK.info.build")
        ).encode()
        recovered = {
            "summary.txt": (
                b"cts_rc=0\nmode=smoke\n"
                b"qpa_transfer=recovered\nqpa_missing_bytes=0\n"
            ),
            "results.qpa": qpa_payload,
        }
        status, _ = reported(recovered)
        self.assertEqual(status, 0)

        invalid_transfers = (
            b"qpa_transfer=failed\nqpa_missing_bytes=64\n",
            b"qpa_transfer=recovered\nqpa_missing_bytes=1\n",
            b"qpa_transfer=complete\nqpa_missing_bytes=0\n",
            b"qpa_missing_bytes=0\n",
        )
        for transfer in invalid_transfers:
            with self.subTest(transfer=transfer):
                status, text = reported(
                    {
                        "summary.txt": (
                            b"cts_rc=0\nmode=smoke\n" + transfer
                        ),
                        "results.qpa": qpa_payload,
                    }
                )
                self.assertEqual(status, 1)
                self.assertIn("campaign_evidence_error=", text)

        killed = (
            "Out of memory: Killed process 1604 (deqp-vk) "
            "total-vm:2793424kB"
        )
        status, text = reported(historical, oom_report=killed)
        self.assertEqual(status, 1)
        self.assertIn("guest_out_of_memory=", text)

        nonaccepted = {
            "summary.txt": (
                b"cts_rc=0\nmode=smoke\nqpa_transfer=complete\n"
            ),
            "results.qpa": qpa(
                qpa_result("dEQP-VK.info.build", status="Fail")
            ).encode(),
        }
        status, text = reported(nonaccepted)
        self.assertEqual(status, 1)
        self.assertIn(
            "CTS exit status disagrees with its delivered QPA", text
        )

        unknown_status = {
            "summary.txt": (
                b"cts_rc=0\nmode=smoke\nqpa_transfer=complete\n"
            ),
            "results.qpa": qpa(
                qpa_result("dEQP-VK.info.build", status="UnknownResult")
            ).encode(),
        }
        status, text = reported(unknown_status)
        self.assertEqual(status, 1)
        self.assertIn("released Vulkan CTS returned unknown status", text)

    def test_fraction03_profile_plans_one_exact_process_boundary(self) -> None:
        profile = cts.apple_mustpass_profile(3)
        selection = cts.resolve_receipt_selection(
            "current-must-pass", (), None, cts.Fraction(3, 16)
        )
        launcher = cts.build_receipt_guest_launcher(
            self.receipt,
            selection,
            timeout=cts.APPLE_MUSTPASS_TIMEOUT,
            progress_timeout=cts.APPLE_MUSTPASS_PROGRESS_TIMEOUT,
            device_exclusions=cts.DEVICE_EXCLUSIONS,
        )

        self.assertEqual(profile.guest_memory_mib, 1280)
        self.assertEqual(profile.restart_pause_seconds, 20)
        self.assertIn(profile.restart_before, launcher)
        self.assertIn("device_memory_mib=%s\\n' 1280", launcher)
        self.assertIn(
            'head -n "$planned_prefix_count" "$selected" > "$continuation"',
            launcher,
        )
        self.assertIn(
            'selected_count=$(wc -l < "$selected" | tr -d \' \')',
            launcher,
        )
        self.assertIn(
            '[ "$cts_cases" -eq "$planned_prefix_count" ]', launcher
        )
        self.assertIn(
            'tail -n +$((planned_prefix_count + 1)) "$selected"', launcher
        )
        self.assertEqual(
            launcher.count("pause_limit=$((pause_start + 20))"), 1
        )
        self.assertIn('[ "$cts_sessions" -lt 8 ] || break', launcher)
        self.assertIn(
            'if [ "$cts_sessions" -eq 1 ] || [ "$status" -eq 0 ]; then\n'
            '        status=$session_status\n'
            '    fi',
            launcher,
        )
        # Syntax checking does not execute the generated retirement delay.
        syntax = subprocess.run(
            ["sh", "-n"],
            input=launcher,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(syntax.returncode, 0, syntax.stderr)

        # Exercise the generated shell with an early ResourceError-sized
        # prefix. The first continuation retains the unfinished planned
        # prefix; reaching the cumulative boundary then selects the suffix
        # beginning with the boundary case, exactly once and in order.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            selected = root / "selected.txt"
            continuation = root / "continue.txt"
            state = root / "state.txt"
            cases = (
                "dEQP-VK.fixture.first",
                "dEQP-VK.fixture.second",
                profile.restart_before,
                "dEQP-VK.fixture.last",
            )
            selected.write_text(
                "".join(f"{case}\n" for case in cases), encoding="utf-8"
            )
            setup_start = launcher.index("restart_matches=$(grep")
            setup_end = launcher.index(': > "$cts_output"', setup_start)
            setup = launcher[setup_start:setup_end]
            shell = subprocess.run(
                ["sh"],
                input=(
                    "set -eu\n"
                    "chown() { :; }\n"
                    f"selected={shlex.quote(str(selected))}\n"
                    f"continuation={shlex.quote(str(continuation))}\n"
                    + setup
                ),
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(shell.returncode, 0, shell.stderr)
            self.assertEqual(
                continuation.read_text(encoding="utf-8").splitlines(),
                list(cases[:2]),
            )

            transition_start = launcher.index("    planned_restart=0\n")
            transition_end = launcher.index(
                '    [ -s "$run_list" ]', transition_start
            )
            transition = launcher[transition_start:transition_end]

            def run_transition(cts_cases: int) -> tuple[int, int]:
                command = (
                    "set -eu\n"
                    "chown() { :; }\n"
                    f"selected={shlex.quote(str(selected))}\n"
                    f"continuation={shlex.quote(str(continuation))}\n"
                    f"state={shlex.quote(str(state))}\n"
                    'run_list="$continuation"\n'
                    "session_cases=1\n"
                    f"cts_cases={cts_cases}\n"
                    "planned_prefix_count=2\n"
                    "planned_restart_pending=1\n"
                    "while :; do\n"
                    + transition
                    + 'printf \'%s %s\\n\' "$planned_restart_pending" '
                    '"$planned_restart" > "$state"\n'
                    "break\n"
                    "done\n"
                )
                result = subprocess.run(
                    ["sh"], input=command, text=True, capture_output=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                pending, planned = state.read_text(encoding="utf-8").split()
                return int(pending), int(planned)

            self.assertEqual(run_transition(1), (1, 0))
            self.assertEqual(
                continuation.read_text(encoding="utf-8").splitlines(),
                [cases[1]],
            )
            self.assertEqual(run_transition(2), (0, 1))
            suffix = continuation.read_text(encoding="utf-8").splitlines()
            self.assertEqual(suffix, list(cases[2:]))
            self.assertEqual([cases[0], cases[1], *suffix], list(cases))

        ordinary = cts.build_receipt_guest_launcher(
            self.receipt,
            cts.resolve_receipt_selection(
                "current-must-pass", (), None, cts.Fraction(2, 16)
            ),
            timeout=cts.APPLE_MUSTPASS_TIMEOUT,
            progress_timeout=cts.APPLE_MUSTPASS_PROGRESS_TIMEOUT,
            device_exclusions=cts.DEVICE_EXCLUSIONS,
        )
        self.assertNotIn("planned restart before", ordinary)
        self.assertNotIn("pause_limit", ordinary)

    def test_fraction03_restart_pauses_for_host_memory_within_a_bound(
        self,
    ) -> None:
        launcher = cts.build_receipt_guest_launcher(
            self.receipt,
            cts.resolve_receipt_selection(
                "current-must-pass", (), None, cts.Fraction(3, 16)
            ),
            timeout=cts.APPLE_MUSTPASS_TIMEOUT,
            progress_timeout=cts.APPLE_MUSTPASS_PROGRESS_TIMEOUT,
            device_exclusions=cts.DEVICE_EXCLUSIONS,
        )
        pause_start = launcher.index(
            '    if [ "$planned_restart" -eq 1 ]; then\n        pause_start='
        )
        pause_end = launcher.index(
            "\n    else\n        printf 'UML_VK_CTS_NOTE: continuing",
            pause_start,
        )
        pause = launcher[pause_start:pause_end] + "\n    fi\n"
        length = cts.APPLE_MUSTPASS_FRACTION03_RESTART_PAUSE_SECONDS

        def pause_before_suffix(deadline: int) -> tuple[str, int]:
            """Run the pause on a fake clock that each sleep advances."""

            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                clock = root / "clock"
                clock.write_text("1000\n", encoding="utf-8")
                bin_directory = root / "bin"
                bin_directory.mkdir()
                stubs = {
                    "date": 'cat "$FAKE_CLOCK"',
                    "sleep": (
                        'now=$(($(cat "$FAKE_CLOCK") + 1))\n'
                        'echo "$now" > "$FAKE_CLOCK"'
                    ),
                }
                for name, body in stubs.items():
                    stub = bin_directory / name
                    stub.write_text(f"#!/bin/sh\n{body}\n", encoding="utf-8")
                    stub.chmod(0o755)
                script = (
                    "planned_restart=1\n"
                    "selected_count=10\n"
                    "cts_cases=4\n"
                    f"deadline={deadline}\n" + pause
                )
                result = subprocess.run(
                    ["sh", "-c", script],
                    text=True,
                    capture_output=True,
                    check=False,
                    timeout=60,
                    env={
                        "PATH": f"{bin_directory}:/usr/bin:/bin",
                        "FAKE_CLOCK": str(clock),
                    },
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                elapsed = int(clock.read_text(encoding="utf-8")) - 1000
                return result.stdout.strip(), elapsed

        note, elapsed = pause_before_suffix(5000)
        self.assertEqual(elapsed, length)
        self.assertIn(f"paused {length} s for host graphics memory", note)
        self.assertIn("continuing 6 cases", note)

        # The pause never runs past the shard's own deadline.
        note, elapsed = pause_before_suffix(1010)
        self.assertEqual(elapsed, 10)
        self.assertIn("paused 10 s for host graphics memory", note)

    def test_device_cli_enforces_the_baked_memory_before_launch(self) -> None:
        fraction03 = cts.apple_mustpass_profile(3)
        options = argparse.Namespace(
            campaign=fraction03.identifier, memory_mib=None
        )
        self.assertEqual(device.apply_campaign_profile(options), fraction03)
        self.assertEqual(options.memory_mib, 1280)

        wrong = argparse.Namespace(
            campaign=fraction03.identifier, memory_mib=1536
        )
        with self.assertRaisesRegex(cts.CampaignError, "baked for 1280 MiB"):
            device.apply_campaign_profile(wrong)

        ordinary = cts.apple_mustpass_profile(2)
        options = argparse.Namespace(
            campaign=ordinary.identifier, memory_mib=None
        )
        self.assertEqual(device.apply_campaign_profile(options), ordinary)
        self.assertEqual(options.memory_mib, 1536)

        with tempfile.TemporaryDirectory() as temporary:
            artifact_root = Path(temporary) / "must-not-exist"
            arguments = (
                "--device", "fixture",
                "--campaign", fraction03.identifier,
                "--memory-mib", "1536",
                "--artifact-root", str(artifact_root),
            )
            with mock.patch.object(device.sys, "platform", "darwin"), \
                 mock.patch.object(device, "run_campaign") as run, \
                 self.assertRaisesRegex(
                     cts.CampaignError, "baked for 1280 MiB"
                 ):
                device.main(list(arguments))
            run.assert_not_called()
            self.assertFalse(artifact_root.exists())

    def test_device_summary_must_match_the_baked_memory_profile(self) -> None:
        profile = cts.apple_mustpass_profile(3)
        _, artifacts = self._device_fraction_artifacts("Pass", fraction=3)
        result = device.validate_device_mustpass_attempt(
            profile.identifier, "", artifacts
        )
        self.assertTrue(result["traversal_complete"])
        self.assertTrue(result["accepted_statuses"])

        for old, new, error in (
            (b"device_memory_mib=1280", b"device_memory_mib=1536", "summary"),
            (b"cts_sessions=2\ncts_cases_run=3\n", b"", "session count"),
            (b"cts_sessions=2", b"cts_sessions=1", "session count"),
            (b"cts_sessions=2", b"cts_sessions=9", "session count"),
            (b"cts_cases_run=3", b"cts_cases_run=2", "complete selection"),
        ):
            with self.subTest(mutation=new):
                changed = dict(artifacts)
                changed["summary.txt"] = artifacts["summary.txt"].replace(
                    old, new
                )
                with self.assertRaisesRegex(cts.CampaignError, error):
                    device.validate_device_mustpass_attempt(
                        profile.identifier, "", changed
                    )

    def test_a_guest_memory_kill_ends_the_device_wait_and_still_reports(
        self,
    ) -> None:
        reaper = (
            "[ 6741.320880] Out of memory: Killed process 1604 (deqp-vk) "
            "total-vm:2793424kB, anon-rss:1164432kB, file-rss:384kB\n"
        )
        for killed, expected in ((reaper, True), ("", False)):
            with self.subTest(killed=bool(killed)):
                elapsed, report = self._device_wait(killed)
                if expected:
                    self.assertIsNotNone(report)
                    self.assertIn("deqp-vk", report)
                    # Promptly, not after the stall guard.
                    self.assertLessEqual(elapsed, device.OOM_GRACE_SECONDS + 30)
                else:
                    self.assertIsNone(report)
                    self.assertGreaterEqual(elapsed, 1800)
        # A killed run has no summary, so its executed prefix's own verdicts and
        # the reaper's line are what the report carries.
        artifacts = {
            "results.qpa": qpa(
                qpa_result("dEQP-VK.api.smoke.create_sampler"),
                qpa_result("dEQP-VK.api.smoke.create_shader", "Fail"),
            ).encode(),
            "skipped.txt": (
                b"guest-memory\tdEQP-VK.reconvergence.maximal.compute"
                b".nesting4.0.1\n"
            ),
        }
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            status = device.report(artifacts, "", reaper.strip())
        self.assertEqual(status, 1)
        text = printed.getvalue()
        self.assertIn("the campaign delivered no summary", text)
        self.assertIn("Pass=1", text)
        self.assertIn("Fail=1", text)
        self.assertIn("skipped_for_a_device_limit=1", text)
        self.assertIn("skipped_guest-memory=1", text)
        self.assertIn("guest_out_of_memory=", text)
        self.assertNotIn("vulkan_campaign_complete=", text)
        self.assertNotIn("vulkan_statuses_accepted=", text)

    def _device_wait(self, killed: str) -> tuple[float, str | None]:
        """Drive the device console wait over a scripted transcript."""

        clock = {"now": 0.0}

        class Console:
            def __init__(self, *_arguments, stdout=None, **_keywords) -> None:
                self.returncode = None
                stdout.write(b"Kernarium Vulkan CTS mustpass campaign enabled\n")
                if killed:
                    stdout.write(killed.encode())
                stdout.flush()

            def poll(self) -> None:
                return None

            def send_signal(self, _number: int) -> None:
                self.returncode = 0

            def wait(self, timeout: float | None = None) -> int:
                self.returncode = 0
                return 0

        options = argparse.Namespace(
            device="fixture",
            campaign=cts.apple_mustpass_campaign(0),
            rootfs_filename="rootfs-vulkan-venus.ext4",
            payload="kernarium-payload-fixture.cpio",
            systemd_unit="uml-cts.target",
            engine="gadgets",
            memory_mib=cts.APPLE_MUSTPASS_GUEST_MEMORY_MIB,
            cpus=1,
            timeout=28200,
            stall_timeout=1800,
        )
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "console.log"
            with contextlib.ExitStack() as stack:
                stack.enter_context(
                    mock.patch.object(device.subprocess, "Popen", Console)
                )
                stack.enter_context(
                    mock.patch.object(
                        device.time, "monotonic", lambda: clock["now"]
                    )
                )
                stack.enter_context(
                    mock.patch.object(
                        device.time,
                        "sleep",
                        lambda seconds: clock.__setitem__(
                            "now", clock["now"] + seconds
                        ),
                    )
                )
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                try:
                    _, report, _ = device.run_campaign(options, log)
                except cts.common.CampaignError as error:
                    self.assertIn("stopped growing", str(error))
                    report = None
        return clock["now"], report

    def test_the_baked_campaign_generation_is_part_of_its_identity(self) -> None:
        names = cts.apple_campaigns()
        self.assertEqual(len(names), cts.APPLE_MUSTPASS_FRACTIONS + 2)
        for index in range(cts.APPLE_MUSTPASS_FRACTIONS):
            version = 4 if index == 3 else 3
            self.assertEqual(
                cts.apple_mustpass_campaign(index),
                f"vulkan-mustpass-{index:02d}of16-v{version}",
            )
        self.assertIsNone(
            cts.apple_mustpass_profile_for_campaign(
                "vulkan-mustpass-03of16-v3"
            )
        )
        with self.assertRaises(cts.CampaignError):
            cts.apple_mustpass_campaign(cts.APPLE_MUSTPASS_FRACTIONS)


if __name__ == "__main__":
    unittest.main()
