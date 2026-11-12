#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
from collections import Counter
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

INTEGRATION = Path(__file__).resolve().parents[2] / "integration/gpu"
sys.path.insert(0, str(INTEGRATION))
import run_graphics_cts_campaign as campaign  # noqa: E402
import run_vk_gl_cts as gl  # noqa: E402
import test_run_vk_gl_cts as fixtures  # noqa: E402
import receipt_fixtures  # noqa: E402

# How long a test waits for a child process or a worker thread it started. The
# wait is a hard bound that only a failure reaches, so it outlasts a host under
# many times its normal load instead of being sized to the time a test needs.
WAIT_SECONDS = 60


def setUpModule() -> None:
    receipt_fixtures.bind_vulkan_receipt(campaign.vk)


def ownership_record(attempt: Path) -> dict:
    token = "b" * 32
    path = attempt / gl.guest_process.JOURNAL_NAME
    path.write_text("".join(json.dumps(record) + "\n" for record in (
        {"event": "starting", "token": token, "owner_pid": 1001},
        {"event": "started", "pid": 1002},
        {"event": "reaped", "pid": 1002, "return_code": 0},
    )))
    return {"guest_ownership_token": token, "guest_ownership_sha256": gl.sha256_file(path)}


# The renderer probe a gl46 shard writes, kept to the lines verification reads.
# The Virgl text is what the final campaign's gl46-run0002 and gl46-run0003
# shards recorded and a renderer-blind verification step rejected.
PROBE_PREAMBLE = (
    "device=/dev/dri/card0\ndevice=/dev/dri/renderD128\n"
    "x11_server=xorg\nx11_dri3=1\nx11_present=1\nx11_glamor=1\n"
    "x11_connected=1\nx11_drm_fd=1\nx11_blanking=off\nguest_page_size=4096\n"
)
RENDERER_PROBES = {
    "zink": (
        PROBE_PREAMBLE
        + "OpenGL renderer string: zink over Virtio-GPU Venus\n"
    ).encode(),
    "virgl": (
        PROBE_PREAMBLE
        + "x11_display_renderer=virgl\n"
        + "OpenGL vendor string: Mesa\n"
        + "OpenGL renderer string: virgl "
          "(zink Vulkan 1.4(Apple M1 Ultra (MESA_KOSMICKRISP)))\n"
    ).encode(),
}


def renderer_probe(definition: dict) -> bytes:
    return RENDERER_PROBES[definition["opengl"]["renderer"]]


def spec(directory: Path, renderer: str = "zink") -> dict:
    receipt = fixtures.zink_gl_derivative_receipt(gl.load_identity())
    emulator = directory / "uml-darwin"
    emulator.write_bytes(b"fixture executable")
    emulator.chmod(0o755)
    runtime_identity = {"emulator_sha256": gl.sha256_file(emulator), "kosmickrisp_sha256": "2" * 64}
    _, contract = gl.load_zink_gl_derivative_receipt(receipt, gl.load_identity())
    inventory = fixtures.write_config_inventory_evidence(
        directory / "inventory", "gl46", contract, runtime_identity=runtime_identity,
        renderer=renderer,
    )
    config_rows = [{"id": 1, "surface_types": ["window", "pixmap", "pbuffer"]},
                   {"id": 2, "surface_types": ["pbuffer", "pixmap"]}]
    path = directory / "opengl.receipt.json"
    path.write_text(json.dumps(receipt))
    vk_receipt = campaign.vk.receipt_contract.validate_receipt(campaign.vk.RECEIPT_PATH)
    vk_receipt["mustpass"]["unique_case_count"] = 10
    return {
        "suites": ["vulkan", "gl46"], "fraction_count": 8,
        "cpus": 4, "memory": "4G", "engine": "jit", "page_profile": "4k",
        "shard_timeout": 900, "guest_timeout": 1500, "console_stall_timeout": 600,
        "console_stall_busy_timeout": 1800,
        "max_resumptions": campaign.common.DEFAULT_MAX_RESUMPTIONS,
        "emulator": str(emulator), "interpreter": sys.executable,
        "runners": {suite: str(path) for suite, path in campaign.RUNNERS.items()},
        **runtime_identity,
        "source": {"git": {"revision": "a" * 40, "dirty_paths": []}},
        "vulkan": {"rootfs": str(directory / "rootfs-vulkan-venus.ext4"), "receipt": vk_receipt,
                   "receipt_path": str(campaign.vk.RECEIPT_PATH),
                   "receipt_sha256": gl.sha256_file(campaign.vk.RECEIPT_PATH)},
        "opengl": {"renderer": renderer,
                   "rootfs": str(directory / "rootfs-vulkan-zink-gl.ext4"), "receipt": receipt,
                   "receipt_path": str(path), "receipt_sha256": gl.sha256_file(path),
                   "config_inventory_run": str(inventory), "egl_configs": config_rows,
                   "egl_config_sha256": "e" * 64,
                   "runs": gl.opengl_cts_plan.expand_runs(receipt["opengl_cts"]["mustpass"], config_rows)},
    }


def with_gles32(directory: Path, definition: dict, renderer: str = "zink") -> dict:
    """The same specification with the ES 3.2 matrix added to it."""
    definition = copy.deepcopy(definition)
    receipt = definition["opengl"]["receipt"]
    _, contract = gl.load_zink_gl_derivative_receipt(receipt, gl.load_identity())
    inventory = fixtures.write_config_inventory_evidence(
        directory / "gles32-inventory", "gles32", contract,
        runtime_identity={key: definition[key] for key in
                          ("emulator_sha256", "kosmickrisp_sha256")},
        renderer=renderer,
    )
    manifest = gl.load_gles32_matrix()
    config_rows = definition["opengl"]["egl_configs"]
    attributes = json.loads(inventory.read_text())["egl_config_attributes"]
    definition["suites"] = ["vulkan", "gl46", "gles32"]
    definition["gles32"] = {
        **{key: definition["opengl"][key] for key in
           ("renderer", "rootfs", "receipt", "receipt_path", "receipt_sha256")},
        "config_inventory_run": str(inventory), "egl_configs": config_rows,
        "egl_config_sha256": "f" * 64, "manifest": manifest,
        "egl_config_attributes": attributes,
        "egl_config_attributes_sha256": gl.egl_attribute_digest(attributes),
        "runs": campaign.common.gles_cts_plan.expand_runs(
            manifest, config_rows, attributes),
    }
    return definition


def results(shard: campaign.Shard) -> dict[str, str]:
    if shard.suite == "vulkan":
        return {"dEQP-VK.mandatory0": "Pass", "dEQP-VK.mandatory1": "Pass",
                f"dEQP-VK.case{shard.index}": "NotSupported"}
    files = fixtures.opengl_mustpass_files()
    manifest = gl.opengl_cts_plan.build_manifest(files.__getitem__)
    runs = gl.opengl_cts_plan.expand_runs(manifest, [
        {"id": 1, "surface_types": ["window", "pixmap", "pbuffer"]},
        {"id": 2, "surface_types": ["pbuffer", "pixmap"]},
    ])
    leaf = next(row for row in manifest["leaves"] if row["id"] == runs[shard.run_index]["leaf_id"])
    cases = files[leaf["path"]].decode().splitlines()
    return {cases[index]: "NotSupported" if index == 0 else "Pass"
            for index in range(shard.index, len(cases), 8)}


