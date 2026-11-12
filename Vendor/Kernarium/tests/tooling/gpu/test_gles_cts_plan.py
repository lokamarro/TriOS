#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "integration/gpu"))
import gles_cts_plan as plan  # noqa: E402
import opengl_cts_plan as gl_plan  # noqa: E402
import run_vk_gl_cts as common  # noqa: E402
import test_run_vk_gl_cts as fixtures  # noqa: E402


CONFIGS = [
    {"id": 1, "surface_types": ["window", "pixmap", "pbuffer"]},
    {"id": 2, "surface_types": ["pbuffer", "pixmap"]},
    {"id": 5, "surface_types": ["pixmap"]},
]


def attribute_row(identifier: int, surfaces: list[str], samples: int = 0,
                  conformant: bool = True) -> dict:
    return {
        "id": identifier, "red": 8, "green": 8, "blue": 8, "alpha": 8,
        "depth": 24, "stencil": 8, "samples": samples,
        "renderable": ["EGL_OPENGL_ES2_BIT", plan.ES3_BIT],
        "conformant": (["EGL_OPENGL_ES2_BIT", plan.ES3_BIT] if conformant
                       else ["EGL_OPENGL_ES2_BIT"]),
        "surface_types": surfaces,
    }


# The EGL attribute table the ES 3.2 inventory records. Its conformant,
# single-sample subset is exactly CONFIGS; the multisample row is reachable
# only through an AOSP record, and the non-conformant row through neither.
ATTRIBUTES = [
    attribute_row(1, ["window", "pixmap", "pbuffer"]),
    attribute_row(2, ["pixmap", "pbuffer"]),
    attribute_row(3, ["window", "pbuffer"], samples=4),
    attribute_row(4, ["window"], conformant=False),
    attribute_row(5, ["pixmap"]),
]


class GlesCtsPlanTests(unittest.TestCase):
    def setUp(self) -> None:
        self.manifest = common.load_gles32_matrix()

    def test_repository_documents_expand_the_official_es32_run_table(self) -> None:
        # getTestRunsForES admits every supported API in the NoContext and
        # Khronos ES collections and only the exact API in Single Config.
        rows = self.manifest["configurations"]
        self.assertEqual(
            [(row["collection"], row["leaf_id"], row["use_for_first_egl_config"],
              row["surface_width"], row["surface_height"], row["base_seed"],
              row["fbo_config"], row["aosp_config"]) for row in rows],
            [
                ("no-context", "gles2-khr-noctx-main", True, 64, 64, 1, None, None),
                ("no-context", "gles32-khr-noctx-main", True, 64, 64, 1, None, None),
                ("single", "gles32-khr-single", True, 64, 64, 1, None, None),
                ("main", "gles2-khr-main", True, 64, 64, 1, None, None),
                ("main", "gles2-khr-main", False, 64, 64, 1, None, None),
                ("main", "gles3-khr-main", True, 64, 64, 1, None, None),
                ("main", "gles3-khr-main", False, 64, 64, 1, None, None),
                ("main", "gles31-khr-main", True, 64, 64, 1, None, None),
                ("main", "gles31-khr-main", False, 64, 64, 1, None, None),
                ("main", "gles32-khr-main", True, 64, 64, 1, None, None),
                ("main", "gles32-khr-main", True, 113, 47, 2, None, None),
                ("main", "gles32-khr-main", True, 64, -1, 3, "rgba8888d24s8", None),
                ("main", "gles32-khr-main", True, -1, 64, 3, "rgba8888d24s8", None),
                ("main", "gles32-khr-main", False, 64, 64, 1, None, None),
                ("main", "gles32-khr-main", False, 113, 47, 2, None, None),
                ("main", "gles32-khr-glesext", True, 64, 64, 1, None, None),
                # The AOSP collections come last so they renumber nothing.
                ("aosp-es", "gles2-main", True, 256, 256, None, None,
                 "rgba8888d24s8ms0"),
                ("aosp-es", "gles3-main", True, 256, 256, None, None,
                 "rgba8888d24s8ms0"),
                ("aosp-es", "gles3-multisample", True, 256, 256, None, None,
                 "rgba8888d24s8ms4"),
                ("aosp-es", "gles31-main", True, 256, 256, None, None,
                 "rgba8888d24s8ms0"),
                ("aosp-es", "gles31-multisample", True, 256, 256, None, None,
                 "rgba8888d24s8ms4"),
                ("aosp-egl", "egl-main", True, 256, 256, None, None,
                 "rgba8888d24s8ms0"),
            ],
        )
        self.assertEqual(self.manifest["leaf_count"], 14)
        self.assertNotIn("gles31-khr-single", {row["leaf_id"] for row in rows})

    def test_runs_follow_the_configuration_list_and_its_surfaces(self) -> None:
        runs = plan.expand_runs(self.manifest, CONFIGS, ATTRIBUTES)
        # Three one-time runs on the first config, eight first-config rows,
        # five rows on each remaining config, then the six AOSP runs.
        self.assertEqual(len(runs), 3 + 8 + 5 * (len(CONFIGS) - 1) + 6)
        self.assertEqual([run["egl_config_id"] for run in runs[:11]], [1] * 11)
        self.assertEqual([run["egl_config_id"] for run in runs[11:16]], [2] * 5)
        self.assertEqual([run["egl_config_id"] for run in runs[16:21]], [5] * 5)
        self.assertEqual(
            [run["surface_type"] for run in runs[:11]],
            ["window"] * 8 + ["fbo", "fbo", "window"],
        )
        self.assertEqual({run["surface_type"] for run in runs[11:16]}, {"pbuffer"})
        self.assertEqual({run["surface_type"] for run in runs[16:21]}, {"pixmap"})
        self.assertEqual([run["index"] for run in runs], list(range(len(runs))))
        self.assertEqual({run["config_scope"] for run in runs[:21]}, {"conformant"})
        self.assertEqual({run["config_scope"] for run in runs[21:]}, {"aosp"})

    def test_aosp_runs_resolve_their_named_gl_configuration(self) -> None:
        runs = plan.expand_runs(self.manifest, CONFIGS, ATTRIBUTES)
        rows = self.manifest["configurations"]
        # rgba8888d24s8ms0 takes the first compatible AOSP config and
        # rgba8888d24s8ms4 the multisample one the conformant list excludes.
        self.assertEqual(
            [(rows[run["configuration_index"]]["leaf_id"], run["egl_config_id"],
              run["surface_type"]) for run in runs[21:]],
            [("gles2-main", 1, "window"), ("gles3-main", 1, "window"),
             ("gles3-multisample", 3, "window"), ("gles31-main", 1, "window"),
             ("gles31-multisample", 3, "window"), ("egl-main", 1, "none")],
        )
        # An AOSP command line names its GL config and, outside the EGL run, a
        # surface type; it never seeds the run or names an EGL config id.
        self.assertEqual(
            plan.runner_arguments(self.manifest, runs[21], "/run/selected.txt"),
            ["--deqp-caselist-file=/run/selected.txt",
             "--deqp-screen-rotation=unspecified",
             "--deqp-surface-width=256", "--deqp-surface-height=256",
             "--deqp-watchdog=disable",
             "--deqp-gl-config-name=rgba8888d24s8ms0",
             "--deqp-surface-type=window"],
        )
        self.assertEqual(
            plan.runner_arguments(self.manifest, runs[26], "/run/selected.txt")[-1],
            "--deqp-gl-config-name=rgba8888d24s8ms0",
        )

    def test_the_aosp_mapping_needs_the_authenticated_attribute_table(self) -> None:
        with self.assertRaisesRegex(plan.PlanError, "no EGL configuration attributes"):
            plan.expand_runs(self.manifest, CONFIGS)
        without_multisample = [row for row in ATTRIBUTES if row["samples"] == 0]
        with self.assertRaisesRegex(plan.PlanError, "rgba8888d24s8ms4"):
            plan.expand_runs(self.manifest, CONFIGS, without_multisample)
        disagreeing = [dict(row, samples=4) if row["id"] == 5 else row
                       for row in ATTRIBUTES]
        with self.assertRaisesRegex(plan.PlanError, "reproduce the conformant list"):
            plan.expand_runs(self.manifest, CONFIGS, disagreeing)

    def test_every_official_run_is_a_single_addressable_shard(self) -> None:
        runs = plan.expand_runs(self.manifest, CONFIGS, ATTRIBUTES)
        addresses = {(run["egl_config_index"], run["configuration_index"])
                     for run in runs}
        self.assertEqual(len(addresses), len(runs))

    def test_documents_must_keep_their_order_identity_and_scope(self) -> None:
        for mutate, message in (
            (lambda m: m["documents"].reverse(), "collection order"),
            (lambda m: m["documents"][0].update(sha256="0" * 64), "identity changed"),
            (lambda m: m["documents"][2].update(
                text=m["documents"][2]["text"].replace('os="any"', 'os="android"', 1)),
             "identity changed"),
            (lambda m: m.update(runner_source={**plan.RUNNER_SOURCE, "size": 1}),
             "runner scope source changed"),
            (lambda m: m["leaves"][0].update(sha256="0" * 64 + "x"),
             "invalid OpenGL ES leaf digest"),
            (lambda m: m["configurations"][0].update(base_seed=9),
             "differ from authenticated XML"),
        ):
            broken = copy.deepcopy(self.manifest)
            mutate(broken)
            with self.subTest(message=message):
                with self.assertRaisesRegex(plan.PlanError, message):
                    plan.validate_manifest(broken)

    def test_an_unsupported_caselist_or_surface_is_refused(self) -> None:
        for old, new in (
            ("gles32-khr-single.txt", "gles33-khr-single.txt"),
            ("gles32-khr-single.txt", "../gles32-khr-single.txt"),
            ('name="khr-single"', 'name="khr-other"'),
            ("--deqp-surface-type=fbo", "--deqp-surface-type=window"),
            ("--deqp-base-seed=1", "--deqp-base-seed=-1"),
        ):
            broken = copy.deepcopy(self.manifest)
            index = 2 if "single" in old else 2
            document = broken["documents"][index]
            if old not in document["text"]:
                document = next(item for item in broken["documents"]
                                if old in item["text"])
            document["text"] = document["text"].replace(old, new, 1)
            payload = document["text"].encode("utf-8")
            document["size"] = len(payload)
            document["sha256"] = gl_plan.digest(payload)
            with self.subTest(old=old), self.assertRaises(plan.PlanError):
                plan.validate_manifest(broken)

    def test_leaf_identities_must_agree_with_the_cts_identity(self) -> None:
        document = json.loads(common.GLES32_MATRIX_PATH.read_text())
        identity = common.load_identity()
        shared = {leaf.path for leaf in common.leaf_identities(identity).values()}
        row = next(item for item in document["leaves"] if item["path"] in shared)
        row["sha256"] = "0" * 64
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "vk_gl_cts_gles32_matrix.json"
            for name in ("vk_gl_cts_gles_mustpass.xml",
                         "vk_gl_cts_gles_mustpass_noctx.xml",
                         "vk_gl_cts_gles_mustpass_single.xml",
                         "vk_gl_cts_gles_aosp_mustpass.xml",
                         "vk_gl_cts_egl_aosp_mustpass.xml"):
                (path.parent / name).write_bytes(
                    common.GLES32_MATRIX_PATH.with_name(name).read_bytes())
            path.write_text(json.dumps(document))
            with self.assertRaisesRegex(common.CampaignError, "disagrees with the CTS identity"):
                common.load_gles32_matrix(identity, path)

    def receipt(self) -> dict:
        # The ES 3.2 matrix runs from the same sealed image as GL46, so its
        # receipt carries the GL46 scope and the shared runner source.
        return fixtures.zink_gl_derivative_receipt(common.load_identity())

    def test_typed_matrix_matches_every_xml_role_and_guest_identity_checks(self) -> None:
        receipt = self.receipt()
        configs = tuple(common.EglConfig(row["id"], tuple(row["surface_types"]))
                        for row in CONFIGS)
        _, contract = common.load_zink_gl_derivative_receipt(
            receipt, common.load_identity())
        for run in plan.expand_runs(self.manifest, CONFIGS, ATTRIBUTES):
            with self.subTest(run=run["index"]):
                matrix = common.gles32_matrix_run(
                    self.manifest, receipt, configs, "1" * 64,
                    run["egl_config_index"], run["configuration_index"], (0, 8),
                    (), ATTRIBUTES,
                )
                self.assertEqual(matrix.configuration.api, "gles32")
                self.assertEqual(
                    common.matrix_runner_arguments(matrix, "/run/selected.txt"),
                    plan.runner_arguments(self.manifest, run, "/run/selected.txt"),
                )
                script = common.build_guest_launcher(
                    matrix.selection, 0, 60, contract,
                    matrix=matrix, renderer_backend="virgl",
                )
                self.assertIn(matrix.mustpass_xml_sha256, script)
                self.assertIn(matrix.selection.leaf.sha256, script)
                self.assertEqual(
                    subprocess.run(["/bin/sh", "-n"], input=script, text=True,
                                   capture_output=True).returncode, 0)

    def test_a_legacy_receipt_or_wrong_role_cannot_run_the_matrix(self) -> None:
        configs = tuple(common.EglConfig(row["id"], tuple(row["surface_types"]))
                        for row in CONFIGS)
        legacy = self.receipt()
        legacy["schema_version"] = 3
        with self.assertRaisesRegex(common.CampaignError, "schema-4"):
            common.gles32_matrix_run(self.manifest, legacy, configs, "1" * 64,
                                     0, 3, (0, 8), (), ATTRIBUTES)
        # The one-time collections exist on the first config only, the
        # other-config rows only away from it, and an unknown row never.
        for config_index, xml_index in ((1, 0), (1, 2), (1, 3), (0, 4), (0, 99)):
            with self.subTest(config=config_index, xml=xml_index):
                with self.assertRaises(common.CampaignError):
                    common.gles32_matrix_run(self.manifest, self.receipt(), configs,
                                             "1" * 64, config_index, xml_index,
                                             (0, 8), (), ATTRIBUTES)

    def test_the_matrix_is_bound_to_the_es32_inventory_and_its_runtime(self) -> None:
        receipt = self.receipt()
        _, contract = common.load_zink_gl_derivative_receipt(
            receipt, common.load_identity())
        runtime = {"emulator_sha256": "1" * 64, "kosmickrisp_sha256": "2" * 64}
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = fixtures.write_config_inventory_evidence(
                directory, "gles32", contract, runtime_identity=runtime,
                renderer="virgl",
            )
            configs, _, attributes = common.load_config_inventory_evidence(
                path, "gles32", "4k", "vnc", contract, runtime)
            self.assertEqual([row.identifier for row in configs], [1, 2])
            # The ES 3.2 scope also records the attribute table the AOSP
            # mapping resolves its GL configuration names against.
            self.assertEqual([row["id"] for row in attributes], [1, 2, 3, 4])
            self.assertEqual(attributes[2]["samples"], 4)
            original = path.read_text()
            for key, value in (("emulator_sha256", "3" * 64),
                               ("kosmickrisp_sha256", "3" * 64),
                               ("api", "gl46"), ("egl_configs", []),
                               ("egl_config_attributes_sha256", "4" * 64),
                               ("config_cases", ["CTS-Configs.es32"]),
                               ("completed_cases", 1)):
                changed = json.loads(original)
                changed[key] = value
                path.write_text(json.dumps(changed))
                with self.subTest(key=key), self.assertRaises(common.CampaignError):
                    common.load_config_inventory_evidence(
                        path, "gles32", "4k", "vnc", contract, runtime)
            path.write_text(original)
            with self.assertRaises(common.CampaignError):
                # A GL46 inventory never stands in for the ES 3.2 one.
                common.load_config_inventory_evidence(
                    path, "gl46", "4k", "vnc", contract, runtime)

    def test_the_es32_inventory_scope_captures_the_attribute_table(self) -> None:
        receipt = self.receipt()
        _, contract = common.load_zink_gl_derivative_receipt(
            receipt, common.load_identity())
        script = common.build_guest_launcher(
            None, 0, 60, contract, config_inventory_api="gles32",
            renderer_backend="virgl")
        # Two cases need a caselist: --deqp-case selects one filter, and both
        # results must land in one session's QPA.
        self.assertIn("--deqp-caselist-file=/run/uml-cts/configs.txt", script)
        self.assertIn("CTS-Configs.es32", script)
        self.assertIn(common.EGL_ATTRIBUTE_CASE, script)
        self.assertEqual(
            subprocess.run(["/bin/sh", "-n"], input=script, text=True,
                           capture_output=True).returncode, 0)
        narrow = common.build_guest_launcher(
            None, 0, 60, contract, config_inventory_api="gl46",
            renderer_backend="virgl")
        self.assertIn("--deqp-case=CTS-Configs.gl46", narrow)
        self.assertNotIn("configs.txt", narrow)

    def test_the_cli_takes_an_es32_matrix_and_its_xml_row(self) -> None:
        receipt = self.receipt()
        _, contract = common.load_zink_gl_derivative_receipt(
            receipt, common.load_identity())
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            receipt_path = directory / "receipt.json"
            receipt_path.write_text(json.dumps(receipt))
            arguments = ["--renderer", "virgl", "--config-inventory", "gles32",
                         "--rootfs-type", "ext4", "--rootfs-receipt", str(receipt_path)]
            self.assertEqual(
                common.parse_arguments(arguments).config_inventory, "gles32")
            emulator = directory / "uml-darwin"
            emulator.write_bytes(b"#!/bin/sh\nexit 0\n")
            emulator.chmod(0o755)
            runtime = {"emulator_sha256": common.sha256_file(emulator),
                       "kosmickrisp_sha256": "2" * 64}
            inventory = fixtures.write_config_inventory_evidence(
                directory / "inventory", "gles32", contract,
                runtime_identity=runtime, renderer="virgl")
            rootfs = directory / "rootfs.ext4"
            rootfs.write_bytes(b"")
            shard = ["--matrix", "gles32", "--qualification", "--display", "vnc",
                     "--renderer", "virgl", "--rootfs-type", "ext4",
                     "--rootfs", str(rootfs),
                     "--rootfs-receipt", str(receipt_path),
                     "--config-inventory-run", str(inventory),
                     "--emulator", str(emulator),
                     "--egl-config-index", "0", "--fraction", "3,8"]
            with mock.patch.object(common, "verify_staged_kosmickrisp",
                                   return_value=runtime["kosmickrisp_sha256"]), \
                    mock.patch.object(common, "validate_rootfs_file"):
                options = common.parse_arguments(
                    shard + ["--mustpass-config-index", "9"])
                self.assertEqual(options.matrix_run.configuration.leaf.identifier,
                                 "gles32-khr-main")
                self.assertEqual(options.matrix_run.selection.fraction_index, 3)
                self.assertEqual(options.runner_oracle.sha256,
                                 plan.RUNNER_SOURCE["sha256"])
                with self.assertRaises(SystemExit), \
                        contextlib.redirect_stderr(io.StringIO()):
                    # An XML-scoped matrix always names the row it runs.
                    common.parse_arguments(shard)


if __name__ == "__main__":
    unittest.main()