def make_gl_attempt(root: Path, definition: dict, shard: campaign.Shard,
                    probe: bytes | None = None) -> Path:
    attempt = campaign.attempt_path(root, shard)
    run = attempt / "driver-output/fixture-run"
    (run / "artifacts").mkdir(parents=True)
    command = campaign.child_command(definition, shard, attempt / "driver-output")
    options = gl.parse_arguments(command[3:])
    selection = options.matrix_run.selection
    rows = results(shard)
    artifacts = {
        "renderer.txt": probe if probe is not None else renderer_probe(definition),
        "summary.txt": b"cts_rc=0\n",
        "selected.txt": "".join(f"{case}\n" for case in rows).encode(),
        "results.qpa": fixtures.qpa(*(fixtures.qpa_result(case, status)
                                       for case, status in rows.items())).encode(),
    }
    for name, payload in artifacts.items():
        (run / "artifacts" / name).write_bytes(payload)
    (run / gl.ARTIFACT_TRANSPORT_LOG).write_text("".join(
        fixtures.framed_artifact(name, payload) for name, payload in artifacts.items()))
    (run / "console.log").write_text("Darwin UML stopped: result=0 exitcode=0\n")
    (attempt / "runner.log").write_text("fixture runner completed\n")
    item = definition["opengl"]
    metadata = {
        "schema_version": 1, "qualification": True, "formal_conformance": False,
        "engine": "jit", "memory": "4G", "cpus": 4, "page_profile": "4k",
        "kosmickrisp_sha256": definition["kosmickrisp_sha256"],
        "git": definition["source"]["git"], "console_sha256": gl.sha256_file(run / "console.log"),
        "adapter": "macos", "backend": gl.host_backend_label(item["renderer"]),
        "renderer": item["renderer"],
        "display": "vnc", "context": "egl", "mode": "mustpass-matrix-shard", "surface": [1280, 720],
        "rootfs_type": "ext4", "rootfs_receipt": item["receipt"],
        "rootfs_receipt_sha256": item["receipt_sha256"],
        "rootfs_sha256": item["receipt"]["derivative"]["sha256"],
        "glcts_sha256": item["receipt"]["opengl_cts"]["executable"]["sha256"],
        "emulator_sha256": definition["emulator_sha256"],
        "successful_case_statuses": item["receipt"]["runner_oracle"]["successful"],
        "api": "gl46", "leaf": selection.leaf.identifier, "leaf_sha256": selection.leaf.sha256,
        "fraction": [shard.index, 8], "exact_cases": [],
        "seed": options.matrix_run.configuration.base_seed,
        "matrix": gl.matrix_metadata(options),
        "expected_cases": selection.expected_count, "emulator_return_code": 0,
        "command": gl.build_harness_command(options, selection, run),
        "max_resumptions": definition["max_resumptions"],
        "console_stall_timeout": 600, "console_stall_busy_timeout": 1800,
        "session_count": 1, "artifacts_provenance": "single-session-guest",
        "completed_cases": len(rows), "status_totals": gl.status_totals(rows),
        "waived_results": {},
        "selected_caselist_sha256": hashlib.sha256(artifacts["selected.txt"]).hexdigest(),
        "qpa_sha256": hashlib.sha256(artifacts["results.qpa"]).hexdigest(),
        "renderer_sha256": hashlib.sha256(artifacts["renderer.txt"]).hexdigest(),
        "artifact_transport_sha256": gl.sha256_file(run / gl.ARTIFACT_TRANSPORT_LOG),
    }
    (run / "run.json").write_text(json.dumps(metadata))
    (attempt / "attempt.json").write_text(json.dumps({
        **ownership_record(attempt),
        "shard": shard.name(), "command": command, "state": "finished", "return_code": 0,
        "error": None, "runner_log_sha256": gl.sha256_file(attempt / "runner.log"),
    }))
    return run


def make_resumed_gl_attempt(root: Path, definition: dict, shard: campaign.Shard) -> Path:
    """A gl46 attempt whose first guest stalled and whose second finished it."""
    attempt = campaign.attempt_path(root, shard)
    run = attempt / "driver-output/fixture-run"
    run.mkdir(parents=True)
    command = campaign.child_command(definition, shard, attempt / "driver-output")
    options = gl.parse_arguments(command[3:])
    selection = options.matrix_run.selection
    rows = results(shard)
    cases = list(rows)
    renderer = renderer_probe(definition)
    selected = "".join(f"{case}\n" for case in cases).encode()
    stalled_qpa = fixtures.qpa(
        *(fixtures.qpa_result(case, rows[case]) for case in cases[:1])
    ).replace("#endSession\n", "").encode()
    final_qpa = fixtures.qpa(
        *(fixtures.qpa_result(case, rows[case]) for case in cases[1:])
    ).encode()
    ownership = ownership_record(attempt)
    payloads = [
        {"renderer.txt": renderer, "selected.txt": selected,
         "results.qpa": stalled_qpa},
        {"renderer.txt": renderer,
         "selected.txt": "".join(f"{case}\n" for case in cases[1:]).encode(),
         "results.qpa": final_qpa, "summary.txt": b"cts_rc=0\n"},
    ]
    sessions = []
    for index, payload in enumerate(payloads):
        guest = run if index == 0 else run / f"{gl.SESSION_DIRECTORY}/{index:02d}"
        guest.mkdir(parents=True, exist_ok=True)
        artifacts = run / f"{gl.SESSION_DIRECTORY}/{index:02d}/artifacts"
        artifacts.mkdir(parents=True)
        for name, value in payload.items():
            (artifacts / name).write_bytes(value)
        console = guest / "console.log"
        console.write_text(
            "Darwin UML stopped: result=0 exitcode=0\n"
            if index
            else f"Test case '{cases[1]}'..\nwent quiet\n"
        )
        transport = guest / gl.ARTIFACT_TRANSPORT_LOG
        transport.write_text("".join(
            fixtures.framed_artifact(name if name != "results.qpa" else "results.qpa.0000", value)
            for name, value in payload.items()))
        journal = None
        if index:
            journal = f"{gl.SESSION_DIRECTORY}/{index:02d}/{gl.guest_process.JOURNAL_NAME}"
            (run / journal).write_text(
                (attempt / gl.guest_process.JOURNAL_NAME).read_text()
            )
        sessions.append(gl.SessionEvidence(
            index=index,
            outcome="completed" if index else "stalled",
            resume_after=0 if index == 0 else 1,
            qpa_text=gl.decode_qpa(payload["results.qpa"]),
            completed_cases=tuple(cases[:1]) if index == 0 else tuple(cases[1:]),
            console_case=None if index else cases[1],
            console=str(console.relative_to(run)),
            artifact_transport=str(transport.relative_to(run)),
            artifacts_directory=str(artifacts.relative_to(run)),
            return_code=0,
            ownership_journal=journal,
        ))
    merged = gl.merged_shard_artifacts(sessions, run, len(cases))
    gl.write_artifacts(merged, run / "artifacts")
    merged_results = gl.validate_qpa_union(
        gl.decode_qpa(merged["results.qpa"]), cases
    )
    (attempt / "runner.log").write_text("fixture runner resumed\n")
    item = definition["opengl"]
    metadata = {
        "schema_version": 1, "qualification": True, "formal_conformance": False,
        "engine": "jit", "memory": "4G", "cpus": 4, "page_profile": "4k",
        "kosmickrisp_sha256": definition["kosmickrisp_sha256"],
        "git": definition["source"]["git"],
        "console_sha256": gl.sha256_file(run / "console.log"),
        "adapter": "macos", "backend": gl.host_backend_label(item["renderer"]),
        "renderer": item["renderer"],
        "display": "vnc", "context": "egl", "mode": "mustpass-matrix-shard",
        "surface": [1280, 720], "rootfs_type": "ext4",
        "rootfs_receipt": item["receipt"],
        "rootfs_receipt_sha256": item["receipt_sha256"],
        "rootfs_sha256": item["receipt"]["derivative"]["sha256"],
        "glcts_sha256": item["receipt"]["opengl_cts"]["executable"]["sha256"],
        "emulator_sha256": definition["emulator_sha256"],
        "successful_case_statuses": item["receipt"]["runner_oracle"]["successful"],
        "api": "gl46", "leaf": selection.leaf.identifier,
        "leaf_sha256": selection.leaf.sha256,
        "fraction": [shard.index, 8], "exact_cases": [],
        "seed": options.matrix_run.configuration.base_seed,
        "matrix": gl.matrix_metadata(options),
        "expected_cases": selection.expected_count, "emulator_return_code": 0,
        "command": gl.build_harness_command(options, selection, run),
        "max_resumptions": definition["max_resumptions"],
        "console_stall_timeout": 600, "console_stall_busy_timeout": 1800,
        "session_count": len(sessions), "artifacts_provenance": "host-merged",
        "sessions": gl.session_records(sessions, run),
        "completed_cases": len(merged_results),
        "status_totals": gl.status_totals(merged_results),
        "waived_results": {},
        "selected_caselist_sha256": hashlib.sha256(merged["selected.txt"]).hexdigest(),
        "qpa_sha256": hashlib.sha256(merged["results.qpa"]).hexdigest(),
        "renderer_sha256": hashlib.sha256(merged["renderer.txt"]).hexdigest(),
        "artifact_transport_sha256": gl.sha256_file(run / gl.ARTIFACT_TRANSPORT_LOG),
    }
    (run / "run.json").write_text(json.dumps(metadata))
    (attempt / "attempt.json").write_text(json.dumps({
        **ownership,
        "shard": shard.name(), "command": command, "state": "finished",
        "return_code": 0, "error": None,
        "runner_log_sha256": gl.sha256_file(attempt / "runner.log"),
    }))
    return run


def campaign_record(definition: dict, attempted: tuple[str, ...] = (),
                    **fields: object) -> dict:
    """The campaign.json of one generation that launched each named shard once."""
    return {
        "schema_version": campaign.SCHEMA_VERSION, "kind": campaign.KIND,
        "plan": campaign.plan_identity(definition),
        "generations": [copy.deepcopy(definition)],
        "attempts": {name: [{"path": f"shards/{name}", "generation": 0}]
                     for name in attempted},
        **fields,
    }


def every_shard_passed(definition, root=Path("/nonexistent")):
    """Resolve every shard to a passing attempt of the current generation."""
    return {shard.name(): {"path": root, "spec": definition, "result": {}}
            for shard in campaign.shards(definition)}


class GraphicsCampaignTests(unittest.TestCase):
    def setUp(self) -> None:
        provider = mock.patch.object(gl, "verify_staged_kosmickrisp", return_value="2" * 64)
        provider.start()
        self.addCleanup(provider.stop)

    def arguments(self, root: Path) -> list[str]:
        return ["--suite", "both", "--campaign-dir", str(root / "campaign"),
                "--emulator", str(root / "uml-darwin"), "--vulkan-rootfs", str(root / "vk.ext4"),
                "--opengl-rootfs", str(root / "gl.ext4"), "--opengl-receipt", str(root / "gl.json"),
                "--opengl-config-inventory-run", str(root / "inventory/run.json")]

    def test_fixed_eight_fractions_and_four_cpu_four_gib_commands(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            definition = spec(Path(temporary))
            keys = campaign.shards(definition)
            self.assertEqual(len(keys), 98)
            self.assertEqual(Counter(key.suite for key in keys), {"vulkan": 8, "gl46": 90})
            for key in keys:
                command = campaign.child_command(definition, key, Path(temporary) / key.name())
                options = (campaign.vk if key.suite == "vulkan" else gl).parse_arguments(command[3:])
                self.assertEqual(options.cpus, 4)
                self.assertEqual(options.memory, "4G")
                self.assertEqual(options.engine, "jit")
                self.assertTrue(options.qualification)
                self.assertEqual(command[command.index("--fraction") + 1], f"{key.index},8")
                self.assertEqual(options.console_stall_timeout, 600)
                if key.suite == "gl46":
                    self.assertEqual(options.console_stall_busy_timeout, 1800)
                else:
                    self.assertNotIn("--console-stall-busy-timeout", command)
                self.assertEqual(str(options.rootfs), definition["vulkan" if key.suite == "vulkan" else "opengl"]["rootfs"])

    def test_es32_matrix_adds_its_own_shards_runs_and_inventory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = with_gles32(root, spec(root))
            keys = campaign.shards(definition)
            counts = Counter(key.suite for key in keys)
            self.assertEqual(counts["vulkan"], 8)
            self.assertEqual(counts["gl46"], 90)
            # Three one-time runs plus eight first-config and five other-config
            # rows and the six AOSP runs, eight fractions each, minus the
            # provably empty fractions of the one-case ES 2.0 no-context leaf.
            self.assertEqual(len(definition["gles32"]["runs"]), 22)
            self.assertEqual(counts["gles32"], 22 * 8 - 7)
            names = [key.name() for key in keys if key.suite == "gles32"]
            self.assertIn("gles32-run0000-00of08", names)
            self.assertIn("gles32-run0015-07of08", names)
            self.assertIn("gles32-run0021-07of08", names)
            self.assertEqual(len(set(names)), len(names))
            shard = next(key for key in keys if key.suite == "gles32"
                         and key.run_index == 15)
            command = campaign.child_command(definition, shard, root / shard.name())
            self.assertEqual(command[command.index("--matrix") + 1], "gles32")
            self.assertEqual(
                command[command.index("--config-inventory-run") + 1],
                definition["gles32"]["config_inventory_run"])
            self.assertEqual(command[command.index("--fraction") + 1],
                             f"{shard.index},8")
            run = definition["gles32"]["runs"][15]
            self.assertEqual(command[command.index("--mustpass-config-index") + 1],
                             str(run["configuration_index"]))
            # The AOSP runs address the configuration they resolved out of
            # aospConfigs, which the conformant list need not contain.
            aosp = definition["gles32"]["runs"][-1]
            self.assertEqual(aosp["config_scope"], "aosp")
            self.assertEqual(aosp["surface_type"], "none")
            self.assertEqual(definition["gles32"]["runs"][-4]["egl_config_id"], 3)

    def test_adding_the_es32_matrix_keeps_every_recorded_shard(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            extended = with_gles32(root, definition)
            before = campaign.plan_identity(definition)
            after = campaign.plan_identity(extended)
            self.assertEqual(campaign.extend_plan(before, after), after)
            self.assertEqual(before["opengl"], after["opengl"])
            self.assertEqual(
                {key.name() for key in campaign.shards(definition)},
                {key.name() for key in campaign.shards(extended)
                 if key.suite != "gles32"})
            for broken in (
                {**before, "page_profile": "16k"},
                {**before, "opengl": {**before["opengl"], "renderer": "virgl"}},
                {**after, "suites": ["vulkan", "gles32", "gl46"]},
                after,
            ):
                with self.subTest(plan=sorted(broken)), \
                        self.assertRaisesRegex(gl.CampaignError, "changed plan"):
                    campaign.extend_plan(broken, after)

    def test_relaunch_with_the_es32_matrix_records_the_extended_plan(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            extended = with_gles32(root, definition)
            location = root / "campaign"
            location.mkdir()
            (location / "campaign.json").write_text(json.dumps(campaign_record(
                definition, ("gl46-run0000-00of08",),
                created_at="2026-01-01T00:00:00+00:00", state="pending")))
            passed = every_shard_passed(definition)
            with mock.patch.object(campaign, "build_spec", return_value=extended), \
                    mock.patch.object(campaign, "classify_attempts",
                                      return_value=(passed, [], {}, [])) as classify, \
                    mock.patch.object(campaign, "run_pending",
                                      return_value=({}, {}, [])) as launch, \
                    mock.patch.object(campaign, "aggregate", return_value={}):
                campaign.main(self.arguments(root) + ["--dry-run"][:0])
            classify.assert_called_once()
            launch.assert_called_once()
            record = json.loads((location / "campaign.json").read_text())
            self.assertEqual(record["plan"], campaign.plan_identity(extended))
            self.assertIn("gl46-run0000-00of08", record["attempts"])
            self.assertEqual(record["generations"][0], definition)

    def test_the_gl_suite_runs_both_opengl_matrices_without_vulkan(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = ["--campaign-dir", str(root / "campaign"),
                      "--emulator", str(root / "uml-darwin"),
                      "--opengl-rootfs", str(root / "gl.ext4"),
                      "--opengl-receipt", str(root / "gl.json"),
                      "--opengl-config-inventory-run", str(root / "gl46/run.json"),
                      "--gles32-config-inventory-run", str(root / "gles32/run.json")]
            options = campaign.parse_arguments(["--suite", "gl", *inputs])
            self.assertEqual(options.suite, "gl")
            # The GL pair is "all" without Vulkan, in the same order, so a
            # campaign planned with it completes from those two suites alone.
            self.assertEqual(
                list(campaign.SUITE_SELECTIONS["gl"]),
                [suite for suite in campaign.SUITE_SELECTIONS["all"]
                 if suite != "vulkan"])
            self.assertIsNone(options.vulkan_rootfs)
            for dropped in ("--opengl-rootfs", "--opengl-receipt",
                            "--opengl-config-inventory-run",
                            "--gles32-config-inventory-run"):
                index = inputs.index(dropped)
                trimmed = inputs[:index] + inputs[index + 2:]
                with self.subTest(dropped=dropped), \
                        contextlib.redirect_stderr(io.StringIO()), \
                        self.assertRaises(SystemExit):
                    campaign.parse_arguments(["--suite", "gl", *trimmed])
            definition = with_gles32(root, spec(root))
            pair = {**definition, "suites": list(campaign.SUITE_SELECTIONS["gl"])}
            keys = campaign.shards(pair)
            self.assertEqual({key.suite for key in keys}, {"gl46", "gles32"})
            self.assertEqual(len(keys),
                             len(campaign.shards(definition))
                             - sum(1 for key in campaign.shards(definition)
                                   if key.suite == "vulkan"))
            order = [key.suite for key in keys if key.index == 0]
            self.assertEqual(
                order,
                sorted(order, key=campaign.SUITE_ORDER.index))
            plan = campaign.plan_identity(pair)
            self.assertNotIn("vulkan", plan)
            self.assertEqual(plan["suites"], ["gl46", "gles32"])

    def test_cli_rejects_resource_and_selector_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            args = self.arguments(Path(temporary))
            self.assertEqual(campaign.parse_arguments(args).jobs, 8)
            self.assertIsNone(campaign.parse_arguments(args).jobs_file)
            self.assertEqual(campaign.parse_arguments(args).console_stall_busy_timeout, 3600)
            for extra in (["--jobs", "9"], ["--cpus", "8"], ["--memory", "8G"],
                          ["--case", "dEQP-VK.info.build"], ["--fraction-count", "16"],
                          ["--guest-timeout", "1"], ["--verify-only", "--dry-run"],
                          ["--console-stall-busy-timeout", "599"],
                          ["--console-stall-busy-timeout", "0"]):
                with self.subTest(extra=extra), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    campaign.parse_arguments(args + extra)

    def test_one_case_leaves_omit_only_provably_empty_fractions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            definition = spec(Path(temporary))
            keys = campaign.shards(definition)
            for run_index in (2, 3):
                self.assertEqual([key.index for key in keys if key.run_index == run_index], [0])
            self.assertEqual([key.index for key in keys if key.run_index == 7], list(range(8)))
            self.assertEqual(len({key.name() for key in keys}), len(keys))

    def test_xml_run_omission_reordering_or_egl_identity_relabel_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            definition = spec(Path(temporary))
            for mutation in ("omit", "reorder", "config"):
                bad = copy.deepcopy(definition)
                runs = bad["opengl"]["runs"]
                if mutation == "omit":
                    runs.pop(8)
                elif mutation == "reorder":
                    runs[7], runs[8] = runs[8], runs[7]
                else:
                    bad["opengl"]["egl_configs"].reverse()
                with self.subTest(mutation=mutation), self.assertRaisesRegex(gl.CampaignError, "run plan differs"):
                    campaign.shards(bad)

    def test_full_gl_campaign_rejects_legacy_scope_before_inventory_or_launch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            receipt = fixtures.zink_gl_derivative_receipt(gl.load_identity())
            _, contract = gl.load_zink_gl_derivative_receipt(receipt, gl.load_identity())
            receipt["schema_version"] = 3
            args = ["--suite", "opengl", "--campaign-dir", str(root / "campaign"),
                    "--emulator", definition["emulator"], "--opengl-rootfs", str(root / "gl.ext4"),
                    "--opengl-receipt", str(root / "gl.json"),
                    "--opengl-config-inventory-run", str(root / "inventory/run.json")]
            with mock.patch.object(campaign, "source_identity", return_value={}), mock.patch.object(
                gl, "load_derivative_receipt", return_value=(receipt, contract)
            ), mock.patch.object(gl, "load_config_inventory_evidence") as inventory, self.assertRaisesRegex(
                gl.CampaignError, "schema-4"
            ):
                campaign.build_spec(campaign.parse_arguments(args))
            inventory.assert_not_called()

    def test_zink_qualification_routes_only_through_valid_combined_receipt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            definition = spec(Path(temporary))
            args = campaign.child_command(definition, campaign.Shard("gl46", 0, 7), Path(temporary))[3:]
            self.assertTrue(gl.parse_arguments(args).qualification)
            for invalid in (args[:args.index("--rootfs-receipt")],
                            ["--qualification", "--leaf", "gl46", "--api", "gl46"]):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    gl.parse_arguments(invalid)
            path = Path(definition["opengl"]["receipt_path"])
            receipt = definition["opengl"]["receipt"]
            receipt["runner_oracle"]["successful"].append("Waiver")
            path.write_text(json.dumps(receipt))
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                gl.parse_arguments(args)

    def test_exact_aggregate_separates_unique_execution_and_ns_counts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            definition = spec(Path(temporary))

            def verify(_root, _spec, key, _attempt=None, **_kwargs):
                rows = results(key)
                return {"results": rows, "status_totals": gl.status_totals(rows)}

            with mock.patch.object(campaign, "validate_attempt", side_effect=verify), mock.patch.object(
                campaign.vk, "receipt_mandatory_cases", return_value=("dEQP-VK.mandatory0", "dEQP-VK.mandatory1")
            ):
                result = campaign.aggregate(
                    Path(temporary), definition, every_shard_passed(definition))
            self.assertFalse(result["formal_conformance"])
            vk = result["suites"]["vulkan"]
            self.assertEqual(vk["unique_case_count"], 10)
            self.assertEqual(vk["case_execution_count"], 24)
            self.assertEqual(vk["status_totals"], {"Pass": 16, "NotSupported": 8})
            self.assertEqual(vk["unique_status_totals"], {"Pass": 2, "NotSupported": 8})
            self.assertFalse(vk["all_pass"])
            self.assertEqual(result["suites"]["gl46"]["unique_case_count"], 62)
            self.assertEqual(result["suites"]["gl46"]["case_execution_count"], 112)
            self.assertEqual(result["suites"]["gl46"]["run_count"], 13)
            self.assertTrue(result["unwaived_runner_success"])
            self.assertEqual(result["suites"]["gl46"]["waived_cases"], [])
            self.assertEqual(vk["waived_cases"], [])

    def test_waived_gl_results_are_listed_and_clear_unwaived_success(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            definition = spec(Path(temporary))
            waived = {}

            def verify(_root, _spec, key, _attempt=None, **_kwargs):
                rows = results(key)
                if key.suite == "gl46" and key.run_index == 0 and key.index == 0:
                    case = next(iter(rows))
                    rows[case] = gl.WAIVED_STATUS
                    waived[case] = key.run_index
                return {"results": rows, "status_totals": gl.status_totals(rows)}

            with mock.patch.object(campaign, "validate_attempt", side_effect=verify), mock.patch.object(
                campaign.vk, "receipt_mandatory_cases", return_value=("dEQP-VK.mandatory0", "dEQP-VK.mandatory1")
            ):
                result = campaign.aggregate(
                    Path(temporary), definition, every_shard_passed(definition))
            self.assertEqual(len(waived), 1)
            case = next(iter(waived))
            self.assertFalse(result["unwaived_runner_success"])
            gl46 = result["suites"]["gl46"]
            self.assertEqual(gl46["waived_cases"], [case])
            self.assertFalse(gl46["all_pass"])
            self.assertEqual(gl46["status_totals"].get(gl.WAIVED_STATUS), 1)
            self.assertEqual([run["waived_cases"] for run in gl46["runs"] if run["index"] == 0], [[case]])
            self.assertTrue(all(run["waived_cases"] == [] for run in gl46["runs"] if run["index"] != 0))
            self.assertEqual(result["suites"]["vulkan"]["waived_cases"], [])

    def test_source_identity_freezes_the_waiver_list(self) -> None:
        files = campaign.source_identity()["files"]
        self.assertEqual(
            files["tests/integration/gpu/vk_gl_cts_waivers.json"],
            gl.sha256_file(gl.WAIVERS_PATH),
        )

    def test_source_identity_covers_modules_the_runners_import(self) -> None:
        files = campaign.source_identity()["files"]
        # run_gpu_guest.py imports its emulator cleanup from the support tree.
        cleanup = gl.gpu_guest.remove_emulator_run_directories
        support = Path(sys.modules[cleanup.__module__].__file__).resolve()
        self.assertEqual(
            files[str(support.relative_to(gl.REPO_ROOT))],
            gl.sha256_file(support),
        )

    def test_aggregate_rejects_omission_duplicate_order_and_mandatory_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            definition = spec(Path(temporary))
            for mutation in ("missing", "duplicate", "mandatory", "order"):
                def verify(_root, _spec, key, _attempt=None, **_kwargs):
                    rows = results(key)
                    if key.index == 7 and key.suite == "vulkan":
                        if mutation == "missing":
                            rows.pop("dEQP-VK.mandatory0")
                        elif mutation == "duplicate":
                            rows["dEQP-VK.case0"] = rows.pop("dEQP-VK.case7")
                        elif mutation == "mandatory":
                            rows["dEQP-VK.mandatory0"] = "NotSupported"
                    if mutation == "order" and key.suite == "gl46" and key.index == 0:
                        rows = dict(reversed(list(rows.items())))
                    return {"results": rows, "status_totals": gl.status_totals(rows)}
                with self.subTest(mutation=mutation), mock.patch.object(campaign, "validate_attempt", side_effect=verify), mock.patch.object(
                    campaign.vk, "receipt_mandatory_cases", return_value=("dEQP-VK.mandatory0", "dEQP-VK.mandatory1")
                ), self.assertRaises(gl.CampaignError):
                    campaign.aggregate(
                        Path(temporary), definition, every_shard_passed(definition))

    def test_gl_attempt_reparses_qpa_and_transport_and_provider(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            key = campaign.Shard("gl46", 0, 7)
            run = make_gl_attempt(root, definition, key)
            result = campaign.validate_attempt(root, definition, key)
            self.assertEqual(result["status_totals"], {"NotSupported": 1, "Pass": 1})
            path = run / "run.json"
            original = path.read_text()
            for field, value in (("kosmickrisp_sha256", "0" * 64), ("qualification", False),
                                 ("emulator_sha256", "0" * 64), ("cpus", 8),
                                 ("leaf", "gl46-gtf"), ("fraction", [1, 8])):
                changed = json.loads(original)
                changed[field] = value
                path.write_text(json.dumps(changed))
                with self.subTest(field=field), self.assertRaises(gl.CampaignError):
                    campaign.validate_attempt(root, definition, key)
            path.write_text(original)
            artifact = run / "artifacts/results.qpa"
            artifact.write_bytes(artifact.read_bytes().replace(b"Pass", b"Fail"))
            with self.assertRaisesRegex(gl.CampaignError, "transport differs"):
                campaign.validate_attempt(root, definition, key)

    def test_gl_attempt_verification_follows_the_plan_renderer(self) -> None:
        """Judge the probe against the renderer the plan launched.

        The final campaign launched its OpenGL shards with
        `--opengl-renderer virgl` and then verified them against Zink over
        Venus, so two clean shards were rejected for reporting exactly the
        stack they had been asked to run.
        """
        for renderer, other in (("virgl", "zink"), ("zink", "virgl")):
            with self.subTest(renderer=renderer):
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    definition = spec(root, renderer=renderer)
                    key = campaign.Shard("gl46", 0, 7)
                    make_gl_attempt(root, definition, key)
                    result = campaign.validate_attempt(root, definition, key)
                    self.assertEqual(result["status_totals"],
                                     {"NotSupported": 1, "Pass": 1})
                with tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    definition = spec(root, renderer=renderer)
                    key = campaign.Shard("gl46", 0, 7)
                    make_gl_attempt(root, definition, key,
                                    probe=RENDERER_PROBES[other])
                    with self.assertRaisesRegex(
                        gl.CampaignError, "renderer probe did not report"
                    ):
                        campaign.validate_attempt(root, definition, key)

    def test_gl46_case_subset_narrows_its_run_and_never_counts_as_a_shard(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            item = definition["opengl"]
            configs = tuple(
                gl.EglConfig(row["id"], tuple(row["surface_types"]))
                for row in item["egl_configs"]
            )
            run = item["runs"][0]
            cases = ("KHR-GL46.case.a", "KHR-GL46.case.b")
            narrowed = gl.opengl_matrix_run(
                item["receipt"], configs, item["egl_config_sha256"],
                run["egl_config_index"], run["configuration_index"], (3, 8),
                cases,
            )
            whole = gl.opengl_matrix_run(
                item["receipt"], configs, item["egl_config_sha256"],
                run["egl_config_index"], run["configuration_index"], (3, 8),
            )
            # Same shard, same leaf, fewer cases.
            self.assertEqual(narrowed.selection.leaf, whole.selection.leaf)
            self.assertEqual(narrowed.selection.fraction_index, 3)
            self.assertEqual(narrowed.selection.fraction_count, 8)
            self.assertEqual(narrowed.selection.exact_cases, cases)
            self.assertEqual(narrowed.selection.expected_count, len(cases))
            self.assertEqual(whole.selection.exact_cases, ())

            key = campaign.Shard("gl46", 0, 7)
            attempt = make_gl_attempt(root, definition, key)
            metadata = json.loads((attempt / "run.json").read_text())
            metadata["mode"] = "mustpass-matrix-case-subset"
            (attempt / "run.json").write_text(json.dumps(metadata))
            with self.assertRaises(gl.CampaignError):
                campaign.validate_attempt(root, definition, key)

    def test_resumed_gl_attempt_reparses_as_one_complete_shard(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            key = campaign.Shard("gl46", 0, 7)
            run = make_resumed_gl_attempt(root, definition, key)
            metadata = json.loads((run / "run.json").read_text())
            # The emulator exit was a session abort: nothing is host-recorded,
            # the console name is kept only as a diagnostic, and the case the
            # dead guest was running carries the re-run guest's own status.
            self.assertNotIn("host_recorded_results", metadata)
            self.assertIsNotNone(metadata["sessions"][0]["console_case"])
            self.assertIsNone(metadata["sessions"][1]["console_case"])
            self.assertEqual(metadata["sessions"][1]["resume_after"], 1)
            campaign.validate_attempt(root, definition, key)
            merged = run / "artifacts/results.qpa"
            original = merged.read_bytes()
            merged.write_bytes(original.replace(
                b'<Result StatusCode="Pass">result</Result>',
                b'<Result StatusCode="Fail">result</Result>', 1))
            with self.assertRaisesRegex(gl.CampaignError, "not reproducible"):
                campaign.validate_attempt(root, definition, key)
            merged.write_bytes(original)
            (run / f"{gl.SESSION_DIRECTORY}/01/{gl.guest_process.JOURNAL_NAME}").unlink()
            with self.assertRaises(gl.CampaignError):
                campaign.validate_attempt(root, definition, key)

    def test_artifact_symlinks_extra_files_and_limits_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            run = make_gl_attempt(root, definition, campaign.Shard("gl46", 0, 0))
            names = set(gl.SHARD_ARTIFACTS)
            with mock.patch.object(gl, "MAX_UNCOMPRESSED_ARTIFACT", 1), self.assertRaises(gl.CampaignError):
                campaign.read_artifacts(run, names)
            extra = run / "artifacts/extra.txt"
            extra.write_text("not part of the receipt")
            with self.assertRaises(gl.CampaignError):
                campaign.read_artifacts(run, names)
            extra.unlink()
            original = run / "artifacts/selected.txt"
            original.rename(root / "selected.txt")
            original.symlink_to(root / "selected.txt")
            with self.assertRaises(gl.CampaignError):
                campaign.read_artifacts(run, names)

    def test_pool_refills_without_waiting_for_five_slow_workers(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            definition = spec(Path(temporary))
            definition["suites"] = ["vulkan"]
            first = threading.Barrier(6)
            refilled = threading.Event()
            guard = threading.Lock()
            active = peak = 0

            def worker(_root, _spec, key, _cancelled, *_rest):
                nonlocal active, peak
                with guard:
                    active += 1
                    peak = max(peak, active)
                try:
                    if key.index < 6:
                        first.wait(timeout=WAIT_SECONDS)
                        if key.index != 0 and not refilled.wait(timeout=WAIT_SECONDS):
                            raise AssertionError("pool did not refill the freed slot")
                    elif key.index == 6:
                        refilled.set()
                    return {"cases": 1, "status_totals": {"Pass": 1}}
                finally:
                    with guard:
                        active -= 1

            complete, failures = {}, {}
            with mock.patch.object(campaign, "run_shard", side_effect=worker), contextlib.redirect_stdout(io.StringIO()):
                campaign.run_pending(Path(temporary), definition, list(campaign.shards(definition)),
                                     6, False, threading.Event(), complete, failures)
            self.assertTrue(refilled.is_set())
            self.assertEqual(peak, 6)
            self.assertEqual(len(complete), 8)
            self.assertFalse(failures)

    def test_a_jobs_file_raises_the_pool_above_its_floor_between_launches(self) -> None:
        """--jobs is the floor. The control file, re-read before every launch,
        may raise the owned guests up to MAX_JOBS; whatever else it holds
        (nothing, a word, a lower or an absurd number) means the floor or the
        cap, never a stopped pool."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            control = root / "jobs"
            self.assertEqual(campaign.current_jobs(None, 5), 5)
            self.assertEqual(campaign.current_jobs(control, 5), 5)
            for content, expected in (("seven\n", 5), ("", 5), ("3\n", 5),
                                      ("99\n", campaign.MAX_JOBS), (" 7 \n", 7)):
                control.write_text(content)
                self.assertEqual(campaign.current_jobs(control, 5), expected, content)
            definition = spec(root)
            definition["suites"] = ["vulkan"]
            control.write_text("4\n")
            guard = threading.Lock()
            full = threading.Event()
            active = peak = 0

            def worker(_root, _spec, key, _cancelled, *_rest):
                nonlocal active, peak
                with guard:
                    active += 1
                    peak = max(peak, active)
                    if active == 4:
                        full.set()
                try:
                    if key.index < 4 and not full.wait(timeout=WAIT_SECONDS):
                        raise AssertionError("the file did not raise the pool to four")
                    if key.index == 3:
                        control.write_text("1\n")
                    return {"cases": 1, "status_totals": {"Pass": 1}}
                finally:
                    with guard:
                        active -= 1

            complete, failures = {}, {}
            output = io.StringIO()
            with mock.patch.object(campaign, "run_shard", side_effect=worker), contextlib.redirect_stdout(output):
                campaign.run_pending(root, definition, list(campaign.shards(definition)),
                                     2, False, threading.Event(), complete, failures, jobs_file=control)
            self.assertEqual(peak, 4)
            self.assertEqual(len(complete), 8)
            self.assertFalse(failures)
            self.assertIn("Concurrency 2 -> 4", output.getvalue())
            self.assertIn("Concurrency 4 -> 2", output.getvalue())

    def test_a_failed_attempt_is_retried_beside_the_evidence_it_left(self) -> None:
        """A campaign ends when every shard has passed once, not when every
        shard passed on the first try. The failed attempt stays exactly where
        it is, so what it recorded is still readable afterwards."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            definition["suites"] = ["vulkan"]
            location = root / "campaign"
            location.mkdir()
            (location / "campaign.lock").touch()
            failed = campaign.Shard("vulkan", 0)
            (location / "campaign.json").write_text(json.dumps(
                campaign_record(definition, (failed.name(),))))
            first = campaign.attempt_path(location, failed)
            first.mkdir(parents=True)
            retried = []

            def worker(_root, _spec, key, _cancelled, attempt=None, *_rest):
                retried.append((key.name(), attempt))
                return {"cases": 1, "status_totals": {"Pass": 1}}

            def resolved(_root, _spec, key, attempt=None, **_kwargs):
                if attempt == first:
                    raise gl.CampaignError("leaf runner exited 1")
                return {"cases": 1, "shard": key.name(), "attempt": str(attempt)}

            with mock.patch.object(campaign, "build_spec", return_value=definition), \
                    mock.patch.object(campaign, "run_shard", side_effect=worker), \
                    mock.patch.object(campaign, "check_current_inputs"), \
                    mock.patch.object(campaign, "validate_attempt", side_effect=resolved), \
                    mock.patch.object(campaign, "aggregate", return_value={"complete": True}), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(campaign.main(self.arguments(root)), 0)

            names = [name for name, _ in retried]
            self.assertIn(failed.name(), names)
            second = dict(retried)[failed.name()]
            self.assertEqual(second, location / "retries" / failed.name() / "0002")
            self.assertTrue(first.is_dir())
            catalog = campaign.load_json(location / "campaign.json")["attempts"]
            self.assertEqual([entry["path"] for entry in catalog[failed.name()]],
                             [f"shards/{failed.name()}",
                              f"retries/{failed.name()}/0002"])

    def test_a_new_emulator_opens_a_generation_and_keeps_passed_shards(self) -> None:
        """Swapping the emulator must cost only the shards that have not passed
        yet. The attempt that did pass keeps being judged against the inputs
        that produced it, which is what makes spanning generations honest."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            definition["suites"] = ["vulkan"]
            location = root / "campaign"
            location.mkdir()
            done = campaign.Shard("vulkan", 0)
            campaign.attempt_path(location, done).mkdir(parents=True)
            (location / "campaign.json").write_text(json.dumps(
                campaign_record(definition, (done.name(),), state="running")))
            replaced = copy.deepcopy(definition)
            replaced["emulator_sha256"] = "f" * 64
            judged = []

            def resolved(_root, generation, key, attempt=None, **_kwargs):
                judged.append((key.name(), generation["emulator_sha256"]))
                return {"cases": 1}

            def worker(_root, _spec, key, _cancelled, *_rest):
                return {"cases": 1}

            with mock.patch.object(campaign, "build_spec", return_value=replaced), \
                    mock.patch.object(campaign, "validate_attempt", side_effect=resolved), \
                    mock.patch.object(campaign, "run_shard", side_effect=worker), \
                    mock.patch.object(campaign, "check_current_inputs"), \
                    mock.patch.object(campaign, "aggregate", return_value={"complete": True}), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(campaign.main(self.arguments(root)), 0)

            stored = campaign.load_json(location / "campaign.json")
            self.assertEqual(stored["schema_version"], campaign.SCHEMA_VERSION)
            self.assertEqual([item["emulator_sha256"] for item in stored["generations"]],
                             [definition["emulator_sha256"], "f" * 64])
            # The shard that already passed is reparsed against generation 0.
            self.assertIn((done.name(), definition["emulator_sha256"]), judged)
            self.assertEqual(len(stored["attempts"][done.name()]), 1)
            self.assertEqual(stored["attempts"]["vulkan-01of08"][0]["generation"], 1)

    def test_stuck_wait_reporting_only_reaches_the_generation_that_asked(self) -> None:
        """The generated guest script is embedded in every recorded harness
        command, so enabling this for a new generation must leave the commands
        of earlier attempts exactly as they were."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            shard = campaign.Shard("vulkan", 0)
            output = root / "driver-output"
            before = campaign.child_command(definition, shard, output)
            self.assertNotIn("--relax-diagnostics", before)

            asked = dict(definition, relax_diagnostics=True)
            after = campaign.child_command(asked, shard, output)
            self.assertIn("--relax-diagnostics", after)

            # An OpenGL shard is untouched either way.
            gl_shard = campaign.Shard("gl46", 0, 0)
            self.assertEqual(campaign.child_command(definition, gl_shard, output),
                             campaign.child_command(asked, gl_shard, output))

    def test_a_changed_plan_is_a_different_campaign(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            definition["suites"] = ["vulkan"]
            location = root / "campaign"
            location.mkdir()
            (location / "campaign.json").write_text(json.dumps(campaign_record(definition)))
            replaced = copy.deepcopy(definition)
            replaced["page_profile"] = "16k"
            with mock.patch.object(campaign, "build_spec", return_value=replaced), \
                    mock.patch.object(campaign, "run_pending") as launch, \
                    self.assertRaises(gl.CampaignError):
                campaign.main(self.arguments(root))
            launch.assert_not_called()

    def test_a_campaign_of_another_schema_is_refused_before_launch(self) -> None:
        """A schema-1 record predates retries and generations; nothing reads it."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            location = root / "campaign"
            location.mkdir()
            (location / "campaign.json").write_text(json.dumps({
                "schema_version": 1, "kind": campaign.KIND, "spec": definition,
                "attempted_shards": [],
            }))
            before = (location / "campaign.json").read_bytes()
            with mock.patch.object(campaign, "build_spec", return_value=definition), \
                    mock.patch.object(campaign, "run_pending") as launch, \
                    self.assertRaisesRegex(gl.CampaignError, "unsupported campaign schema"):
                campaign.main(self.arguments(root))
            launch.assert_not_called()
            self.assertEqual((location / "campaign.json").read_bytes(), before)

    def test_a_shard_left_running_is_adopted_rather_than_relaunched(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            definition["suites"] = ["vulkan"]
            location = root / "campaign"
            location.mkdir()
            live = campaign.Shard("vulkan", 0)
            attempt = campaign.attempt_path(location, live)
            attempt.mkdir(parents=True)
            (attempt / "attempt.json").write_text(json.dumps({
                "shard": live.name(), "state": "started",
                "pid": os.getpid(), "pgid": os.getpgid(0),
                "guest_ownership_token": "t" * 32,
            }))
            record = campaign_record(definition, (live.name(),), state="running")
            passed, adoptions, failures, pending = campaign.classify_attempts(
                location, definition, record)
            self.assertFalse(passed)
            self.assertFalse(failures)
            self.assertNotIn(live, pending)
            self.assertEqual([(shard.name(), path) for shard, path, _ in adoptions],
                             [(live.name(), attempt)])

    def test_an_adopted_leaf_is_judged_by_the_status_it_left_behind(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            shard = campaign.Shard("vulkan", 0)
            attempt = campaign.attempt_path(root, shard)
            attempt.mkdir(parents=True)
            (attempt / "runner.log").write_bytes(b"log\n")
            (attempt / "attempt.json").write_text(json.dumps({
                "shard": shard.name(), "state": "started", "pgid": 424242,
                "guest_ownership_token": "t" * 32,
            }))
            (attempt / campaign.LEAF_EXIT_NAME).write_text(json.dumps(
                {"token": "t" * 32, "return_code": 0}))
            with mock.patch.object(campaign, "group_exists", return_value=False), \
                    mock.patch.object(campaign, "validate_attempt",
                                      return_value={"cases": 1}):
                result = campaign.adopt_attempt(
                    root, definition, shard, attempt,
                    threading.Event(), threading.Event())
            self.assertEqual(result, {"cases": 1})
            stored = campaign.load_json(attempt / "attempt.json")
            self.assertEqual(stored["state"], "finished")
            self.assertTrue(stored["adopted"])
            self.assertEqual(stored["return_code"], 0)

            # A leaf that failed is not laundered by being adopted.
            (attempt / campaign.LEAF_EXIT_NAME).write_text(json.dumps(
                {"token": "t" * 32, "return_code": 1}))
            with mock.patch.object(campaign, "group_exists", return_value=False), \
                    self.assertRaisesRegex(gl.CampaignError, "adopted leaf exited 1"):
                campaign.adopt_attempt(root, definition, shard, attempt,
                                       threading.Event(), threading.Event())

    def test_detaching_hands_over_the_running_leaf_instead_of_stopping_it(self) -> None:
        """Detaching exists so a restart keeps hours of guest work. The leaf is
        deliberately left running and its record left in the state the next
        pool recognises."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            shard = campaign.Shard("vulkan", 0)
            attempt = campaign.attempt_path(root, shard)
            detaching = threading.Event()
            process = mock.Mock(pid=999999)
            process.poll.side_effect = lambda: detaching.set()
            with mock.patch.object(campaign.subprocess, "Popen", return_value=process), \
                    mock.patch.object(campaign, "check_current_inputs"), \
                    mock.patch.object(campaign, "stop_owned_group") as stop, \
                    mock.patch.object(campaign, "group_exists", return_value=True), \
                    self.assertRaises(campaign.CampaignDetached):
                campaign.run_shard(root, definition, shard, threading.Event(),
                                   attempt, detaching)
            stop.assert_not_called()
            self.assertEqual(
                campaign.load_json(attempt / "attempt.json")["state"], "started")

    def test_an_attempt_limit_stops_a_shard_from_retrying_forever(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            definition["suites"] = ["vulkan"]
            location = root / "campaign"
            location.mkdir()
            failed = campaign.Shard("vulkan", 0)
            (location / "campaign.json").write_text(json.dumps(
                campaign_record(definition, (failed.name(),))))
            campaign.attempt_path(location, failed).mkdir(parents=True)
            with mock.patch.object(campaign, "build_spec", return_value=definition), \
                    mock.patch.object(campaign, "run_pending") as launch, \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(
                    campaign.main(self.arguments(root) + ["--max-attempts", "1"]), 1)
            pending = launch.call_args.args[2]
            self.assertNotIn(failed, pending)

    def test_missing_attempt_directory_cannot_be_retried(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            location = root / "campaign"
            location.mkdir()
            (location / "campaign.json").write_text(json.dumps(
                campaign_record(definition, ("vulkan-00of08",), state="running")))
            with mock.patch.object(campaign, "build_spec", return_value=definition), mock.patch.object(
                campaign, "run_pending"
            ) as launch, self.assertRaisesRegex(gl.CampaignError, "launch catalog disagree"):
                campaign.main(self.arguments(root))
            launch.assert_not_called()

    def test_launch_catalog_is_persisted_before_worker_and_resume_skips_finished(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            definition["suites"] = ["vulkan"]
            location = root / "campaign"
            location.mkdir()
            first = campaign.Shard("vulkan", 0)
            campaign.attempt_path(location, first).mkdir(parents=True)
            (location / "campaign.json").write_text(json.dumps(
                campaign_record(definition, (first.name(),), state="running")))
            seen = []

            def worker(_root, _spec, key, _cancelled, *_rest):
                catalog = campaign.load_json(location / "campaign.json")["attempts"]
                self.assertIn(key.name(), catalog)
                seen.append(key.name())
                return {"cases": 1, "status_totals": {"Pass": 1}}

            with mock.patch.object(campaign, "build_spec", return_value=definition), mock.patch.object(
                campaign, "validate_attempt", return_value={"cases": 1}
            ), mock.patch.object(campaign, "run_shard", side_effect=worker), mock.patch.object(
                campaign, "check_current_inputs"
            ), mock.patch.object(campaign, "aggregate", return_value={"complete": True}), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(campaign.main(self.arguments(root)), 0)
            self.assertEqual(set(seen), {key.name() for key in campaign.shards(definition)} - {first.name()})
            self.assertEqual(len(campaign.load_json(location / "campaign.json")["attempts"]), 8)

    def test_pool_fail_fast_keep_going_and_prelaunch_cancellation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            definition = spec(Path(temporary))
            definition["suites"] = ["vulkan"]
            for keep_going, pre_cancelled in ((False, False), (True, False), (False, True)):
                cancelled = threading.Event()
                if pre_cancelled:
                    cancelled.set()
                complete, failures, launched = {}, {}, []

                def worker(_root, _spec, key, _cancelled, *_rest):
                    launched.append(key.index)
                    if key.index == 0:
                        raise gl.CampaignError("fixture leaf failed")
                    return {"cases": 1}

                with self.subTest(keep_going=keep_going, cancelled=pre_cancelled), mock.patch.object(
                    campaign, "run_shard", side_effect=worker
                ), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    campaign.run_pending(Path(temporary), definition, list(campaign.shards(definition)),
                                         1, keep_going, cancelled, complete, failures)
                self.assertEqual(launched, [] if pre_cancelled else list(range(8)) if keep_going else [0])

    def test_dry_run_creates_no_campaign_or_process(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            with mock.patch.object(campaign, "build_spec", return_value=definition), mock.patch.object(
                campaign.subprocess, "Popen"
            ) as launch, contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(campaign.main(self.arguments(root) + ["--dry-run"]), 0)
            self.assertEqual(len(output.getvalue().splitlines()), 98)
            launch.assert_not_called()
            self.assertFalse((root / "campaign").exists())

    def test_changed_spec_and_verify_only_never_launch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            location = root / "campaign"
            location.mkdir()
            (location / "campaign.lock").touch()
            (location / "campaign.json").write_text(json.dumps(campaign_record(definition)))
            before = (location / "campaign.json").read_bytes()
            for changed in (False, True):
                if changed:
                    definition["emulator_sha256"] = "f" * 64
                with mock.patch.object(campaign, "build_spec", return_value=definition), mock.patch.object(
                    campaign, "run_pending"
                ) as launch, self.assertRaises(gl.CampaignError):
                    campaign.main(self.arguments(root) + ["--verify-only"])
                launch.assert_not_called()
                self.assertEqual((location / "campaign.json").read_bytes(), before)

    def test_owned_group_uses_bounded_term_kill_fallback(self) -> None:
        process = mock.Mock(pid=123456)
        process.poll.return_value = None
        alive = True

        def send(pgid, sig):
            nonlocal alive
            self.assertEqual(pgid, process.pid)
            if sig == signal.SIGKILL:
                alive = False

        with mock.patch.object(campaign, "group_exists", side_effect=lambda _pid: alive), mock.patch.object(
            campaign.os, "killpg", side_effect=send
        ) as kill:
            campaign.stop_owned_group(process, grace=0, term_grace=0, kill_grace=0.1)
        self.assertEqual(kill.call_args_list, [mock.call(process.pid, signal.SIGTERM),
                                             mock.call(process.pid, signal.SIGKILL)])
        process.send_signal.assert_called_once_with(signal.SIGINT)

    def test_permission_denial_never_proves_process_group_gone(self) -> None:
        process = mock.Mock(pid=123456)
        process.poll.return_value = 0
        with mock.patch.object(campaign.os, "killpg", side_effect=PermissionError):
            self.assertTrue(campaign.group_exists(process.pid))
            with self.assertRaisesRegex(gl.CampaignError, "did not terminate"):
                campaign.stop_owned_group(process, grace=0, term_grace=0, kill_grace=0)

    def test_vulkan_attempt_requires_recorded_candidate_emulator(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            key = campaign.Shard("vulkan", 0)
            attempt = campaign.attempt_path(root, key)
            run = attempt / "driver-output/fixture-run"
            run.mkdir(parents=True)
            (run / "console.log").write_text("fixture console")
            (attempt / "runner.log").write_text("fixture runner")
            (attempt / "attempt.json").write_text(json.dumps({
                **ownership_record(attempt),
                "shard": key.name(), "state": "finished", "return_code": 0, "error": None,
                "command": campaign.child_command(definition, key, attempt / "driver-output"),
                "runner_log_sha256": gl.sha256_file(attempt / "runner.log"),
            }))
            metadata = {
                "qualification": True, "formal_conformance": False, "engine": "jit",
                "memory": "4G", "cpus": 4, "page_profile": "4k",
                "kosmickrisp_sha256": definition["kosmickrisp_sha256"],
                "git": definition["source"]["git"],
                "console_sha256": gl.sha256_file(run / "console.log"),
            }
            for digest in (None, "f" * 64):
                if digest is not None:
                    metadata["emulator_sha256"] = digest
                (run / "run.json").write_text(json.dumps(metadata))
                with self.subTest(digest=digest), self.assertRaisesRegex(gl.CampaignError, "emulator_sha256"):
                    campaign.validate_attempt(root, definition, key)

    def test_metadata_failure_after_spawn_still_reaps_owned_group(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            process = mock.Mock(pid=123456, returncode=0)
            actual_write = campaign.write_json
            writes = 0

            def failing_write(path, value):
                nonlocal writes
                writes += 1
                if writes == 2:
                    raise OSError("fixture disk full after child creation")
                actual_write(path, value)

            with mock.patch.object(campaign, "check_current_inputs"), mock.patch.object(
                campaign.subprocess, "Popen", return_value=process
            ), mock.patch.object(campaign, "stop_owned_group") as stop, mock.patch.object(
                campaign, "write_json", side_effect=failing_write
            ), self.assertRaisesRegex(OSError, "disk full"):
                campaign.run_shard(root, definition, campaign.Shard("vulkan", 0), threading.Event())
            stop.assert_called_once()
            self.assertEqual(stop.call_args.args, (process,))
            self.assertEqual(stop.call_args.kwargs["ownership"][0],
                             root / "shards/vulkan-00of08" / gl.guest_process.JOURNAL_NAME)

    def test_emulator_failure_cancels_keep_going_and_reaps_leaf(self) -> None:
        for reason in ("stalled", "dirty-shutdown", None):
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                definition = spec(root)
                definition["suites"] = ["vulkan"]
                launched = []
                process = mock.Mock(pid=123456, returncode=1)
                process.poll.return_value = 1

                def spawn(*_args, **kwargs):
                    launched.append(kwargs["env"])
                    if reason is not None:
                        setting = json.loads(kwargs["env"][gl.CAMPAIGN_FAILURE_ENV])
                        Path(setting["path"]).write_text(json.dumps({
                            "token": setting["token"], "reason": reason,
                        }))
                    return process

                with mock.patch.object(campaign, "check_current_inputs"), mock.patch.object(
                    campaign.subprocess, "Popen", side_effect=spawn
                ), mock.patch.object(campaign, "group_exists", return_value=False), mock.patch.object(
                    campaign, "stop_owned_group"
                ) as stop, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    campaign.run_pending(root, definition, list(campaign.shards(definition)),
                                         1, True, threading.Event(), {}, {})
                self.assertEqual(len(launched), 1 if reason else 8)
                self.assertEqual(stop.call_count, len(launched))
                record = json.loads((campaign.attempt_path(root, campaign.Shard("vulkan", 0)) /
                                     "attempt.json").read_text())
                self.assertEqual(record["state"], "failed")
                self.assertIn(f"emulator failure: {reason}" if reason else "leaf runner exited 1",
                              record["error"])

    def test_dirty_shutdown_notification_excludes_clean_cts_timeout(self) -> None:
        for dirty in (False, True):
            with self.subTest(dirty=dirty), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                notification = root / "emulator-failure.json"
                transcript = gl.DARWIN_CLEAN_STOP + (gl.DIRTY_DRM_SHUTDOWN if dirty else "")
                frames = fixtures.framed_artifact("summary.txt", b"cts_rc=124\n")
                process = mock.Mock(returncode=0)
                with mock.patch.dict(os.environ, {gl.CAMPAIGN_FAILURE_ENV: json.dumps({
                    "path": str(notification), "token": "test-token",
                })}), mock.patch.object(gl.guest_process, "managed_child", return_value=contextlib.nullcontext(process)), mock.patch.object(
                    gl, "wait_for_guest", return_value=(transcript, frames)
                ):
                    if dirty:
                        with self.assertRaisesRegex(gl.CampaignError, "emulator failure: dirty-shutdown"):
                            gl.run_guest_session(["fixture"], root / "console.log", root / "transport.log",
                                                 root / "artifact.sock", guest_timeout=1)
                    else:
                        result = gl.run_guest_session(["fixture"], root / "console.log", root / "transport.log",
                                                      root / "artifact.sock", guest_timeout=1)
                        self.assertEqual(result.outcome, "completed")
                self.assertEqual(notification.exists(), dirty)

    def test_host_deadline_cancels_even_after_owned_cleanup_succeeds(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            cancelled = threading.Event()
            process = mock.Mock(pid=123456, returncode=None)
            process.poll.return_value = None
            with mock.patch.object(campaign, "check_current_inputs"), mock.patch.object(
                campaign.subprocess, "Popen", return_value=process
            ), mock.patch.object(campaign.time, "monotonic", side_effect=[0, 10**9]), mock.patch.object(
                campaign, "stop_owned_group"
            ) as stop, self.assertRaisesRegex(gl.CampaignError, "leaf exceeded host deadline"):
                campaign.run_shard(root, definition, campaign.Shard("vulkan", 0), cancelled)
            self.assertTrue(cancelled.is_set())
            stop.assert_called_once()

    def test_stalled_leaf_retains_prefix_before_notifying_pool(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            notification = root / "emulator-failure.json"
            console = root / "console.log"
            transport = root / gl.ARTIFACT_TRANSPORT_LOG
            payload = b"retained partial QPA"
            frames = fixtures.framed_artifact("results.qpa", payload)
            process = mock.Mock(returncode=-2)
            reaped = False

            @contextlib.contextmanager
            def managed(*_args, **_kwargs):
                nonlocal reaped
                try:
                    yield process
                finally:
                    reaped = True

            def notify(reason):
                self.assertTrue(reaped)
                self.assertEqual(transport.read_text(), frames)
                self.assertEqual((root / "artifacts/results.qpa").read_bytes(), payload)
                actual_notify(reason)

            actual_notify = gl.report_campaign_emulator_failure
            with mock.patch.dict(os.environ, {gl.CAMPAIGN_FAILURE_ENV: json.dumps({
                "path": str(notification), "token": "test-token",
            })}), mock.patch.object(gl.guest_process, "managed_child", managed), mock.patch.object(
                gl, "wait_for_guest", side_effect=gl.GuestSessionInterrupted(
                    "shutdown timeout", reason="stalled", transcript="CTS complete",
                    artifact_transcript=frames,
                )
            ), mock.patch.object(gl, "report_campaign_emulator_failure", side_effect=notify), self.assertRaisesRegex(
                gl.CampaignError, "emulator failure: stalled"
            ):
                gl.run_guest_session(["fixture"], console, transport, root / "artifact.sock",
                                     guest_timeout=1)
            self.assertEqual(json.loads(notification.read_text()),
                             {"token": "test-token", "reason": "stalled"})

    def test_a_recorded_shutdown_stall_waives_only_the_stop_marker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            run = Path(temporary)
            console = f"{gl.SESSION_DIRECTORY}/00/console.log"
            (run / console).parent.mkdir(parents=True)
            (run / console).write_text("guest delivered its evidence and never stopped\n")
            metadata = {"sessions": [{"console": console, "shutdown_stall": True}]}
            final, clean_stop, stalled = campaign.check_session_ownership(
                run, metadata, "token")
            self.assertEqual(final, run / console)
            self.assertFalse(clean_stop)
            self.assertTrue(stalled)
            # The same session without a recorded stall still owes its marker.
            metadata["sessions"][0]["shutdown_stall"] = False
            self.assertEqual(
                campaign.check_session_ownership(run, metadata, "token")[1:], (True, False)
            )

    def test_delivered_shutdown_stall_keeps_the_pool_running(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            notification = root / "emulator-failure.json"
            frames = fixtures.framed_artifact("summary.txt", b"cts_rc=0\n")
            process = mock.Mock(returncode=-15)
            with mock.patch.dict(os.environ, {gl.CAMPAIGN_FAILURE_ENV: json.dumps({
                "path": str(notification), "token": "test-token",
            })}), mock.patch.object(
                gl.guest_process, "managed_child",
                return_value=contextlib.nullcontext(process),
            ), mock.patch.object(
                gl, "wait_for_guest", side_effect=gl.GuestSessionInterrupted(
                    "guest did not stop", reason="stalled",
                    transcript="CTS complete", artifact_transcript=frames,
                    delivered=True,
                )
            ), contextlib.redirect_stderr(io.StringIO()) as reported:
                result = gl.run_guest_session(
                    ["fixture"], root / "console.log", root / "transport.log",
                    root / "artifact.sock", guest_timeout=1,
                )
            # The evidence was complete before the guest refused to power off:
            # the shard keeps it and its peers keep running.
            self.assertTrue(result.shutdown_stall)
            self.assertEqual(result.outcome, "stalled")
            self.assertIn("shutdown stall", reported.getvalue())
            self.assertFalse(notification.exists())
            self.assertEqual(result.artifacts["summary.txt"], b"cts_rc=0\n")

    def test_missing_nested_reap_ack_cancels_pool_after_leaf_group_exits(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            definition = spec(root)
            cancelled = threading.Event()
            process = mock.Mock(pid=123456, returncode=1)
            process.poll.return_value = 1

            def spawn(*_args, **kwargs):
                ownership = json.loads(kwargs["env"][gl.guest_process.OWNERSHIP_ENV])
                Path(ownership["path"]).write_text(json.dumps({
                    "event": "starting", "token": ownership["token"], "owner_pid": 123457,
                }) + "\n")
                return process

            with mock.patch.object(campaign, "check_current_inputs"), mock.patch.object(
                campaign.subprocess, "Popen", side_effect=spawn
            ), mock.patch.object(campaign, "group_exists", return_value=False), self.assertRaisesRegex(
                gl.CampaignError, "incomplete"
            ):
                campaign.run_shard(root, definition, campaign.Shard("vulkan", 0), cancelled)
            self.assertTrue(cancelled.is_set())
            record = json.loads((root / "shards/vulkan-00of08/attempt.json").read_text())
            self.assertEqual(record["state"], "failed")
            self.assertIn("incomplete", record["error"])

    @unittest.skipUnless(os.name == "posix", "process sessions require POSIX")
    def test_real_owned_session_cleanup_preserves_unrelated_process(self) -> None:
        owned = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], start_new_session=True)
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"], start_new_session=True)
        try:
            campaign.stop_owned_group(owned, grace=0.1, term_grace=0.1, kill_grace=1)
            self.assertIsNotNone(owned.poll())
            self.assertFalse(campaign.group_exists(owned.pid))
            self.assertIsNone(unrelated.poll())
        finally:
            for process in (owned, unrelated):
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=WAIT_SECONDS)

    @unittest.skipUnless(os.name == "posix", "process sessions require POSIX")
    def test_real_cleanup_reaps_descendant_after_leaf_exits(self) -> None:
        process = subprocess.Popen([
            sys.executable, "-c",
            "import subprocess,sys; "
            "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)']); "
            "print(child.pid,flush=True)",
        ], start_new_session=True, stdout=subprocess.PIPE)
        try:
            self.assertTrue(process.stdout.readline().strip().isdigit())
            process.wait(timeout=WAIT_SECONDS)
            campaign.stop_owned_group(process, grace=0, term_grace=1, kill_grace=2)
            self.assertFalse(campaign.group_exists(process.pid))
        finally:
            process.stdout.close()
            if campaign.group_exists(process.pid):
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=WAIT_SECONDS)


if __name__ == "__main__":
    unittest.main()
