#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import base64
import contextlib
import copy
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import socket
import subprocess
import shlex
import struct
import sys
import tempfile
import threading
import unittest
from xml.etree import ElementTree
from unittest import mock


INTEGRATION_GPU = Path(__file__).resolve().parents[2] / "integration/gpu"
sys.path.insert(0, str(INTEGRATION_GPU))
import run_vk_gl_cts as cts  # noqa: E402
import receipt_fixtures  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[3]


def erofs_contract() -> cts.RootfsContract:
    identity = cts.load_identity()
    glcts = cts.cts_binary_identity(
        identity, "/opt/vk-gl-cts/openglcts/glcts"
    )
    return cts.RootfsContract(
        filesystem_type="erofs",
        size=identity["rootfs"]["size"],
        sha256=identity["rootfs"]["sha256"],
        glcts_sha256=glcts["sha256"],
    )


def derivative_receipt(identity: dict[str, object]) -> dict[str, object]:
    exclusions = ["/opt/geekbench", "/opt/ltp", "/opt/vk-gl-cts/vulkan"]
    return {
        "schema_version": 1,
        "artifact": "uml-gl-gles-rootfs",
        "image": {
            "filesystem": "ext4",
            "filesystem_uuid": "688cb152-ce4b-4ac4-9b5d-3def792791f3",
            "size": 8 * 1024 * 1024 * 1024,
            "sha256": "1" * 64,
            "read_only": True,
        },
        "base_rootfs": {
            "size": identity["rootfs"]["size"],
            "sha256": identity["rootfs"]["sha256"],
        },
        "clone": {
            "excluded_paths": exclusions,
            "excluded_paths_sha256": cts.exclusion_digest(exclusions),
        },
        "guest_receipt": {
            "path": "/usr/share/uml-rootfs/gl-gles-cts-build.env",
            "sha256": "2" * 64,
        },
        "cts": {
            "glcts_path": "/opt/vk-gl-cts/openglcts/glcts",
            "glcts_size": 123456,
            "glcts_sha256": "3" * 64,
            "glcts_build_id": "abcd1234",
        },
    }


def opengl_mustpass_files() -> dict[str, bytes]:
    """A small upstream-shaped OpenGL must-pass tree: each XML and its leaves, by guest path."""
    plan = cts.opengl_cts_plan
    files = {}
    for collection, directory, package_name, _ in plan.COLLECTIONS:
        document = ElementTree.Element("Mustpass", version="main")
        package = ElementTree.SubElement(document, "TestPackage", name=package_name)
        if collection == "es-for-gl":
            rows = [("gl45-es3-main", True, 256, 256, None, "rgba8888d24s8ms0"),
                    ("gl45-es31-main", True, 256, 256, None, "rgba8888d24s8ms0")]
        elif collection == "no-context":
            rows = [(f"gl{api}-khr-main", True, 64, 64, 1, None)
                    for api in (30, 40, 43, 45)]
        elif collection == "single":
            rows = [("gl43-khr-single", True, 64, 64, 1, None),
                    ("gl46-khr-single", True, 64, 64, 1, None)]
        else:
            rows = [("gl46-main", True, 64, 64, 1, None),
                    ("gl46-main", True, 113, 47, 2, None),
                    ("gl46-main", True, 64, -1, 3, "rgba8888d24s8"),
                    ("gl46-main", True, -1, 64, 3, "rgba8888d24s8"),
                    ("gl46-main", False, 64, 64, 1, None),
                    ("gl46-main", False, 113, 47, 2, None),
                    ("gl42-compat-main", True, 64, 64, 1, None)]
        for stem, first, width, height, seed, config in rows:
            command = ["--deqp-screen-rotation=unspecified",
                       f"--deqp-surface-width={width}",
                       f"--deqp-surface-height={height}", "--deqp-watchdog=disable"]
            if seed is not None:
                command.append(f"--deqp-base-seed={seed}")
            if config:
                command.append(f"--deqp-gl-config-name={config}")
                if collection != "es-for-gl":
                    command.append("--deqp-surface-type=fbo")
            name = stem.split("-", 1)[1].removeprefix("compat-")
            ElementTree.SubElement(package, "Configuration", {
                "caseListFile": stem + ".txt", "commandLine": " ".join(command),
                "name": name, "os": "any", "useForFirstEGLConfig": str(first),
            })
            count = 1 if stem in {"gl30-khr-main", "gl40-khr-main"} else 10
            files[f"{plan.ROOT}/{directory}/main/{stem}.txt"] = "".join(
                f"KHR-{stem.replace('-', '.')}.case{index}\n" for index in range(count)
            ).encode()
        files[f"{plan.ROOT}/{directory}/main/mustpass.xml"] = ElementTree.tostring(document)
    files[f"{plan.ROOT}/khronos_mustpass/main/gl46-gtf-main.txt"] = b"GTF-GL46.obsolete\n"
    return files


def zink_gl_derivative_receipt(
    identity: dict[str, object],
) -> dict[str, object]:
    receipt = receipt_fixtures.current_epoch_receipt(
        receipt_fixtures.VULKAN_RECEIPT
    )
    receipt["schema_version"] = 4
    receipt["artifact"] = "uml-vulkan-zink-gl-rootfs"
    receipt["base_rootfs"]["size"] = identity["rootfs"]["size"]
    receipt["base_rootfs"]["sha256"] = identity["rootfs"]["sha256"]
    receipt["derivative"] = {
        "filename": "rootfs-vulkan-zink-gl.ext4",
        "filesystem": "ext4",
        "size": 8 * 1024 * 1024 * 1024,
        "sha256": "4" * 64,
        "filesystem_uuid": "3b59fbc9-4b35-4a2b-890d-286a77b04afd",
        "read_only": True,
    }
    receipt["opengl_cts"] = {
        "release": "vulkan-cts-1.4.6.2",
        "configure_argv": ["cmake"],
        "build_targets": ["cmake", "--build", "build", "--target", "glcts"],
        "install_prefix": "/opt/vk-gl-cts/openglcts",
        "executable": {
            "path": "/opt/vk-gl-cts/openglcts/glcts",
            "size": 123456,
            "sha256": "5" * 64,
        },
        "resources": [
            {
                "path": f"/opt/vk-gl-cts/openglcts/{directory}",
                "file_count": 1,
                "total_bytes": 1,
                "verified_against": "test fixture",
            }
            for directory in ("gl_cts", "gles2", "gles3", "gles31")
        ],
        "build_id": "abcd1234",
        "reported_cpu": "DE_CPU_ARM_64",
        "reported_target": "Default",
        "mustpass": cts.opengl_cts_plan.build_manifest(
            opengl_mustpass_files().__getitem__
        ),
    }
    return receipt


def qpa_result(case: str, status: str = "Pass") -> str:
    return f"""#beginTestCaseResult {case}
<?xml version="1.0" encoding="UTF-8"?>
<TestCaseResult Version="0.3.4" CasePath="{case}">
 <Result StatusCode="{status}">result</Result>
</TestCaseResult>
#endTestCaseResult
"""


def qpa(*results: str) -> str:
    return "#sessionInfo releaseName 1.4.5.5\n#beginSession\n" + "".join(
        results
    ) + "#endSession\n"


EGL_ATTRIBUTE_ROWS = (
    # id, surface bits, samples, conformant to ES 3
    (1, "EGL_WINDOW_BIT|EGL_PIXMAP_BIT|EGL_PBUFFER_BIT", 0, True),
    (2, "EGL_PIXMAP_BIT|EGL_PBUFFER_BIT", 0, True),
    (3, "EGL_WINDOW_BIT|EGL_PBUFFER_BIT", 4, True),
    (4, "EGL_WINDOW_BIT", 0, False),
)


def egl_attribute_qpa(rows: tuple[tuple[int, str, int, bool], ...] = EGL_ATTRIBUTE_ROWS) -> str:
    """dEQP-EGL.info.configs as the ES 3.2 inventory scope records it."""
    case = cts.EGL_ATTRIBUTE_CASE
    configs = "".join(
        f'  <EglConfig RedSize="8" GreenSize="8" BlueSize="8" AlphaSize="8"'
        f' DepthSize="24" StencilSize="8" Samples="{samples}" ConfigID="{identifier}"'
        f' RenderableType="EGL_OPENGL_ES2_BIT|EGL_OPENGL_ES3_BIT_KHR"'
        f' Conformant="{"EGL_OPENGL_ES2_BIT|EGL_OPENGL_ES3_BIT_KHR" if conformant else "EGL_OPENGL_ES2_BIT"}"'
        f' SurfaceTypes="{surfaces}" />\n'
        for identifier, surfaces, samples, conformant in rows
    )
    return f"""#beginTestCaseResult {case}
<?xml version="1.0" encoding="UTF-8"?>
<TestCaseResult Version="0.3.4" CasePath="{case}" CaseType="SelfValidate">
 <EglConfigSet Name="EGL-configs" Description="List of all EGL configs">
{configs} </EglConfigSet>
 <Result StatusCode="Pass">Pass</Result>
</TestCaseResult>
#endTestCaseResult
"""


def config_qpa(
    api: str,
    rows: tuple[str, ...] = (
        "EGL(1): window, pixmap, pbuffer",
        "EGL(2): pbuffer, pixmap",
    ),
    status: str = "Pass",
    attribute_rows: tuple[tuple[int, str, int, bool], ...] = EGL_ATTRIBUTE_ROWS,
) -> str:
    case = cts.CONFIG_CASES[api]
    messages = "".join(f"  <Text>{row}</Text>\n" for row in rows)
    result = f"""#beginTestCaseResult {case}
<?xml version="1.0" encoding="UTF-8"?>
<TestCaseResult Version="0.3.4" CasePath="{case}" CaseType="SelfValidate">
 <Section Name="Configs" Description="Configs">
{messages} </Section>
 <Result StatusCode="{status}">{status}</Result>
</TestCaseResult>
#endTestCaseResult
"""
    if cts.EGL_ATTRIBUTE_CASE in cts.CONFIG_INVENTORY_SCOPES[api]:
        result += egl_attribute_qpa(attribute_rows)
    return qpa(result)


def write_config_inventory_evidence(
    directory: Path,
    api: str,
    contract: cts.RootfsContract,
    page_profile: str = "4k",
    display: str = "vnc",
    runtime_identity: dict[str, str] | None = None,
    renderer: str = "zink",
) -> Path:
    payload = config_qpa(api).encode()
    configs = cts.parse_egl_config_qpa(payload.decode(), api)
    artifacts = directory / "artifacts"
    artifacts.mkdir(parents=True)
    (artifacts / "results.qpa").write_bytes(payload)
    metadata = {
        "schema_version": 1,
        "adapter": "macos",
        "backend": "zink-kosmickrisp",
        "mode": "config-inventory",
        "api": api,
        "page_profile": page_profile,
        "display": display,
        "rootfs_type": contract.filesystem_type,
        "rootfs_sha256": contract.sha256,
        "glcts_sha256": contract.glcts_sha256,
        "emulator_return_code": 0,
        "completed_cases": len(cts.CONFIG_INVENTORY_SCOPES[api]),
        "status_totals": {"Pass": len(cts.CONFIG_INVENTORY_SCOPES[api])},
        "qpa_sha256": hashlib.sha256(payload).hexdigest(),
        "egl_config_sha256": cts.egl_config_digest(configs),
        "egl_configs": [
            {
                "id": config.identifier,
                "surface_types": list(config.surface_types),
                "selected_surface_type": config.selected_surface_type,
            }
            for config in configs
        ],
    }
    if len(cts.CONFIG_INVENTORY_SCOPES[api]) > 1:
        attributes = cts.parse_egl_config_attributes(payload.decode())
        metadata.update({
            "config_scope": cts.CONFIG_INVENTORY_SCOPE_NAMES[api],
            "config_cases": list(cts.CONFIG_INVENTORY_SCOPES[api]),
            "egl_config_attributes": attributes,
            "egl_config_attributes_sha256": cts.egl_attribute_digest(attributes),
        })
    if api in cts.XML_MATRIX_APIS:
        metadata.update({"backend": cts.host_backend_label(renderer),
                         "renderer": renderer,
                         "surface": [1280, 720], **(runtime_identity or {})})
        (directory / "console.log").write_text("Darwin UML stopped: result=0 exitcode=0\n")
        client = (
            "zink over Virtio-GPU Venus" if renderer == "zink"
            else "virgl (zink Vulkan 1.4(Apple M1 Ultra (MESA_KOSMICKRISP)))"
        )
        payloads = {
            "results.qpa": payload,
            "renderer.txt": ("device=/dev/dri/card0\ndevice=/dev/dri/renderD128\n"
                             "x11_server=xorg\nx11_dri3=1\nx11_present=1\nx11_glamor=1\nx11_connected=1\nx11_drm_fd=1\nx11_blanking=off\nguest_page_size=4096\n"
                             f"OpenGL renderer string: {client}\n").encode(),
            "summary.txt": b"cts_rc=0\n",
        }
        for name, content in payloads.items():
            (artifacts / name).write_bytes(content)
        serial = directory / cts.ARTIFACT_TRANSPORT_LOG
        serial.write_text("".join(framed_artifact(name, content) for name, content in payloads.items()))
        metadata.update({"console_sha256": cts.sha256_file(directory / "console.log"),
                         "artifact_transport_sha256": cts.sha256_file(serial)})
    run = directory / "run.json"
    run.write_text(json.dumps(metadata), encoding="utf-8")
    return run


def framed_artifact(name: str, payload: bytes) -> str:
    compressed = gzip.compress(payload, mtime=0)
    digest = hashlib.sha256(compressed).hexdigest()
    encoded = base64.b64encode(compressed).decode("ascii")
    chunks = [encoded[offset : offset + 16] for offset in range(0, len(encoded), 16)]
    lines = [
        f"UML_CTS_ARTIFACT_BEGIN|{name}|gzip+base64|{len(compressed)}|{digest}"
    ]
    lines.extend(
        f"UML_CTS_ARTIFACT_CHUNK|{name}|{index}|{chunk}"
        for index, chunk in enumerate(chunks)
    )
    lines.append(f"UML_CTS_ARTIFACT_END|{name}|{len(chunks)}")
    return "\n".join(lines) + "\n"


def gpu_capability_report(*, venus_capset: bool = False) -> str:
    lines: list[str] = []
    extensions = ["GL_EXT_alpha", "GL_EXT_beta"]
    for major, minor in ((4, 6),):
        encode = lambda value: value.encode().hex()
        lines.append(
            "UML_GPU_HOST_CONTEXT|"
            f"requested={major}.{minor}|created=1|egl_error=12288|"
            f"actual={major}.{minor}|vendor_hex={encode('vendor')}|"
            f"renderer_hex={encode('renderer')}|"
            f"version_hex={encode(f'{major}.{minor} (Core Profile)')}|"
            f"glsl_hex={encode('4.60')}|extensions={len(extensions)}"
        )
        lines.extend(
            "UML_GPU_HOST_EXTENSION|"
            f"requested={major}.{minor}|name_hex={extension.encode().hex()}"
            for extension in extensions
        )
    capset_v1 = bytearray(308)
    struct.pack_into("<I", capset_v1, 0, 1)
    capset_v2 = bytearray(1408)
    struct.pack_into("<I", capset_v2, 0, 2)
    capset_v2[696:705] = b"renderer\0"
    reported = [(1, 1, bytes(capset_v1)), (2, 2, bytes(capset_v2))]
    if venus_capset:
        reported.append((cts.VENUS_CAPSET_ID, 0, bytes(range(16))))
    for identifier, version, payload in reported:
        lines.extend(
            (
                "UML_GPU_CAPSET_BEGIN|"
                f"id={identifier}|version={version}|size={len(payload)}|chunks=1",
                "UML_GPU_CAPSET_CHUNK|"
                f"id={identifier}|index=0|hex={payload.hex()}",
                f"UML_GPU_CAPSET_END|id={identifier}|chunks=1",
            )
        )
    return "\n".join(lines) + "\n"


class VkGlCtsToolingTests(unittest.TestCase):
    def test_frozen_identity_is_complete_and_qualification_is_blocked(self) -> None:
        identity = cts.load_identity()
        leaves = cts.leaf_identities(identity)
        self.assertFalse(identity["cts"]["qualification_ready"])
        self.assertIsNone(identity["cts"]["authoritative_build_receipt"])
        self.assertEqual(identity["cts"]["inventory_scope"], "gles2-gles3")
        self.assertEqual(identity["cts"]["complete_caselist_case_count"], 6972)
        self.assertEqual(
            identity["cts"]["complete_caselist_sha256"],
            "3ec390ed2d36f0fd8e32a02718e19c5750cf9381eab002aac9b71f848ebc6162",
        )
        self.assertEqual(
            identity["cts"]["registered_modules"],
            ["KHR-GLES2", "KHR-GLES3"],
        )
        self.assertEqual(leaves["gles2"].case_count, 473)
        self.assertEqual(leaves["gles3"].case_count, 6498)
        self.assertEqual(leaves["gles31"].case_count, 4101)
        self.assertEqual(leaves["gles32-extensions"].case_count, 1097)
        self.assertEqual(leaves["gl31"].case_count, 888)
        self.assertEqual(leaves["gl46"].case_count, 18647)
        for leaf in leaves.values():
            self.assertRegex(leaf.sha256, r"^[0-9a-f]{64}$")
            self.assertTrue(leaf.path.startswith("/opt/vk-gl-cts/"))

    def test_checked_gles_mustpass_expansion_is_exact_and_typed(self) -> None:
        identity = cts.load_identity()
        payload = cts.GLES_MUSTPASS_PATH.read_bytes()
        self.assertEqual(len(payload), identity["mustpass"]["gles_xml"]["size"])
        self.assertEqual(
            hashlib.sha256(payload).hexdigest(),
            identity["mustpass"]["gles_xml"]["sha256"],
        )
        configurations = cts.load_gles_mustpass(identity)
        self.assertEqual(
            set(configurations).intersection(cts.BASELINE_GLES_APIS),
            set(cts.BASELINE_GLES_APIS),
        )
        first = cts.baseline_mustpass_configuration(
            configurations, "gles3", True
        )
        other = cts.baseline_mustpass_configuration(
            configurations, "gles3", False
        )
        self.assertEqual(first.leaf.identifier, "gles3")
        self.assertEqual(first.base_seed, 1)
        self.assertEqual((first.surface_width, first.surface_height), (64, 64))
        self.assertEqual(first.watchdog, "disable")
        self.assertEqual(first.xml_arguments, other.xml_arguments)

        selection = cts.Selection(first.leaf, (), 0, 8)
        matrix = cts.MatrixRun(
            configuration=first,
            egl_config=cts.EglConfig(7, ("pbuffer", "pixmap")),
            egl_config_index=0,
            config_inventory_sha256="1" * 64,
            selection=selection,
            mustpass_xml_path=identity["mustpass"]["gles_xml"]["path"],
            mustpass_xml_size=identity["mustpass"]["gles_xml"]["size"],
            mustpass_xml_sha256=identity["mustpass"]["gles_xml"]["sha256"],
        )
        self.assertEqual(
            cts.matrix_runner_arguments(matrix, "/run/uml-cts/selected.txt"),
            [
                "--deqp-caselist-file=/run/uml-cts/selected.txt",
                "--deqp-screen-rotation=unspecified",
                "--deqp-surface-width=64",
                "--deqp-surface-height=64",
                "--deqp-watchdog=disable",
                "--deqp-base-seed=1",
                "--deqp-surface-type=pbuffer",
                "--deqp-gl-config-id=7",
                "--deqp-gl-context-type=egl",
            ],
        )

        with tempfile.TemporaryDirectory() as temporary:
            changed = Path(temporary) / "mustpass.xml"
            changed.write_bytes(payload.replace(b"base-seed=1", b"base-seed=9", 1))
            with self.assertRaisesRegex(cts.CampaignError, "identity changed"):
                cts.load_gles_mustpass(identity, changed)

    def test_released_macos_runner_oracle_matches_expansion(self) -> None:
        identity = cts.load_identity()
        configurations = cts.load_gles_mustpass(identity)
        oracle = cts.load_runner_oracle(identity, configurations)
        payload = cts.RUNNER_ORACLE_PATH.read_bytes()
        self.assertEqual(oracle.path, "tests/integration/gpu/vk_gl_cts_runner_oracle.json")
        self.assertEqual(oracle.size, len(payload))
        self.assertEqual(oracle.sha256, hashlib.sha256(payload).hexdigest())
        self.assertEqual(
            oracle.fixture_configs,
            (
                cts.EglConfig(17, ("window", "pixmap", "pbuffer")),
                cts.EglConfig(23, ("pbuffer", "pixmap")),
                cts.EglConfig(29, ("pixmap",)),
            ),
        )
        self.assertEqual(
            oracle.successful_case_statuses,
            (
                "Pass",
                "NotSupported",
                "QualityWarning",
                "CompatibilityWarning",
            ),
        )

        with tempfile.TemporaryDirectory() as temporary:
            changed = Path(temporary) / "oracle.json"
            value = json.loads(payload)
            value["summaries"]["gles3"]["runner_arguments"][1][6] = (
                "--deqp-surface-type=window"
            )
            changed.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(
                cts.CampaignError, "oracle expansion changed"
            ):
                cts.load_runner_oracle(identity, configurations, changed)

    def test_derivative_receipt_is_strict_and_tied_to_the_base(self) -> None:
        identity = cts.load_identity()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "receipt.json"
            receipt = derivative_receipt(identity)
            path.write_text(json.dumps(receipt), encoding="utf-8")
            loaded, contract = cts.load_derivative_receipt(path, identity)
            self.assertEqual(loaded, receipt)
            self.assertEqual(contract.filesystem_type, "ext4")
            self.assertEqual(contract.glcts_sha256, "3" * 64)

            receipt["base_rootfs"]["sha256"] = "4" * 64
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with self.assertRaisesRegex(cts.CampaignError, "different base"):
                cts.load_derivative_receipt(path, identity)

            receipt = derivative_receipt(identity)
            receipt["clone"]["excluded_paths_sha256"] = "5" * 64
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with self.assertRaisesRegex(cts.CampaignError, "exclusion"):
                cts.load_derivative_receipt(path, identity)

    def test_checked_derivative_receipt_freezes_the_selected_image(self) -> None:
        """The application may boot exactly the epoch this receipt names.

        Kernarium bakes this receipt into its generated CTS campaigns, so the
        identity it pins is the only rendered image an iOS build will accept.
        """
        receipt, contract = cts.load_derivative_receipt(
            receipt_fixtures.current_epoch_receipt_file(
                self, receipt_fixtures.ZINK_GL_RECEIPT
            ),
            cts.load_identity(),
        )
        self.assertEqual(receipt["artifact"], "uml-vulkan-zink-gl-rootfs")
        self.assertEqual(contract.filesystem_type, "ext4")
        self.assertEqual(contract.size, 8 * 1024 * 1024 * 1024)
        self.assertEqual(
            contract.sha256,
            "c2030ed1d0d356a0440653ea590d6ffcefebedf14929b477e1e4892b9442b5b0",
        )
        self.assertEqual(
            contract.glcts_sha256,
            "9bb567b4db930fce28e2dc893a09fa0bd5b7c046f72d0807c3546ae4311dc1d1",
        )
        self.assertEqual(
            contract.filesystem_uuid, "3b59fbc9-4b35-4a2b-890d-286a77b04afd"
        )
        self.assertEqual(
            contract.guest_receipt_path,
            "/usr/share/uml-vulkan/rootfs-build-receipt.json",
        )
        # The rendered image drops every unrelated payload the base carries and
        # every other Vulkan ICD, so a reappearing benchmark tree or a second
        # driver means the wrong clone.
        for excluded in (
            "/opt/geekbench",
            "/opt/ltp",
            "/opt/vk-gl-cts",
            "/usr/share/vulkan/icd.d/lvp_icd.json",
        ):
            self.assertIn(excluded, receipt["exclusions"])

    def test_ext4_rootfs_contract_is_forwarded_and_checked_in_guest(self) -> None:
        identity = cts.load_identity()
        with tempfile.TemporaryDirectory() as temporary:
            receipt_path = Path(temporary) / "receipt.json"
            receipt_path.write_text(
                json.dumps(derivative_receipt(identity)), encoding="utf-8"
            )
            options = cts.parse_arguments(
                [
                    "--leaf",
                    "gles3",
                    "--api",
                    "gles3",
                    "--rootfs",
                    "/tmp/rootfs.ext4",
                    "--rootfs-type",
                    "ext4",
                    "--rootfs-receipt",
                    str(receipt_path),
                ]
            )
            command = cts.build_harness_command(
                options,
                cts.Selection(options.leaves["gles3"], (), 0, 1),
                Path("/tmp/uml-cts-test"),
            )
            rootfs_type = command.index("--rootfs-type")
            self.assertEqual(
                command[rootfs_type : rootfs_type + 2],
                ["--rootfs-type", "ext4"],
            )
            launcher = cts.build_guest_launcher(
                cts.Selection(options.leaves["gles3"], (), 0, 1),
                0,
                900,
                options.rootfs_contract,
            )
            self.assertIn("688cb152-ce4b-4ac4-9b5d-3def792791f3", launcher)
            self.assertIn("gl-gles-cts-build.env", launcher)
            self.assertIn("3" * 64, launcher)

        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
            SystemExit
        ):
            cts.parse_arguments(
                [
                    "--leaf",
                    "gles3",
                    "--api",
                    "gles3",
                    "--rootfs-type",
                    "ext4",
                ]
            )

    def test_zink_gl_receipt_owns_exact_gl46_leaves_and_launcher(self) -> None:
        identity = cts.load_identity()
        receipt = zink_gl_derivative_receipt(identity)
        leaf = next(row for row in receipt["opengl_cts"]["mustpass"]["leaves"]
                    if row["id"] == "gl46")
        with tempfile.TemporaryDirectory() as temporary:
            receipt_path = Path(temporary) / "zink-gl-receipt.json"
            receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
            options = cts.parse_arguments(
                [
                    "--renderer",
                    "zink",
                    "--leaf",
                    "gl46",
                    "--api",
                    "gl46",
                    "--fraction",
                    "3,8",
                    "--rootfs",
                    "/tmp/rootfs-vulkan-zink-gl.ext4",
                    "--rootfs-type",
                    "ext4",
                    "--rootfs-receipt",
                    str(receipt_path),
                ]
            )
            self.assertEqual(options.rootfs_contract.glcts_sha256, "5" * 64)
            self.assertEqual(options.leaves["gl46"].case_count, leaf["case_count"])
            self.assertEqual(options.leaves["gl46"].sha256, leaf["sha256"])
            selection = cts.Selection(options.leaves["gl46"], (), 3, 8)
            launcher = cts.build_guest_launcher(
                selection,
                0,
                900,
                options.rootfs_contract,
                renderer_backend="zink",
            )
            self.assertIn("MESA_LOADER_DRIVER_OVERRIDE=zink", launcher)
            self.assertIn("renderer is not Zink over Virtio-GPU Venus", launcher)
            command = cts.build_harness_command(
                options, selection, Path("/tmp/uml-zink-cts-test")
            )
            self.assertEqual(cts.gl_scope_launch_environment(options), ())
            self.assertEqual(command[0], "env")
            self.assertFalse(command[1].startswith("UML_GPU_"))
            self.assertFalse(
                any("systemctl set-environment" in argument for argument in command)
            )
            self.assertEqual(
                command[command.index("--unit") + 1], cts.CTS_TARGET_UNIT
            )

    def test_every_virgl_scope_reports_its_host_provider(self) -> None:
        identity = cts.load_identity()
        with tempfile.TemporaryDirectory() as temporary:
            receipt_path = Path(temporary) / "zink-gl-receipt.json"
            receipt_path.write_text(
                json.dumps(zink_gl_derivative_receipt(identity)),
                encoding="utf-8",
            )
            options = cts.parse_arguments(
                [
                    "--renderer",
                    "virgl",
                    "--config-inventory",
                    "gl46",
                    "--rootfs",
                    "/tmp/rootfs-vulkan-zink-gl.ext4",
                    "--rootfs-type",
                    "ext4",
                    "--rootfs-receipt",
                    str(receipt_path),
                ]
            )
            self.assertTrue(options.gl46_scope)
            self.assertEqual(
                cts.gl_scope_launch_environment(options),
                ("UML_GPU_CAPABILITY_REPORT",),
            )
        gles3 = cts.parse_arguments(["--leaf", "gles3", "--api", "gles3"])
        self.assertFalse(gles3.gl46_scope)
        self.assertEqual(
            cts.gl_scope_launch_environment(gles3),
            ("UML_GPU_CAPABILITY_REPORT",),
        )

    def test_zink_gl_receipt_requires_schema_4(self) -> None:
        identity = cts.load_identity()
        for version in (3, 5):
            receipt = zink_gl_derivative_receipt(identity)
            receipt["schema_version"] = version
            with self.subTest(version=version):
                with self.assertRaisesRegex(
                    cts.CampaignError, "invalid Zink/OpenGL derivative receipt"
                ):
                    cts.load_zink_gl_derivative_receipt(receipt, identity)
                with tempfile.TemporaryDirectory() as temporary:
                    path = Path(temporary) / "receipt.json"
                    path.write_text(json.dumps(receipt), encoding="utf-8")
                    with self.assertRaisesRegex(
                        cts.CampaignError, "invalid derivative rootfs receipt"
                    ):
                        cts.load_derivative_receipt(path, identity)

    def test_zink_gl_receipt_requires_each_source_identity(self) -> None:
        identity = cts.load_identity()
        valid = zink_gl_derivative_receipt(identity)
        for field in ("protocol", "mesa", "cts", "vulkan_headers", "vulkan_loader"):
            for invalid in (None, {}, []):
                with self.subTest(field=field, invalid=invalid):
                    receipt = copy.deepcopy(valid)
                    receipt[field] = invalid
                    with self.assertRaisesRegex(cts.CampaignError, field):
                        cts.load_zink_gl_derivative_receipt(receipt, identity)

    def test_zink_gl_receipt_rejects_stale_and_mismatched_sources(self) -> None:
        identity = cts.load_identity()
        valid = zink_gl_derivative_receipt(identity)
        for field in ("protocol", "mesa", "cts", "vulkan_headers", "vulkan_loader"):
            changes = {
                "repository": "https://example.invalid/unrelated",
                "base_commit": "0" * 40,
                "base_tree": "0" * 40,
            }
            if "release_ref" in valid[field]:
                changes.update(release_ref="stale-tag", release_object="0" * 40)
            if "patched_tree" in valid[field]:
                changes["patched_tree"] = valid[field]["base_tree"]
            for key, value in changes.items():
                with self.subTest(field=field, key=key):
                    receipt = copy.deepcopy(valid)
                    receipt[field][key] = value
                    with self.assertRaisesRegex(cts.CampaignError, field):
                        cts.load_zink_gl_derivative_receipt(receipt, identity)

    def test_zink_gl_receipt_requires_patched_sources_to_say_so(self) -> None:
        identity = cts.load_identity()
        valid = zink_gl_derivative_receipt(identity)
        for field in ("protocol", "cts", "vulkan_headers", "vulkan_loader"):
            receipt = copy.deepcopy(valid)
            receipt[field]["patched"] = not receipt[field]["patched"]
            with self.subTest(field=field), self.assertRaisesRegex(cts.CampaignError, field):
                cts.load_zink_gl_derivative_receipt(receipt, identity)
        for field in ("vulkan_headers", "vulkan_loader"):
            receipt = copy.deepcopy(valid)
            receipt[field]["patches"] = [{"order": 1, "patch": "0001-unexpected.patch"}]
            with self.subTest(field=field), self.assertRaisesRegex(cts.CampaignError, field):
                cts.load_zink_gl_derivative_receipt(receipt, identity)

    def test_zink_gl_epoch_does_not_freeze_vulkan_image_or_build_identity(self) -> None:
        identity = cts.load_identity()
        receipt = zink_gl_derivative_receipt(identity)
        receipt["cts"]["build_id"] = "d" * 40
        _, contract = cts.load_zink_gl_derivative_receipt(receipt, identity)
        self.assertEqual(contract.sha256, "4" * 64)
        self.assertEqual(contract.glcts_sha256, "5" * 64)

    def test_zink_gl_receipt_requires_authenticated_protocol_generation(self) -> None:
        identity = cts.load_identity()
        valid = zink_gl_derivative_receipt(identity)
        for value in (None, {}, {"driver_headers": {}}):
            receipt = copy.deepcopy(valid)
            receipt["protocol"]["generation"] = value
            with self.subTest(value=value), self.assertRaisesRegex(cts.CampaignError, "protocol generation"):
                cts.load_zink_gl_derivative_receipt(receipt, identity)
        for key, value in (("registry_sha256", "0" * 64),
                           ("configure_argv", []), ("driver_headers", {})):
            receipt = copy.deepcopy(valid)
            receipt["protocol"]["generation"][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(cts.CampaignError, "protocol generat"):
                cts.load_zink_gl_derivative_receipt(receipt, identity)

    def test_zink_gl_receipt_requires_exact_offline_protocol_setup(self) -> None:
        identity = cts.load_identity()
        valid = zink_gl_derivative_receipt(identity)
        argv = valid["protocol"]["generation"]["configure_argv"]
        for wrap_mode in ("nofallback", "nodownload"):
            receipt = copy.deepcopy(valid)
            receipt["protocol"]["generation"]["configure_argv"][4] = f"--wrap-mode={wrap_mode}"
            cts.load_zink_gl_derivative_receipt(receipt, identity)
        changes = [
            ["true"],
            ["ninja", *argv[1:]],
            [argv[0], "configure", *argv[2:]],
            argv[:-1],
            [*argv, "--wrap-mode=default"],
            [*argv[:4], "--wrap-mode=default", argv[5]],
            [*argv[:4], "--wrap-mode=forcefallback", argv[5]],
            [*argv[:4], argv[5], argv[4]],
            [*argv[:5], "-Dunrelated=/prepared/sdk"],
        ]
        for invalid in changes:
            receipt = copy.deepcopy(valid)
            receipt["protocol"]["generation"]["configure_argv"] = invalid
            with self.subTest(argv=invalid), self.assertRaisesRegex(
                cts.CampaignError, "protocol generation command"
            ):
                cts.load_zink_gl_derivative_receipt(receipt, identity)

    def test_zink_gl_receipt_requires_normalized_protocol_input_paths(self) -> None:
        identity = cts.load_identity()
        valid = zink_gl_derivative_receipt(identity)
        for index, prefix in ((2, ""), (3, ""), (5, "-Dvulkan-headers-dir=")):
            for path in ("", "relative/input", "/home/../vm/input", "/home/./vm/input",
                         "//home/vm/input", "/home//vm/input", "/home/vm/input/",
                         "/home/vm/input/..", "/home/vm/input/.", "/", "/home/\x00/input"):
                receipt = copy.deepcopy(valid)
                receipt["protocol"]["generation"]["configure_argv"][index] = prefix + path
                diagnostic = ("protocol generation command" if not path and not prefix
                              else "protocol generation .* directory")
                with self.subTest(index=index, path=path), self.assertRaisesRegex(
                    cts.CampaignError, diagnostic
                ):
                    cts.load_zink_gl_derivative_receipt(receipt, identity)

    def test_zink_gl_receipt_requires_one_matching_mesa_protocol_binding(self) -> None:
        identity = cts.load_identity()
        valid = zink_gl_derivative_receipt(identity)
        argv = valid["mesa"]["configure_argv"]
        option = next(value for value in argv if value.startswith("-Dvenus-protocol-dir="))
        without = [value for value in argv if value != option]
        for invalid in (
            without,
            [*without, "-Dvenus-protocol-dir=/unrelated/generated"],
            [*argv, option],
            [*argv, "-Dvenus-protocol-dir=/unrelated/generated"],
            [*argv, "-Dvenus-protocol-dir"],
            [*argv, "-D", option[2:]],
        ):
            receipt = copy.deepcopy(valid)
            receipt["mesa"]["configure_argv"] = invalid
            with self.subTest(argv=invalid), self.assertRaisesRegex(
                cts.CampaignError, "Mesa protocol generation binding"
            ):
                cts.load_zink_gl_derivative_receipt(receipt, identity)

    def test_zink_gl_receipt_requires_the_exact_protocol_header_names(self) -> None:
        identity = cts.load_identity()
        valid = zink_gl_derivative_receipt(identity)
        schema = cts.vulkan_cts_receipt.load_json(cts.vulkan_cts_receipt.SCHEMA_PATH)
        expected = schema["$defs"]["protocolDriverHeader"]["enum"]
        headers = valid["protocol"]["generation"]["driver_headers"]
        self.assertEqual(len(expected), 38)
        self.assertEqual(set(headers), set(expected))
        for name in expected:
            receipt = copy.deepcopy(valid)
            changed = receipt["protocol"]["generation"]["driver_headers"]
            changed["vn_protocol_driver_unrecognized.h"] = changed.pop(name)
            with self.subTest(header=name), self.assertRaisesRegex(
                cts.CampaignError, "protocol generated header inventory"
            ):
                cts.load_zink_gl_derivative_receipt(receipt, identity)

    def test_zink_renderer_probe_requires_venus_and_rejects_software(self) -> None:
        valid = """device=/dev/dri/card0
device=/dev/dri/renderD128
x11_server=xorg
x11_dri3=1
x11_present=1
x11_glamor=1
x11_connected=1
x11_drm_fd=1
x11_blanking=off
guest_page_size=4096
OpenGL renderer string: zink Vulkan 1.4 (Virtio-GPU Venus)
"""
        cts.validate_renderer_probe(valid, renderer_backend="zink")
        with self.assertRaisesRegex(cts.CampaignError, "Virtio-GPU Venus"):
            cts.validate_renderer_probe(
                valid.replace("Virtio-GPU Venus", "unknown Vulkan device"),
                renderer_backend="zink",
            )
        with self.assertRaisesRegex(cts.CampaignError, "forbidden renderer"):
            cts.validate_renderer_probe(
                valid + "llvmpipe\n", renderer_backend="zink"
            )

    def test_complete_inventory_identity_is_exact_and_fail_closed(self) -> None:
        identity = json.loads(json.dumps(cts.load_identity()))
        digest = hashlib.sha256(b"complete caselist\n").hexdigest()
        identity["cts"]["complete_caselist_sha256"] = digest
        identity["cts"]["complete_caselist_case_count"] = 2
        payload = f"""schema_version=2
scope=gles2-gles3
complete=1
exported_package_count=2
complete_caselist_sha256={digest}
complete_caselist_case_count=2
registered_module=KHR-GLES2
registered_module=KHR-GLES3
""".encode()
        count, parsed_digest, modules, packages = cts.validate_inventory_report(
            payload, identity
        )
        self.assertEqual(count, 2)
        self.assertEqual(parsed_digest, digest)
        self.assertEqual(modules, ["KHR-GLES2", "KHR-GLES3"])
        self.assertEqual(packages, 2)

        cts.validate_inventory_report(payload, identity)
        with self.assertRaisesRegex(cts.CampaignError, "identity changed"):
            cts.validate_inventory_report(
                payload.replace(digest.encode(), b"0" * 64), identity
            )
        with self.assertRaisesRegex(cts.CampaignError, "not complete"):
            cts.validate_inventory_report(
                payload.replace(b"complete=1", b"complete=0"), identity
            )
        with self.assertRaisesRegex(cts.CampaignError, "scope is invalid"):
            cts.validate_inventory_report(
                payload.replace(b"scope=gles2-gles3", b"scope=all"), identity
            )
        with self.assertRaisesRegex(cts.CampaignError, "GLES scope"):
            cts.validate_inventory_report(
                payload.replace(
                    b"registered_module=KHR-GLES3",
                    b"registered_module=dEQP-GL45-ES3",
                ),
                identity,
            )
        with self.assertRaisesRegex(cts.CampaignError, "identity is invalid"):
            cts.validate_inventory_report(
                payload.replace(
                    b"exported_package_count=2",
                    b"exported_package_count=3",
                ),
                identity,
            )

    def test_typed_fraction_and_exact_case_reject_shell_input(self) -> None:
        self.assertEqual(cts.parse_fraction("0,8"), (0, 8))
        with self.assertRaises(Exception):
            cts.parse_fraction("8,8")
        with self.assertRaises(Exception):
            cts.parse_fraction("1/8")
        self.assertEqual(
            cts.exact_case(
                "KHR-GLES3.shaders.declarations.declarations."
                "missing_variable_name_vertex"
            ),
            "KHR-GLES3.shaders.declarations.declarations."
            "missing_variable_name_vertex",
        )
        for value in ("KHR-GLES3.*", "a,b", "$(id)", "case name", "../case"):
            with self.subTest(value=value), self.assertRaises(Exception):
                cts.exact_case(value)

    def test_apple_campaign_allow_list_is_exact_and_fail_closed(self) -> None:
        campaigns = cts.load_apple_campaigns()
        self.assertEqual(
            set(campaigns),
            {
                "gles2-identity-sentinel-v1",
                "gles3-declaration-sentinel-v1",
                "gles3-identity-sentinel-v1",
            },
        )
        campaign = campaigns["gles3-declaration-sentinel-v1"]
        self.assertEqual(campaign.api, "gles3")
        self.assertEqual(campaign.leaf.identifier, "gles3")
        self.assertEqual(campaign.selection.expected_count, 1)
        self.assertEqual(
            campaign.cases,
            (
                "KHR-GLES3.shaders.declarations.declarations."
                "missing_variable_name_vertex",
            ),
        )
        self.assertEqual(
            campaigns["gles2-identity-sentinel-v1"].selection.expected_count,
            5,
        )
        self.assertEqual(
            campaigns["gles3-identity-sentinel-v1"].selection.expected_count,
            12,
        )

        with tempfile.TemporaryDirectory() as temporary:
            invalid = Path(temporary) / "campaigns.json"
            invalid.write_text(
                json.dumps(
                    {
                        "schema_version": 2,
                        "campaigns": [
                            {
                                "id": "bad",
                                "api": "gles3",
                                "leaf": "gles3",
                                "cases": ["KHR-GLES3.*"],
                                "seed": 0,
                                "shard_timeout": 900,
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(cts.CampaignError, "invalid Apple"):
                cts.load_apple_campaigns(path=invalid)

    def test_macos_campaign_owns_its_exact_set_seed_and_timeout(self) -> None:
        options = cts.parse_arguments(
            ["--campaign", "gles3-identity-sentinel-v1", "--dry-run"]
        )
        campaign = options.campaigns[options.campaign]
        self.assertEqual(options.seed, campaign.seed)
        self.assertEqual(options.shard_timeout, campaign.shard_timeout)
        self.assertEqual(campaign.selection.expected_count, 12)
        for conflicting in (
            ["--leaf", "gles3"],
            ["--case", campaign.cases[0]],
            ["--seed", "1"],
            ["--shard-timeout", "1"],
        ):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
                SystemExit
            ):
                cts.parse_arguments(
                    [
                        "--campaign",
                        "gles3-identity-sentinel-v1",
                        *conflicting,
                    ]
                )

    def test_repeated_exact_cases_keep_their_order_and_reject_repeats(
        self,
    ) -> None:
        first = "KHR-GLES3.exposed_extensions.validate_extensions"
        second = "KHR-GLES3.info.version"
        options = cts.parse_arguments(
            [
                "--leaf",
                "gles3",
                "--api",
                "gles3",
                "--case",
                first,
                "--case",
                second,
            ]
        )
        self.assertEqual(options.case, [first, second])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
            SystemExit
        ):
            cts.parse_arguments(
                [
                    "--leaf",
                    "gles3",
                    "--api",
                    "gles3",
                    "--case",
                    first,
                    "--case",
                    first,
                ]
            )

    def test_caselist_parser_rejects_duplicates_and_invalid_rows(self) -> None:
        self.assertEqual(
            cts.parse_case_list("# comment\nKHR-GLES2.info.version\n\n"),
            ["KHR-GLES2.info.version"],
        )
        with self.assertRaisesRegex(cts.CampaignError, "duplicate case"):
            cts.parse_case_list("a.b\na.b\n")
        with self.assertRaisesRegex(cts.CampaignError, "invalid case"):
            cts.parse_case_list("a.b;uname\n")
        with self.assertRaisesRegex(cts.CampaignError, "empty"):
            cts.parse_case_list("# only a comment\n")

    def test_qpa_parser_requires_complete_unique_results(self) -> None:
        first = "KHR-GLES2.info.version"
        second = "KHR-GLES2.info.renderer"
        parsed = cts.validate_qpa_union(
            qpa(qpa_result(first), qpa_result(second, "NotSupported")),
            [first, second],
        )
        self.assertEqual(parsed, {first: "Pass", second: "NotSupported"})

        terminated = qpa(
            f"#beginTestCaseResult {first}\n#terminateTestCaseResult Crash\n"
        )
        self.assertEqual(cts.parse_qpa(terminated), {first: "Crash"})

        with self.assertRaisesRegex(cts.CampaignError, "no complete session"):
            cts.parse_qpa(qpa(qpa_result(first)).replace("#endSession\n", ""))
        # A guest that continued its caselist in a fresh CTS session transports
        # one document per session, read as one ordered result stream.
        self.assertEqual(
            cts.parse_qpa(qpa(qpa_result(first)) + qpa(qpa_result(second))),
            {first: "Pass", second: "Pass"},
        )
        with self.assertRaisesRegex(cts.CampaignError, "incomplete QPA"):
            cts.validate_qpa_union(qpa(qpa_result(first)), [first, second])
        with self.assertRaisesRegex(cts.CampaignError, "case order"):
            cts.validate_qpa_union(
                qpa(qpa_result(second), qpa_result(first)),
                [first, second],
            )
        with self.assertRaisesRegex(cts.CampaignError, "duplicate QPA"):
            cts.parse_qpa(qpa(qpa_result(first), qpa_result(first)))

        cts.validate_required_passes({first: "Pass"})
        with self.assertRaisesRegex(cts.CampaignError, "did not pass"):
            cts.validate_required_passes({second: "NotSupported"})
        cts.validate_official_runner_results(
            {
                first: "Pass",
                second: "NotSupported",
                "KHR-GLES2.info.vendor": "QualityWarning",
                "KHR-GLES2.info.extensions": "CompatibilityWarning",
            },
            (
                "Pass",
                "NotSupported",
                "QualityWarning",
                "CompatibilityWarning",
            ),
        )
        for status in ("Fail", "Waiver", "ResourceError", "Crash"):
            with self.subTest(status=status), self.assertRaisesRegex(
                cts.CampaignError, "official CTS runner rejects"
            ):
                cts.validate_official_runner_results(
                    {first: status},
                    (
                        "Pass",
                        "NotSupported",
                        "QualityWarning",
                        "CompatibilityWarning",
                    ),
                )

    def test_qpa_decoder_preserves_non_utf8_test_input(self) -> None:
        case = "KHR-GLES3.shaders.invalid_character"
        payload = qpa(qpa_result(case)).encode("utf-8").replace(
            b">result</Result>",
            b"><ShaderSource>#extension all\xa4 : warn</ShaderSource>"
            b"</Result>",
        )
        decoded = cts.decode_qpa(payload)

        self.assertEqual(
            decoded.encode("utf-8", errors="surrogateescape"), payload
        )
        self.assertEqual(cts.validate_qpa_union(decoded, [case]), {case: "Pass"})

    def test_qpa_case_path_must_match_container_marker(self) -> None:
        container = "KHR-GLES2.info.version"
        payload = qpa_result("KHR-GLES2.info.renderer").replace(
            "#beginTestCaseResult KHR-GLES2.info.renderer",
            f"#beginTestCaseResult {container}",
        )
        with self.assertRaisesRegex(cts.CampaignError, "case-path mismatch"):
            cts.parse_qpa(qpa(payload))

    def test_egl_config_qpa_is_ordered_and_fail_closed(self) -> None:
        configs = cts.parse_egl_config_qpa(config_qpa("gles3"), "gles3")
        self.assertEqual(
            configs,
            (
                cts.EglConfig(1, ("window", "pixmap", "pbuffer")),
                cts.EglConfig(2, ("pbuffer", "pixmap")),
            ),
        )
        self.assertEqual(configs[0].selected_surface_type, "window")
        self.assertEqual(configs[1].selected_surface_type, "pbuffer")
        self.assertRegex(cts.egl_config_digest(configs), r"^[0-9a-f]{64}$")

        with self.assertRaisesRegex(cts.CampaignError, "duplicate config"):
            cts.parse_egl_config_qpa(
                config_qpa("gles3", ("EGL(1): window", "EGL(1): pbuffer")),
                "gles3",
            )
        with self.assertRaisesRegex(cts.CampaignError, "no usable configs"):
            cts.parse_egl_config_qpa(config_qpa("gles2", ()), "gles2")
        with self.assertRaisesRegex(cts.CampaignError, "did not pass"):
            cts.parse_egl_config_qpa(
                config_qpa("gles2", status="Fail"), "gles2"
            )
        with self.assertRaisesRegex(cts.CampaignError, "unsupported row"):
            cts.parse_egl_config_qpa(
                config_qpa("gles2", ("EGL(1): surfaceless",)), "gles2"
            )

    def test_renderer_probe_rejects_every_software_negative_control(self) -> None:
        valid = """device=/dev/dri/card0
device=/dev/dri/renderD128
x11_server=xorg
x11_dri3=1
x11_present=1
x11_glamor=1
x11_connected=1
x11_drm_fd=1
x11_blanking=off
guest_page_size=4096
OpenGL renderer string: virgl (zink Vulkan 1.4(Apple M1 Ultra (MESA_KOSMICKRISP)))
"""
        cts.validate_renderer_probe(valid)
        for other in (
            valid.replace("guest_page_size=4096\n", ""),
            valid.replace("guest_page_size=4096", "guest_page_size=16384"),
        ):
            with self.assertRaisesRegex(
                cts.CampaignError, "4096-byte guest pages"
            ):
                cts.validate_renderer_probe(other)
        other_host = valid.replace(
            "(zink Vulkan 1.4(Apple M1 Ultra (MESA_KOSMICKRISP)))", "(Apple GPU)"
        )
        cts.validate_renderer_probe(
            valid, host_provider="zink-kosmickrisp"
        )
        with self.assertRaisesRegex(cts.CampaignError, "Zink/KosmicKrisp"):
            cts.validate_renderer_probe(
                other_host, host_provider="zink-kosmickrisp"
            )
        with self.assertRaisesRegex(cts.CampaignError, "Zink/KosmicKrisp"):
            cts.validate_renderer_probe(other_host)
        for forbidden in ("llvmpipe", "softpipe", "swrast", "Xvfb"):
            with self.subTest(forbidden=forbidden), self.assertRaisesRegex(
                cts.CampaignError, "forbidden renderer"
            ):
                cts.validate_renderer_probe(valid + forbidden)
        with self.assertRaisesRegex(cts.CampaignError, "renderD128"):
            cts.validate_renderer_probe(valid.replace("device=/dev/dri/renderD128\n", ""))
        with self.assertRaisesRegex(cts.CampaignError, "Xorg"):
            cts.validate_renderer_probe(valid.replace("x11_server=xorg\nx11_dri3=1\nx11_present=1\nx11_glamor=1\nx11_connected=1\nx11_drm_fd=1\nx11_blanking=off\n", ""))

    def test_native_x11_probe_requires_each_gate_and_rejects_other_servers(self) -> None:
        valid = "\n".join(cts.NATIVE_X11_MARKERS) + "\n"
        cts.validate_native_x11_probe(valid)
        for marker in cts.NATIVE_X11_MARKERS:
            with self.subTest(marker=marker), self.assertRaises(cts.CampaignError):
                cts.validate_native_x11_probe(valid.replace(marker + "\n", ""))
        for forbidden in ("Xwayland", "Xvfb"):
            with self.subTest(server=forbidden), self.assertRaises(cts.CampaignError):
                cts.validate_native_x11_probe(valid + forbidden)

    def test_display_virgl_does_not_substitute_for_client_renderer(self) -> None:
        evidence = "\n".join(cts.NATIVE_X11_MARKERS) + "\nx11_display_renderer=virgl\n"
        with self.assertRaisesRegex(cts.CampaignError, "did not report Virgl"):
            cts.validate_renderer_probe(evidence)
        with self.assertRaisesRegex(cts.CampaignError, "Zink over Virtio-GPU Venus"):
            cts.validate_renderer_probe(evidence, renderer_backend="zink")

    def test_final_campaign_virgl_probe_passes_as_virgl_and_fails_as_zink(self) -> None:
        """The probe two clean gl46 shards wrote when a renderer-blind
        verification step rejected them.

        Kept verbatim in the lines verification reads, including the
        `virtio-uml` transport and the `zink` the host provider contributes to
        the client string, because those are what a check written for Zink over
        Venus can mistake for its own stack.
        """
        recorded = """guest_page_size=4096

device=/dev/dri/card0
drm_transport=/dev/dri/card0:/sys/bus/platform/drivers/virtio-uml
device=/dev/dri/renderD128
drm_transport=/dev/dri/renderD128:/sys/bus/platform/drivers/virtio-uml
x11_server=xorg
x11_pid=343
x11_blanking=off
x11_dri3=1
x11_present=1
x11_glamor=1
x11_connected=1
x11_drm_fd=1
x11_display_renderer=virgl
Extended renderer info (GLX_MESA_query_renderer):
    Vendor: Mesa (0x1af4)
    Device: virgl (zink Vulkan 1.4(Apple M1 Ultra (MESA_KOSMICKRISP))) (0x1010)
OpenGL vendor string: Mesa
OpenGL renderer string: virgl (zink Vulkan 1.4(Apple M1 Ultra (MESA_KOSMICKRISP)))
OpenGL core profile version string: 4.6 (Core Profile) Mesa 26.2.1 (git-275ba84f2c)
OpenGL version string: 4.6 (Compatibility Profile) Mesa 26.2.1 (git-275ba84f2c)
OpenGL core profile renderer: virgl (zink Vulkan 1.4(Apple M1 Ultra (MESA_KOSMICKRISP)))
OpenGL compatibility profile renderer: virgl (zink Vulkan 1.4(Apple M1 Ultra (MESA_KOSMICKRISP)))
OpenGL ES profile renderer: virgl (zink Vulkan 1.4(Apple M1 Ultra (MESA_KOSMICKRISP)))
"""
        cts.validate_renderer_probe(
            recorded, "virgl", cts.host_backend_label("virgl")
        )
        with self.assertRaisesRegex(
            cts.CampaignError, "Zink over Virtio-GPU Venus"
        ):
            cts.validate_renderer_probe(
                recorded, "zink", cts.host_backend_label("zink")
            )

    def test_native_x11_setup_separates_display_driver_and_wayland(self) -> None:
        for wayland in (False, True):
            with self.subTest(wayland=wayland):
                setup = cts.native_x11_setup(wayland=wayland)
                syntax = subprocess.run(
                    ["sh", "-n"], input=setup, text=True, capture_output=True,
                    check=False,
                )
                self.assertEqual(syntax.returncode, 0, syntax.stderr)
                self.assertIn("MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu", setup)
                self.assertIn("GALLIUM_DRIVER=virgl", setup)
                self.assertIn("-u VK_DRIVER_FILES", setup)
                self.assertIn("/proc/$candidate/exe", setup)
                self.assertIn("/proc/$xorg_pid/fd/*", setup)
                self.assertIn('/sys/bus/platform/drivers/virtio-uml', setup)
                self.assertIn('drm_transport=%s:%s', setup)
                self.assertIn('unexpected DRM transport $transport', setup)
                self.assertLess(
                    setup.index("printf '\\n' >> \"$renderer\""),
                    setup.index('for device in /dev/dri/card0'),
                )
                self.assertIn("timeout 1 xdpyinfo", setup)
                self.assertIn("pgrep -x Xwayland", setup)
                self.assertIn("pgrep -x Xvfb", setup)
                self.assertEqual("exec /usr/bin/kwin_wayland" in setup, wayland)
                self.assertEqual("dbus_address=" in setup, wayland)
                self.assertNotIn(" --xwayland", setup)

    def test_native_x11_setup_disables_screen_blanking(self) -> None:
        # A CTS guest never receives input, so the screen saver and DPMS always
        # expire mid-run. A blanked screen turns the CRTC off, the Present
        # extension loses the CRTC it schedules against and falls back to its
        # 1 Hz fake vblank, and every FIFO presentation - the CTS client's and
        # the nested compositor's alike - then costs a second per frame.
        for wayland in (False, True):
            with self.subTest(wayland=wayland):
                setup = cts.native_x11_setup(wayland=wayland)
                for option in (
                    "BlankTime", "StandbyTime", "SuspendTime", "OffTime",
                ):
                    self.assertIn(f'Option "{option}" "0"', setup)
                self.assertIn("xset s off -dpms", setup)
                self.assertIn("DPMS is Disabled", setup)
                self.assertIn("x11_blanking=off", setup)
                # The server must be proven unblankable before any client
                # presents through it.
                self.assertLess(
                    setup.index("x11_blanking=off"),
                    setup.index("timeout 10 glxinfo -B"),
                )
        self.assertIn("x11_blanking=off", cts.NATIVE_X11_MARKERS)

    def test_nested_compositor_refuses_its_idle_lock_screen(self) -> None:
        # The compositor's idle lock occludes every client, and an occluded
        # surface is owed no frame callback, so a FIFO presentation waits out
        # its whole bound per frame. A CTS guest never produces the input that
        # would postpone the lock.
        self.assertNotIn("lockscreen", cts.native_x11_setup(wayland=False))
        setup = cts.native_x11_setup(wayland=True)
        self.assertIn("kwin_wayland --no-lockscreen", setup)
        self.assertIn("Autolock=false", setup)
        self.assertIn("wayland_lockscreen=off", setup)
        self.assertLess(
            setup.index("Autolock=false"), setup.index("kwin_wayland --no-"),
        )

    def test_native_display_cleanup_is_ordered_and_bounded(self) -> None:
        setup = cts.native_x11_setup(wayland=True)
        cleanup = setup.split('native_display="$output/native-display"')[0]
        # Model live recorded children and wrappers. Signals update their state;
        # no real processes are signaled by this lifecycle test.
        model = r"""
xorg_pid=3; xorg_stamp=3; xorg_launcher_pid=4; xorg_launcher_stamp=4
kwin_pid=1; kwin_stamp=1; kwin_launcher_pid=2; kwin_launcher_stamp=2
live_1=1; live_2=1; live_3=1; live_4=1
readlink() { case "$2" in /proc/1/exe) echo /usr/bin/kwin_wayland;; /proc/3/exe) echo /usr/lib/xorg/Xorg;; esac; }
native_process_alive() { [ -n "$1" ] || return 1; eval '[ "$live_'"$1"'" -eq 1 ]'; }
kill() { printf 'signal=%s:%s\n' "$1" "$2"; eval "live_$2=0"; }
wait() { printf 'reaped=%s\n' "$1"; }
native_display_cleanup
"""
        result = subprocess.run(["sh", "-eu"], input=cleanup + model,
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        signals = [line for line in result.stdout.splitlines() if line.startswith("signal=")]
        self.assertEqual(signals, ["signal=-TERM:1", "signal=-TERM:2",
                                  "signal=-TERM:3", "signal=-TERM:4"])
        self.assertIn("reaped=2", result.stdout)
        self.assertIn("reaped=4", result.stdout)
        stuck = model.replace('eval "live_$2=0"', ':').replace(
            'native_display_cleanup\n', 'sleep() { :; }\nnative_display_cleanup\n')
        result = subprocess.run(["sh", "-eu"], input=cleanup + stuck,
                                text=True, capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cleanup timed out: kwin", result.stdout)
        self.assertIn("signal=-KILL:1", result.stdout)
        self.assertNotIn("signal=-TERM:3", result.stdout)
        partial = subprocess.run(["sh", "-eu"], input=cleanup + "native_display_cleanup\n",
                                 text=True, capture_output=True, timeout=5)
        self.assertEqual(partial.returncode, 0, partial.stderr)

    def test_native_harness_has_no_competing_desktop(self) -> None:
        options = cts.parse_arguments(["--inventory", "--dry-run"])
        guest_files = cts.gpu_guest.StagedGuestFiles(
            Path("/tmp/native-wsi-test/files")
        )
        command = cts.native_wsi_harness_command(
            options, Path("/tmp/native-wsi-test"), cts.GUEST_SERVICE_NAME,
            guest_files=guest_files,
            width=800, height=600, display="native", rootfs_type="erofs",
        )
        self.assertEqual(
            command[command.index("--unit") + 1], "uml-cts.target"
        )
        self.assertIn("--no-auto-poweroff", command)
        self.assertFalse(any("systemd.mask=" in argument for argument in command))
        self.assertNotIn("--command", command)
        target = next(
            data.decode()
            for host, (data, _) in guest_files.files.items()
            if host.name.endswith("-uml-cts.target")
        )
        self.assertIn(
            f"--guest-file={cts.CTS_TARGET_PATH}=", " ".join(command)
        )
        self.assertIn("Requires=multi-user.target\n", target)
        self.assertIn("Wants=uml-vk-gl-cts.service\n", target)
        conflicts = next(
            line.split("=", 1)[1].split()
            for line in target.splitlines()
            if line.startswith("Conflicts=")
        )
        # The display manager and the fixed-VT getties take the VT the
        # campaign's X server holds; no suite uses the container runtime, and
        # tearing its BPF programs down from a half-started state wedges a
        # guest that is powering off.
        self.assertEqual(
            set(conflicts),
            {
                "display-manager.service", "sddm.service",
                "getty@tty0.service", "getty@tty1.service",
                "docker.service", "docker.socket", "containerd.service",
            },
        )
        self.assertIn("--gpu-width=800", command)
        self.assertIn("--gpu-height=600", command)
        self.assertNotIn("--gpu-vnc-port=0", command)
        self.assertNotIn("display-manager.service", cts.CTS_SERVICE)
        with self.assertRaises(cts.CampaignError):
            cts.cts_target("uml-vk-gl-cts.service\nWants=evil.service")

    def test_host_backend_refuses_retired_providers_and_requires_zink(self) -> None:
        cts.validate_host_backend("Virgl Zink/KosmicKrisp provider: hardware\n")
        for retired in ("Virgl CGL capset probe", "Virgl ANGLE/Metal provider: hardware"):
            with self.subTest(retired=retired), self.assertRaisesRegex(
                cts.CampaignError, "retired host GL provider"
            ):
                cts.validate_host_backend(
                    retired + "\nVirgl Zink/KosmicKrisp provider: hardware\n"
                )
        with self.assertRaisesRegex(cts.CampaignError, "marker is missing"):
            cts.validate_host_backend("virgl started\n")

    def test_gpu_capability_and_native_frame_reports_are_fail_closed(self) -> None:
        report = gpu_capability_report()
        parsed = cts.parse_gpu_capability_report(report)
        self.assertEqual(
            [context["requested"] for context in parsed["host_contexts"]],
            [[4, 6]],
        )
        self.assertEqual(
            [capset["id"] for capset in parsed["virgl_capsets"]], [1, 2]
        )
        self.assertEqual(
            parsed["virgl_capsets"][0]["sha256"],
            hashlib.sha256(struct.pack("<I", 1) + bytes(304)).hexdigest(),
        )
        self.assertEqual(
            parsed["virgl_capsets"][1]["decoded"]["v2"]["renderer"],
            "renderer",
        )
        self.assertNotIn("venus_capset", parsed)
        venus = cts.parse_gpu_capability_report(
            gpu_capability_report(venus_capset=True)
        )
        self.assertEqual(
            [capset["id"] for capset in venus["virgl_capsets"]], [1, 2]
        )
        self.assertEqual(venus["venus_capset"]["id"], cts.VENUS_CAPSET_ID)
        self.assertEqual(venus["venus_capset"]["size"], 16)
        with self.assertRaisesRegex(cts.CampaignError, "omitted a Virgl capset"):
            cts.parse_gpu_capability_report(
                "\n".join(
                    line
                    for line in gpu_capability_report(venus_capset=True)
                    .splitlines()
                    if "|id=2|" not in line
                )
            )
        with self.assertRaisesRegex(cts.CampaignError, "omitted a Virgl capset"):
            cts.parse_gpu_capability_report(
                gpu_capability_report(venus_capset=True).replace(
                    f"id={cts.VENUS_CAPSET_ID}|", "id=3|"
                )
            )
        with self.assertRaisesRegex(cts.CampaignError, "omitted the desktop GL 4.6"):
            cts.parse_gpu_capability_report(
                "\n".join(
                    line
                    for line in report.splitlines()
                    if "requested=4.6" not in line
                )
            )
        with self.assertRaisesRegex(cts.CampaignError, "size changed"):
            cts.parse_gpu_capability_report(
                report.replace(
                    "id=1|version=1|size=308", "id=1|version=1|size=309"
                )
            )

        final = (
            "Native display: direct Metal texture presentation\n"
            "Native display final: guest 8 presented 7 dropped 1 cpu-fallback 0\n"
        )
        self.assertEqual(
            cts.parse_native_display_final(final)["cpu_fallback_frames"], 0
        )
        with self.assertRaisesRegex(cts.CampaignError, "CPU scanout fallback"):
            cts.parse_native_display_final(
                final.replace("cpu-fallback 0", "cpu-fallback 1")
            )

    def test_artifact_frames_round_trip_and_tolerate_console_prefixes(self) -> None:
        transcript = "".join(
            "[  10.0] service: " + line + "\r\n"
            for line in framed_artifact("results.qpa", b"qpa payload").splitlines()
        )
        self.assertEqual(
            cts.decode_artifacts(transcript),
            {"results.qpa": b"qpa payload"},
        )

    def test_artifact_frames_ignore_echoed_launcher_source(self) -> None:
        transcript = (
            "    printf 'UML_CTS_ARTIFACT_BEGIN|%s|gzip+base64|%s|%s\\n'\n"
            + framed_artifact("results.qpa", b"qpa payload")
        )
        self.assertEqual(
            cts.decode_artifacts(transcript),
            {"results.qpa": b"qpa payload"},
        )

        with self.assertRaisesRegex(cts.CampaignError, "malformed"):
            cts.decode_artifacts(
                "[  10.0] service: UML_CTS_ARTIFACT_BROKEN|results.qpa\n"
            )

    def test_artifact_frames_reject_path_checksum_and_chunk_failures(self) -> None:
        unsafe = framed_artifact("../results.qpa", b"payload")
        with self.assertRaisesRegex(cts.CampaignError, "unsafe"):
            cts.decode_artifacts(unsafe)

        corrupt = framed_artifact("results.qpa", b"payload")
        digest = hashlib.sha256(gzip.compress(b"payload", mtime=0)).hexdigest()
        replacement = ("0" if digest[0] != "0" else "1") + digest[1:]
        corrupt = corrupt.replace(digest, replacement, 1)
        with self.assertRaisesRegex(cts.CampaignError, "checksum"):
            cts.decode_artifacts(corrupt)

        truncated = framed_artifact("results.qpa", b"payload")
        truncated = "\n".join(truncated.splitlines()[:-2]) + "\n"
        with self.assertRaisesRegex(cts.CampaignError, "missing its trailer"):
            cts.decode_artifacts(truncated)

    def test_artifact_transport_acknowledges_only_canonical_frames(self) -> None:
        frames = framed_artifact("results.qpa", b"payload").splitlines()
        acknowledgements = [
            cts.artifact_acknowledgement(frame) for frame in frames
        ]
        self.assertEqual(
            acknowledgements[0],
            "UML_CTS_ARTIFACT_ACK|results.qpa|begin",
        )
        self.assertEqual(
            acknowledgements[-1],
            "UML_CTS_ARTIFACT_ACK|results.qpa|end",
        )
        self.assertTrue(
            all(cts.ACK_ARTIFACT.fullmatch(item) for item in acknowledgements)
        )
        with self.assertRaisesRegex(cts.CampaignError, "non-frame"):
            cts.artifact_acknowledgement("UML_CTS_STATUS|rc=0")

        host, guest = socket.socketpair()
        host.setblocking(False)
        guest.setblocking(False)
        transport = cts.AcknowledgedArtifactSocket(Path("/unused"))
        transport.connection = host
        transport.connected = True
        try:
            guest.sendall((frames[0] + "\n").encode("ascii"))
            transport.service()
            self.assertEqual(
                guest.recv(256),
                b"UML_CTS_ARTIFACT_ACK|results.qpa|begin\n",
            )
            guest.close()
            transport.service()
            self.assertEqual(transport.finish(), frames[0] + "\n")
        finally:
            transport.close()
            guest.close()

    def test_artifact_writer_keeps_outputs_beneath_new_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "artifacts"
            cts.write_artifacts({"summary.txt": b"cts_rc=0\n"}, destination)
            self.assertEqual(
                (destination / "summary.txt").read_bytes(),
                b"cts_rc=0\n",
            )
            with self.assertRaises(FileExistsError):
                cts.write_artifacts({"summary.txt": b"again"}, destination)

    def test_launcher_uses_only_frozen_guest_inputs_and_bounded_output(self) -> None:
        leaf = cts.leaf_identities(cts.load_identity())["gles3"]
        selection = cts.Selection(leaf, (), 1, 8)
        launcher = cts.build_guest_launcher(
            selection,
            seed=7,
            timeout=300,
            rootfs_contract=erofs_contract(),
        )
        self.assertIn(leaf.path, launcher)
        self.assertIn(leaf.sha256, launcher)
        self.assertIn("/opt/vk-gl-cts/openglcts/glcts", launcher)
        self.assertIn("--deqp-gl-context-type=egl", launcher)
        self.assertIn("--deqp-log-images=disable", launcher)
        self.assertIn("--deqp-test-oom=disable", launcher)
        self.assertIn("timeout --signal=TERM --kill-after=30 300", launcher)
        self.assertIn("runuser -u uml", launcher)
        self.assertIn("pgrep -x Xwayland", launcher)
        self.assertIn(r"tr '\000' '\n'", launcher)
        self.assertNotIn("\0", launcher)
        self.assertIn('DISPLAY="$xdisplay"', launcher)
        self.assertNotIn("DISPLAY=:0", launcher)
        self.assertIn("pgrep -x Xvfb", launcher)
        self.assertIn("/dev/dri/renderD128", launcher)
        self.assertIn(
            'actual_page_size=$(getconf PAGESIZE)\n'
            '[ "$actual_page_size" -eq 4096 ] || {\n'
            '    echo "UML_CTS_ERROR: guest page size $actual_page_size does '
            'not match 4096"\n'
            '    exit 1\n'
            '}\n'
            "printf 'guest_page_size=%s\n' \"$actual_page_size\" "
            '>> "$renderer"\n',
            launcher,
        )
        self.assertIn("UML_CTS_ARTIFACT_BEGIN", launcher)
        self.assertIn("systemctl poweroff", launcher)
        self.assertNotIn("apt ", launcher)
        self.assertNotIn("curl ", launcher)
        self.assertNotIn("MESA_GL_VERSION_OVERRIDE", launcher)
        self.assertNotIn("MESA_GLES_VERSION_OVERRIDE", launcher)
        self.assertNotIn("/dev/ubda", launcher)
        self.assertNotIn("\n+    ", launcher)
        exact_launcher = cts.build_guest_launcher(
            cts.Selection(
                leaf,
                (
                    "KHR-GLES3.shaders.declarations.declarations."
                    "missing_variable_name_vertex",
                    "KHR-GLES3.exposed_extensions.validate_extensions",
                ),
                0,
                1,
            ),
            seed=7,
            timeout=300,
            rootfs_contract=erofs_contract(),
        )
        self.assertIn(
            "exact case is absent from selected must-pass leaf",
            exact_launcher,
        )
        for generated_launcher in (launcher, exact_launcher):
            syntax = subprocess.run(
                ["sh", "-n"],
                input=generated_launcher,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(syntax.returncode, 0, syntax.stderr)

        virtio_launcher = cts.build_guest_launcher(
            selection,
            seed=7,
            timeout=300,
            rootfs_contract=erofs_contract(),
            artifact_transport="virtio-serial",
        )
        self.assertIn("artifact_transport=virtio-serial", virtio_launcher)
        self.assertIn(
            f"channel={cts.VIRTIO_SERIAL_GUEST_DEVICE}",
            virtio_launcher,
        )
        protocol = (
            REPO_ROOT / "darwin/device/console/protocol.hpp"
        ).read_text(encoding="utf-8")
        self.assertIn(f'"{cts.VIRTIO_SERIAL_PORT_NAME}"', protocol)
        self.assertNotIn('stty -F "$channel"', virtio_launcher)
        self.assertIn('exec 3<> "$channel"', virtio_launcher)
        self.assertIn("UML_CTS_ARTIFACT_ACK|$name|$index", virtio_launcher)
        self.assertIn("timeout --signal=TERM 30 head -n 1", virtio_launcher)
        syntax = subprocess.run(
            ["sh", "-n"],
            input=virtio_launcher,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(syntax.returncode, 0, syntax.stderr)
        with self.assertRaisesRegex(cts.CampaignError, "transport"):
            cts.build_guest_launcher(
                selection,
                7,
                300,
                erofs_contract(),
                artifact_transport="virtiofs",
            )

    def test_inventory_launcher_scopes_the_real_display_to_gles(self) -> None:
        launcher = cts.build_guest_launcher(
            None, 0, 900, erofs_contract()
        )
        self.assertIn("--deqp-runmode=txt-caselist", launcher)
        self.assertIn(
            "'--deqp-caselist-export-file=/run/uml-cts/${packageName}-cases."
            "${typeExtension}'",
            launcher,
        )
        self.assertIn("--deqp-duplicate-case-name-check=enable", launcher)
        self.assertIn("--deqp-log-filename=/run/uml-cts/results.qpa", launcher)
        self.assertIn("--deqp-case=KHR-GLES2.*", launcher)
        self.assertIn("--deqp-case=KHR-GLES3.*", launcher)
        tokens = shlex.split(launcher.replace("\\\n", ""), comments=True)
        inventory_shells = [
            tokens[index + 2] for index, token in enumerate(tokens[:-2])
            if token == "/bin/sh" and tokens[index + 1] == "-c"
        ]
        self.assertEqual(len(inventory_shells), 2)  # checkpoint and direct branches
        for shell in inventory_shells:
            modules = shell.split(" &&\n")
            self.assertEqual(len(modules), len(cts.GLES_INVENTORY_MODULES))
            for module in modules:
                arguments = shlex.split(module)
                self.assertEqual(arguments[0], "/opt/vk-gl-cts/openglcts/glcts")
                self.assertIn("--deqp-egl-display-type=x11", arguments)
        self.assertLess(
            launcher.index("--deqp-case=KHR-GLES2.*"),
            launcher.index("--deqp-case=KHR-GLES3.*"),
        )
        self.assertNotIn("dEQP-GL45", launcher)
        self.assertNotIn("--deqp-caselist-file=", launcher)
        self.assertIn(
            'package_caselist="$output/$inventory_package-cases.txt"',
            launcher,
        )
        self.assertIn("scope=gles2-gles3", launcher)
        self.assertIn('emit_artifact inventory.txt "$inventory_report"', launcher)
        self.assertIn("x11_server=xorg", launcher)
        self.assertIn("--deqp-egl-display-type=x11", launcher)
        self.assertIn("export EGL_PLATFORM=x11", launcher)
        self.assertIn("systemctl poweroff", launcher)
        syntax = subprocess.run(
            ["sh", "-n"],
            input=launcher,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(syntax.returncode, 0, syntax.stderr)
        options = cts.parse_arguments(["--inventory", "--dry-run"])
        self.assertTrue(options.inventory)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
            SystemExit
        ):
            cts.parse_arguments(
                ["--inventory", "--leaf", "gles3", "--api", "gles3"]
            )

    def test_config_inventory_launcher_runs_one_exact_cts_case(self) -> None:
        launcher = cts.build_guest_launcher(
            None,
            0,
            900,
            erofs_contract(),
            config_inventory_api="gles3",
        )
        self.assertIn("--deqp-case=CTS-Configs.es3", launcher)
        self.assertIn("--deqp-gl-context-type=egl", launcher)
        self.assertNotIn("--deqp-runmode=txt-caselist", launcher)
        self.assertIn("hierarchy_inventory_mode=0", launcher)
        self.assertIn("selected_mode=0", launcher)
        self.assertIn("emit_artifact results.qpa", launcher)
        syntax = subprocess.run(
            ["sh", "-n"],
            input=launcher,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(syntax.returncode, 0, syntax.stderr)
        options = cts.parse_arguments(
            ["--config-inventory", "gles3", "--display", "vnc", "--dry-run"]
        )
        self.assertEqual(options.config_inventory, "gles3")
        self.assertIsNone(options.matrix_run)

    def test_config_inventory_evidence_revalidates_its_qpa(self) -> None:
        contract = erofs_contract()
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            run = write_config_inventory_evidence(directory, "gles2", contract)
            configs, digest, _ = cts.load_config_inventory_evidence(
                run, "gles2", "4k", "vnc", contract
            )
            self.assertEqual(len(configs), 2)
            self.assertEqual(digest, cts.egl_config_digest(configs))

            qpa_path = directory / "artifacts" / "results.qpa"
            qpa_path.write_bytes(qpa_path.read_bytes() + b"changed")
            with self.assertRaisesRegex(cts.CampaignError, "identity changed"):
                cts.load_config_inventory_evidence(
                    run, "gles2", "4k", "vnc", contract
                )

    def test_gl46_config_inventory_binds_the_opengl_stack_it_was_captured_on(
        self,
    ) -> None:
        identity = cts.load_identity()
        runtime = {"emulator_sha256": "a" * 64, "kosmickrisp_sha256": "b" * 64}
        receipt = zink_gl_derivative_receipt(identity)
        _, contract = cts.load_zink_gl_derivative_receipt(receipt, identity)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for renderer, other in (("zink", "virgl"), ("virgl", "zink")):
                run = write_config_inventory_evidence(
                    root / renderer, "gl46", contract,
                    runtime_identity=runtime, renderer=renderer,
                )
                configs, digest, _ = cts.load_config_inventory_evidence(
                    run, "gl46", "4k", "vnc", contract, runtime,
                    (1280, 720), renderer,
                )
                self.assertEqual(digest, cts.egl_config_digest(configs))
                with self.assertRaisesRegex(
                    cts.CampaignError, "does not match backend"
                ):
                    cts.load_config_inventory_evidence(
                        run, "gl46", "4k", "vnc", contract, runtime,
                        (1280, 720), other,
                    )

    def test_matrix_case_subset_narrows_a_shard_and_names_itself(self) -> None:
        """An official shard can be narrowed to named cases.

        Isolating one failing case is the only way to tell a real defect from
        an ordering artefact, and the developer leaf path cannot do it on a
        sealed image: its leaf identity comes from the checked-in identity
        file, not from the image's own receipt. Narrowing the matrix shard
        keeps the receipt-bound leaf gate exactly as it is.
        """
        identity = cts.load_identity()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt_path = root / "receipt.json"
            receipt_path.write_text(
                json.dumps(derivative_receipt(identity)), encoding="utf-8"
            )
            _, contract = cts.load_derivative_receipt(receipt_path, identity)
            inventory_run = write_config_inventory_evidence(
                root / "inventory", "gles3", contract
            )
            cases = ["dEQP-GLES3.functional.a", "dEQP-GLES3.functional.b"]
            common = [
                "--matrix", "gles3",
                "--config-inventory-run", str(inventory_run),
                "--egl-config-index", "1",
                "--fraction", "3,8",
                "--display", "vnc",
                "--rootfs", str(root / "rootfs.ext4"),
                "--rootfs-type", "ext4",
                "--rootfs-receipt", str(receipt_path),
                "--qualification",
                "--dry-run",
            ]
            selector = [argument for case in cases for argument in ("--case", case)]
            options = cts.parse_arguments(common + selector)
            selection = options.matrix_run.selection
            # The shard is still the shard it was, and the request narrows it.
            self.assertEqual(selection.fraction_index, 3)
            self.assertEqual(selection.fraction_count, 8)
            self.assertEqual(selection.exact_cases, tuple(cases))
            self.assertEqual(selection.expected_count, len(cases))

            for backend in ("virgl", "zink"):
                with self.subTest(renderer=backend):
                    launcher = cts.build_guest_launcher(
                        selection,
                        options.seed,
                        options.shard_timeout,
                        options.rootfs_contract,
                        matrix=options.matrix_run,
                        renderer_backend=backend,
                    )
                    # The guest derives the shard, keeps the requested cases in
                    # the shard's order, and proves the two agree.
                    self.assertIn("shard_index=3", launcher)
                    self.assertIn("shard_count=8", launcher)
                    self.assertIn('"$selected.shard"', launcher)
                    self.assertIn('grep -Fx -f "$selected.request"', launcher)
                    self.assertIn(
                        "exact cases are not an ordered subset "
                        "of the selected matrix shard",
                        launcher,
                    )
                    for case in cases:
                        self.assertIn(case, launcher)
                    # The receipt-bound leaf gate is untouched.
                    self.assertIn(
                        "selected must-pass leaf identity changed", launcher
                    )

            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                cts.parse_arguments(common + selector + ["--case", cases[0]])
            self.assertIn("--case must not repeat a case", stderr.getvalue())

    def test_developer_shard_still_takes_cases_or_a_fraction(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
            cts.parse_arguments(
                [
                    "--leaf", "gles3",
                    "--api", "gles3",
                    "--case", "dEQP-GLES3.functional.a",
                    "--fraction", "3,8",
                    "--dry-run",
                ]
            )
        self.assertIn("--case or --fraction, not both", stderr.getvalue())

    def test_matrix_cli_requires_authenticated_discovery_and_exact_xml(self) -> None:
        identity = cts.load_identity()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            receipt_path = root / "receipt.json"
            receipt_path.write_text(
                json.dumps(derivative_receipt(identity)), encoding="utf-8"
            )
            _, contract = cts.load_derivative_receipt(receipt_path, identity)
            inventory_run = write_config_inventory_evidence(
                root / "inventory", "gles3", contract
            )
            common = [
                "--matrix",
                "gles3",
                "--config-inventory-run",
                str(inventory_run),
                "--egl-config-index",
                "1",
                "--fraction",
                "3,8",
                "--display",
                "vnc",
                "--rootfs",
                str(root / "rootfs.ext4"),
                "--rootfs-type",
                "ext4",
                "--rootfs-receipt",
                str(receipt_path),
                "--qualification",
                "--dry-run",
            ]
            options = cts.parse_arguments(common)
            matrix = options.matrix_run
            self.assertEqual(
                options.runner_oracle.path,
                "tests/integration/gpu/vk_gl_cts_runner_oracle.json",
            )
            self.assertRegex(options.runner_oracle.sha256, r"^[0-9a-f]{64}$")
            self.assertEqual(matrix.egl_config_index, 1)
            self.assertEqual(matrix.egl_config.identifier, 2)
            self.assertFalse(
                matrix.configuration.use_for_first_egl_config
            )
            self.assertEqual(matrix.selection.expected_count, 812)
            launcher = cts.build_guest_launcher(
                matrix.selection,
                options.seed,
                options.shard_timeout,
                options.rootfs_contract,
                matrix=matrix,
            )
            self.assertIn("--deqp-gl-config-id=2", launcher)
            self.assertIn("--deqp-surface-type=pbuffer", launcher)
            self.assertIn("--deqp-base-seed=1", launcher)
            self.assertIn("--deqp-watchdog=disable", launcher)
            self.assertIn(
                "awk -v shard_index=3 -v shard_count=8", launcher
            )
            self.assertNotIn("awk -v index=", launcher)
            self.assertIn(identity["mustpass"]["gles_xml"]["sha256"], launcher)
            self.assertNotIn("--deqp-base-seed=0", launcher)
            syntax = subprocess.run(
                ["sh", "-n"],
                input=launcher,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(syntax.returncode, 0, syntax.stderr)
            harness = cts.build_harness_command(
                options, matrix.selection, root / "artifacts"
            )
            self.assertEqual(
                len(
                    [
                        argument
                        for argument in harness
                        if argument.startswith("--virtio-serial-socket=")
                    ]
                ),
                1,
            )

            without_qualification = [
                item for item in common if item != "--qualification"
            ]
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
                SystemExit
            ):
                cts.parse_arguments(without_qualification)
            wrong_display = list(common)
            wrong_display[wrong_display.index("vnc")] = "native"
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(
                SystemExit
            ):
                cts.parse_arguments(wrong_display)

    def test_kernarium_relay_accepts_every_driver_artifact_frame(self) -> None:
        """The application relays what the drivers it hosts actually send.

        The relay validates each frame before acknowledging it, so a bound
        below a driver's own encoding silently truncates that campaign's
        evidence instead of failing the build. Both drivers are checked here
        because only one of them is read when a campaign is generated.
        """
        runtime = (
            REPO_ROOT / "Kernarium/Runtime/KernariumHarness.mm"
        ).read_text(encoding="utf-8")
        gl_launcher = (INTEGRATION_GPU / "run_vk_gl_cts.py").read_text(
            encoding="utf-8"
        )
        vulkan_launcher = (INTEGRATION_GPU / "run_vk_cts.py").read_text(
            encoding="utf-8"
        )
        lines = [
            int(match)
            for match in re.findall(r"base64 -w (\d+) ", gl_launcher)
        ]
        vulkan_line = re.search(
            r"^ARTIFACT_BASE64_LINE_LENGTH = (\d+) \* (\d+)$",
            vulkan_launcher,
            re.MULTILINE,
        )
        self.assertIsNotNone(vulkan_line)
        lines.append(int(vulkan_line.group(1)) * int(vulkan_line.group(2)))
        self.assertTrue(lines)
        widest = max(lines)
        chunk_bound = int(
            re.search(r"text\.size\(\) <= (\d+)U", runtime).group(1)
        )
        frame_bound = int(
            re.search(
                r"maximumFrameBytes = (\d+)U", runtime
            ).group(1)
        )
        self.assertGreaterEqual(chunk_bound, widest)
        # A frame is one chunk plus its header fields.
        self.assertGreater(frame_bound, chunk_bound)
        for name in (
            "renderer.txt",
            "selected.txt",
            "expected.txt",
            "results.qpa",
            "summary.txt",
        ):
            self.assertIn(f'"{name}"', runtime)
        self.assertIn('segmentPrefix = "results.qpa."', runtime)

    def test_kernarium_campaigns_are_the_common_page_specific_launchers(
        self,
    ) -> None:
        import kernarium_campaigns
        import run_vk_cts as vulkan

        derivative_receipt = receipt_fixtures.current_epoch_receipt_file(
            self, receipt_fixtures.ZINK_GL_RECEIPT
        )
        vulkan_receipt = receipt_fixtures.current_epoch_receipt_file(
            self, receipt_fixtures.VULKAN_RECEIPT
        )
        patches = (
            mock.patch.object(
                kernarium_campaigns, "GL_DERIVATIVE_RECEIPT_PATH",
                derivative_receipt,
            ),
            mock.patch.object(vulkan, "RECEIPT_PATH", vulkan_receipt),
        )
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

        def launcher(identifier: str, image_type: str) -> str:
            campaign = kernarium_campaigns.build(
                identifier, image_type=image_type
            )
            self.assertEqual(campaign.unit, "uml-cts.target")
            path = next(
                path for path, (_, mode) in campaign.files.items()
                if mode == 0o755 and not path.endswith("-run")
            )
            text = campaign.files[path][0].decode()
            self.assertNotIn("/dev/ubda", text)
            return text

        identifiers = kernarium_campaigns.identifiers()
        self.assertEqual(len(identifiers), len(set(identifiers)))
        for identifier in (
            "gles3-declaration-sentinel-v1",
            "gles2-identity-sentinel-v1",
            "gles3-identity-sentinel-v1",
            "gles2-config-inventory-v1",
            "gles3-config-inventory-v1",
            "gles2-matrix-cfg00-000of001-v1",
            "gles3-matrix-cfg19-003of004-v1",
            "gl46-matrix-cfg00-xml01-001of032-virgl-v1",
            "gl46-matrix-cfg00-xml01-001of032-zink-v1",
            "vulkan-info-development-v1",
            "vulkan-info-receipt-smoke-v1",
            "vulkan-mustpass-03of16-v4",
        ):
            self.assertIn(identifier, identifiers)
        self.assertNotIn("vulkan-mustpass-03of16-v3", identifiers)

        sentinel = kernarium_campaigns.build(
            "gles3-declaration-sentinel-v1", image_type="erofs",
        )
        self.assertEqual(
            {path: mode for path, (_, mode) in sentinel.files.items()},
            {
                cts.GUEST_LAUNCHER_PATH: 0o755,
                cts.GUEST_SERVICE_PATH: 0o644,
                cts.CTS_TARGET_PATH: 0o644,
            },
        )
        self.assertEqual(
            sentinel.files[cts.CTS_TARGET_PATH][0].decode(),
            cts.cts_target("uml-vk-gl-cts.service"),
        )
        self.assertEqual(
            set(sentinel.payload().files),
            {f"/payload{path}" for path in sentinel.files},
        )
        text = launcher("gles3-declaration-sentinel-v1", "erofs")
        self.assertIn("does not match 4096", text)
        self.assertIn("UML_CTS_ARTIFACT_BEGIN", text)
        self.assertIn("x11_server=xorg", text)
        ext4 = launcher("gles3-declaration-sentinel-v1", "ext4")
        self.assertIn("3b59fbc9-4b35-4a2b-890d-286a77b04afd", ext4)
        self.assertIn(
            "9bb567b4db930fce28e2dc893a09fa0bd5b7c046f72d0807c3546ae4311dc1d1",
            ext4,
        )
        self.assertIn(
            "--deqp-case=CTS-Configs.es2",
            launcher("gles2-config-inventory-v1", "erofs"),
        )
        self.assertIn(
            "--deqp-caselist-file=/run/uml-cts/selected.txt",
            launcher("gles3-matrix-cfg19-003of004-v1", "erofs"),
        )
        self.assertIn(
            "aosp_mustpass/main/gl45-es31-main.txt",
            launcher("gl46-matrix-cfg00-xml01-001of032-zink-v1", "ext4"),
        )
        with self.assertRaisesRegex(
            cts.CampaignError, "requires the sealed ext4 derivative"
        ):
            kernarium_campaigns.build(
                "gl46-matrix-cfg00-xml01-001of032-virgl-v1",
                image_type="erofs",
            )

        development = kernarium_campaigns.build(
            "vulkan-info-development-v1", image_type="erofs"
        )
        self.assertIn(
            "expected no-device status 137",
            development.files[kernarium_campaigns.VULKAN_WRAPPER_PATH][0]
            .decode(),
        )
        case_list = development.files[vulkan.GUEST_CASE_LIST_PATH][0]
        self.assertIn(
            hashlib.sha256(case_list).hexdigest(),
            launcher("vulkan-info-development-v1", "erofs"),
        )
        self.assertIn(
            "Wants=uml-vulkan-cts.service\n",
            development.files[cts.CTS_TARGET_PATH][0].decode(),
        )
        with self.assertRaisesRegex(cts.CampaignError, "frozen EROFS image"):
            kernarium_campaigns.build(
                "vulkan-info-development-v1", image_type="ext4",
            )
        smoke = launcher("vulkan-info-receipt-smoke-v1", "ext4")
        self.assertIn(cts.sha256_file(vulkan_receipt), smoke)
        self.assertIn("/usr/lib/aarch64-linux-gnu/libvulkan.so.1.4.363", smoke)
        self.assertIn("formal_conformance=false", smoke)
        with self.assertRaisesRegex(cts.CampaignError, "ext4 derivative"):
            kernarium_campaigns.build(
                "vulkan-info-receipt-smoke-v1", image_type="erofs",
            )
        mustpass = launcher("vulkan-mustpass-03of16-v4", "ext4")
        self.assertIn(
            "pipeline.fast_linked_library.render_to_image.core.2d_array."
            "huge.width_height_layers.r8g8b8a8_unorm",
            mustpass,
        )
        self.assertIn("pause_limit=$((pause_start + 20))", mustpass)
        with self.assertRaisesRegex(cts.CampaignError, "no Kernarium campaign"):
            kernarium_campaigns.build(
                "no-such-campaign", image_type="erofs"
            )

    def test_signed_apple_matrix_freezes_complete_current_geometry(self) -> None:
        shards = cts.load_apple_matrix_shards()
        self.assertEqual(len(shards), 100)
        for api in cts.BASELINE_GLES_APIS:
            api_shards = [
                shard for shard in shards.values() if shard.api == api
            ]
            fraction_count = 1 if api == "gles2" else 4
            self.assertEqual(len(api_shards), 20 * fraction_count)
            for config_index in range(20):
                config_shards = [
                    shard
                    for shard in api_shards
                    if shard.matrix.egl_config_index == config_index
                ]
                self.assertEqual(
                    [shard.selection.fraction_index for shard in config_shards],
                    list(range(fraction_count)),
                )
                self.assertTrue(
                    all(
                        shard.selection.fraction_count == fraction_count
                        and shard.selection.expected_count > 0
                        and cts.SHA256.fullmatch(shard.selected_sha256)
                        is not None
                        for shard in config_shards
                    )
                )
                self.assertEqual(
                    sum(
                        shard.selection.expected_count
                        for shard in config_shards
                    ),
                    config_shards[0].leaf.case_count,
                )

        with tempfile.TemporaryDirectory() as temporary:
            manifest = json.loads(
                (INTEGRATION_GPU / "vk_gl_cts_apple_matrix.json").read_text(
                    encoding="utf-8"
                )
            )
            manifest["apis"]["gles3"]["selected_sha256"].pop()
            path = Path(temporary) / "matrix.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(
                cts.CampaignError, "invalid Apple CTS matrix bounds"
            ):
                cts.load_apple_matrix_shards(path=path)

    def test_apple_gl_matrix_cell_matches_the_macos_workload(self) -> None:
        """The desktop GL cell is the shard the macOS lane accepts on.

        Both renderer backends run the identical caselist, so an Apple result
        is comparable to the macOS one case for case rather than only in
        aggregate.
        """
        identity = cts.load_identity()
        receipt, _ = cts.select_rootfs_contract(
            identity,
            "ext4",
            receipt_fixtures.current_epoch_receipt_file(
                self, receipt_fixtures.ZINK_GL_RECEIPT
            ),
        )
        self.assertEqual(
            len(cts.load_apple_matrix_shards(identity)), 100
        )
        shards = cts.load_apple_matrix_shards(identity, receipt=receipt)
        gl_shards = {
            identifier: shard
            for identifier, shard in shards.items()
            if shard.api == "gl46"
        }
        self.assertEqual(
            sorted(gl_shards),
            [
                "gl46-matrix-cfg00-xml01-001of032-virgl-v1",
                "gl46-matrix-cfg00-xml01-001of032-zink-v1",
            ],
        )
        self.assertEqual(
            {shard.renderer_backend for shard in gl_shards.values()},
            {"virgl", "zink"},
        )
        for shard in gl_shards.values():
            self.assertEqual(shard.leaf.identifier, "gl45-es31-main-es-for-gl")
            self.assertEqual(shard.selection.expected_count, 977)
            self.assertEqual(shard.matrix.configuration.name, "es31-main")
            self.assertEqual(shard.matrix.opengl_configuration_index, 1)
            self.assertEqual(shard.matrix.egl_config.identifier, 1)
            self.assertEqual(
                shard.selected_sha256,
                "aac3e92ea51e20a29ecdfc500e4dedc2b62b8fc785fe229def375f2b3a67960d",
            )

        with tempfile.TemporaryDirectory() as temporary:
            manifest = json.loads(
                (INTEGRATION_GPU / "vk_gl_cts_apple_matrix.json").read_text(
                    encoding="utf-8"
                )
            )
            manifest["opengl"]["renderer_backends"] = ["virgl", "virgl"]
            path = Path(temporary) / "matrix.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(
                cts.CampaignError, "invalid Apple CTS matrix OpenGL bounds"
            ):
                cts.load_apple_matrix_shards(identity, path, receipt)

    def test_cli_blocks_qualification_and_interpreter(self) -> None:
        stderr = io.StringIO()
        old_stderr = sys.stderr
        try:
            sys.stderr = stderr
            with self.assertRaises(SystemExit):
                cts.parse_arguments(
                    [
                        "--leaf",
                        "gles3",
                        "--api",
                        "gles3",
                        "--qualification",
                    ]
                )
            self.assertIn("No authoritative receipt", stderr.getvalue())
            with self.assertRaises(SystemExit):
                cts.parse_arguments(
                    [
                        "--leaf",
                        "gles3",
                        "--api",
                        "gles3",
                        "--engine",
                        "interpreter",
                    ]
                )
        finally:
            sys.stderr = old_stderr

    def test_harness_installs_its_launcher_and_service_as_guest_files(
        self,
    ) -> None:
        options = cts.parse_arguments(["--leaf", "gles3", "--api", "gles3"])
        selection = cts.Selection(options.leaves["gles3"], (), 0, 1)
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory) / "run"
            command = cts.build_harness_command(options, selection, run)
            self.assertFalse(run.exists(), "building the command wrote files")
            staged = {
                argument.split("=", 2)[1]: Path(argument.split("=", 2)[2])
                for argument in command
                if argument.startswith("--guest-file=")
            }
            self.assertEqual(
                set(staged),
                {
                    cts.GUEST_LAUNCHER_PATH,
                    cts.GUEST_SERVICE_PATH,
                    cts.CTS_TARGET_PATH,
                },
            )
            self.assertNotIn("base64 -d", " ".join(command))
            # The session target starts the service; nothing is typed.
            self.assertNotIn("--command", command)
            self.assertEqual(
                cts.prepare_harness_command(options, selection, run), command
            )
            launcher = staged[cts.GUEST_LAUNCHER_PATH]
            self.assertEqual(launcher.stat().st_mode & 0o777, 0o755)
            self.assertEqual(
                launcher.read_text(),
                cts.build_guest_launcher(
                    selection,
                    options.seed,
                    options.shard_timeout,
                    options.rootfs_contract,
                    artifact_transport="virtio-serial",
                    renderer_backend=options.renderer,
                    checkpoint_bytes=cts.QPA_CHECKPOINT_BYTES,
                    progress_timeout=options.console_stall_timeout,
                    progress_busy_timeout=options.console_stall_busy_timeout,
                ),
            )
            self.assertEqual(
                staged[cts.GUEST_SERVICE_PATH].read_text(), cts.CTS_SERVICE
            )
            # A resumed session runs a different launcher, so its command
            # names a different file: the recorded command identifies what ran.
            resumed = cts.build_harness_command(
                options, selection, run, resume_after=3
            )
            self.assertNotEqual(resumed, command)

    def test_native_harness_does_not_pass_vnc_only_options(self) -> None:
        options = cts.parse_arguments(
            [
                "--leaf",
                "gles3",
                "--api",
                "gles3",
                "--display",
                "native",
            ]
        )
        leaf = options.leaves["gles3"]
        command = cts.build_harness_command(
            options,
            cts.Selection(leaf, (), 0, 1),
            Path("/tmp/uml-cts-test"),
        )
        self.assertIn("--gpu-display=native", command)
        self.assertFalse(any(item.startswith("--gpu-vnc-") for item in command))
        self.assertEqual(
            command[:3],
            ["env", "UML_GPU_CAPABILITY_REPORT=1", "UML_GPU_FRAME_STATS=1"],
        )


class ArtifactTransportTests(unittest.TestCase):
    def transport(self, connection: socket.socket) -> cts.AcknowledgedArtifactSocket:
        transport = cts.AcknowledgedArtifactSocket(Path("/unused"))
        transport.connection = connection
        transport.connected = True
        self.addCleanup(transport.close)
        return transport

    def test_fragmented_and_coalesced_frames_acknowledge_in_one_call(self) -> None:
        transcript = framed_artifact("results.qpa", b"payload")
        frames = transcript.splitlines()
        expected = "".join(cts.artifact_acknowledgement(f) + "\n" for f in frames)
        host, guest = socket.socketpair()
        host.setblocking(False)
        guest.setblocking(False)
        self.addCleanup(guest.close)
        transport = self.transport(host)
        guest.sendall(transcript[:7].encode("ascii"))
        transport.service()
        with self.assertRaises(BlockingIOError):
            guest.recv(4096)
        guest.sendall(transcript[7:].encode("ascii"))
        transport.service()
        self.assertEqual(guest.recv(4096), expected.encode("ascii"))
        guest.close()
        transport.service()
        self.assertTrue(transport.closed)
        self.assertEqual(transport.finish(), transcript)
        self.assertEqual(cts.decode_artifacts(transcript), {"results.qpa": b"payload"})

    def test_backpressure_retains_exact_suffix_and_services_new_input(self) -> None:
        frames = framed_artifact("results.qpa", b"payload").splitlines()
        connection = mock.Mock(spec=socket.socket)
        transport = self.transport(connection)
        connection.recv.side_effect = [
            (frame + "\n").encode("ascii") for frame in frames[:2]
        ]
        ack0, ack1 = [
            (cts.artifact_acknowledgement(frame) + "\n").encode("ascii")
            for frame in frames[:2]
        ]
        attempts = []

        def send(payload: bytes) -> int:
            attempts.append(bytes(payload))
            if len(attempts) == 1:
                raise BlockingIOError()
            return 5 if len(attempts) == 2 else len(payload)

        connection.send.side_effect = send
        with mock.patch.object(
            cts.select, "select", side_effect=[
                ([connection], [], []),
                ([connection], [], []),
                ([], [connection], []),
            ],
        ) as readiness:
            transport.service(timeout=0.05)
            self.assertEqual(transport.output, ack0)
            transport.service(timeout=0.05)
            self.assertEqual(transport.output, (ack0 + ack1)[5:])
            transport.service(timeout=0.05)
        self.assertEqual(attempts, [ack0, ack0 + ack1, (ack0 + ack1)[5:]])
        self.assertEqual(transport.output, b"")
        self.assertEqual(connection.recv.call_args_list, [mock.call(64 * 1024)] * 2)
        self.assertEqual(
            readiness.call_args_list,
            [
                mock.call([connection], [], [], 0.05),
                mock.call([connection], [connection], [], 0.05),
                mock.call([connection], [connection], [], 0.05),
            ],
        )

    def test_idle_connected_socket_waits_once_without_polling(self) -> None:
        connection = mock.Mock(spec=socket.socket)
        transport = self.transport(connection)
        with (
            mock.patch.object(
                cts.select, "select", return_value=([], [], []),
            ) as readiness,
            mock.patch.object(cts.time, "sleep") as sleep,
        ):
            transport.service(timeout=0.05)
        readiness.assert_called_once_with([connection], [], [], 0.05)
        sleep.assert_not_called()
        connection.recv.assert_not_called()
        connection.send.assert_not_called()

    def test_absent_and_closed_sockets_wait_without_spinning(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            for connected in (False, True):
                with self.subTest(connected=connected):
                    transport = cts.AcknowledgedArtifactSocket(
                        Path(temporary) / "absent.sock"
                    )
                    transport.connected = connected
                    transport.closed = connected
                    with (
                        mock.patch.object(cts.time, "sleep") as sleep,
                        mock.patch.object(cts.select, "select") as readiness,
                    ):
                        transport.service(timeout=0.05)
                        transport.service()
                    sleep.assert_called_once_with(0.05)
                    readiness.assert_not_called()

    def test_continuously_readable_socket_gets_only_one_receive_and_send(self) -> None:
        frame = framed_artifact("results.qpa", b"payload").splitlines()[0]
        connection = mock.Mock(spec=socket.socket)
        connection.recv.return_value = (frame + "\n").encode("ascii")
        connection.send.side_effect = lambda payload: len(payload)
        transport = self.transport(connection)
        with mock.patch.object(
            cts.select, "select", return_value=([connection], [], []),
        ) as readiness:
            transport.service(timeout=0.05)
        readiness.assert_called_once()
        connection.recv.assert_called_once_with(64 * 1024)
        connection.send.assert_called_once()
        self.assertEqual(transport.lines, [frame + "\n"])

    def test_invalid_frames_are_never_acknowledged(self) -> None:
        for payload in (b"\n", b"status\n", b"\xff\n", b"status\r\n"):
            with self.subTest(payload=payload):
                connection = mock.Mock(spec=socket.socket)
                connection.recv.return_value = payload
                transport = self.transport(connection)
                with mock.patch.object(
                    cts.select, "select", return_value=([connection], [], []),
                ):
                    with self.assertRaises(cts.CampaignError):
                        transport.service()
                connection.send.assert_not_called()
                self.assertEqual(transport.lines, [])
                self.assertEqual(transport.output, b"")

    def test_overlong_partial_frame_is_rejected_without_acknowledgement(self) -> None:
        connection = mock.Mock(spec=socket.socket)
        connection.recv.return_value = b"x"
        transport = self.transport(connection)
        transport.input.extend(b"x" * cts.MAX_ARTIFACT_TRANSPORT_LINE)
        with mock.patch.object(
            cts.select, "select", return_value=([connection], [], []),
        ):
            with self.assertRaisesRegex(cts.CampaignError, "exceeds its bound"):
                transport.service()
        connection.send.assert_not_called()

    def test_eof_rejects_both_partial_input_and_pending_acknowledgement(self) -> None:
        for pending in ("input", "output"):
            with self.subTest(pending=pending):
                connection = mock.Mock(spec=socket.socket)
                connection.recv.return_value = b""
                transport = self.transport(connection)
                getattr(transport, pending).extend(b"pending")
                with mock.patch.object(
                    cts.select, "select",
                    return_value=([connection], [connection], []),
                ):
                    with self.assertRaisesRegex(cts.CampaignError, "incomplete frame"):
                        transport.service()
                connection.close.assert_called_once()
                connection.send.assert_not_called()
                self.assertTrue(transport.closed)


class ArtifactGuestWaitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.context = contextlib.ExitStack()
        self.addCleanup(self.context.close)
        temporary = self.context.enter_context(tempfile.TemporaryDirectory())
        self.log = Path(temporary) / "console.log"
        self.log.write_text("guest progress\n", encoding="utf-8")
        self.clock = 0.0
        self.process = mock.Mock(spec=subprocess.Popen)
        self.process.poll.return_value = None
        self.transport = mock.Mock()
        self.transport.connection = object()
        self.transport.lines = []
        self.transport.finish.side_effect = lambda: "".join(self.transport.lines)
        self.context.enter_context(mock.patch.object(
            cts, "AcknowledgedArtifactSocket", return_value=self.transport,
        ))
        self.context.enter_context(mock.patch.object(
            cts.time, "monotonic", side_effect=lambda: self.clock,
        ))
        self.context.enter_context(mock.patch.object(
            cts.time, "sleep",
            side_effect=AssertionError("unconditional polling sleep"),
        ))
        self.context.enter_context(contextlib.redirect_stdout(io.StringIO()))
        self.waits = []

    def advance(self, timeout: float, *, ready_after: float | None = None) -> None:
        self.assertGreater(timeout, 0)
        self.assertLessEqual(timeout, cts.ARTIFACT_SERVICE_INTERVAL)
        self.waits.append(timeout)
        self.clock += timeout if ready_after is None else min(timeout, ready_after)

    def wait(self, timeout: int = 1, *, stall_timeout: int = 0) -> tuple[str, str]:
        return cts.wait_for_guest(
            self.process, self.log, timeout, Path("/unused"),
            console_stall_timeout=stall_timeout,
        )

    def test_exit_drains_final_frame_without_fixed_pacing(self) -> None:
        self.process.poll.side_effect = [None, None, 0]

        def service(timeout: float) -> None:
            self.advance(timeout)
            if len(self.waits) == 4:
                self.transport.lines.append("final frame\n")
                self.transport.connection = None

        self.transport.service.side_effect = service
        self.assertEqual(self.wait(), ("guest progress\n", "final frame\n"))
        self.assertEqual(len(self.waits), 4)
        self.process.wait.assert_called_once_with(timeout=5)
        self.transport.close.assert_called_once()

    def test_continuous_input_cannot_starve_hard_deadline_or_process_poll(self) -> None:
        def service(timeout: float) -> None:
            self.advance(timeout, ready_after=0.01)
            self.transport.lines.append("progress\n")

        self.transport.service.side_effect = service
        with self.assertRaisesRegex(TimeoutError, "exceeded 1 seconds"):
            self.wait()
        self.assertEqual(self.process.poll.call_count, len(self.waits))
        self.assertGreater(len(self.waits), 20)
        self.assertEqual(self.clock, 1)
        self.transport.close.assert_called_once()

    def test_idle_socket_still_checks_console_stall(self) -> None:
        self.transport.service.side_effect = self.advance
        with self.assertRaisesRegex(TimeoutError, "no progress for 30 seconds"):
            self.wait(timeout=100, stall_timeout=30)
        self.assertGreaterEqual(self.clock, 60)
        self.assertLess(self.clock, 61)
        self.assertEqual(self.process.poll.call_count, len(self.waits))
        self.transport.close.assert_called_once()

    def test_post_exit_pending_bytes_still_fail_after_bounded_drain(self) -> None:
        self.process.poll.return_value = 0
        self.transport.service.side_effect = self.advance
        self.transport.finish.side_effect = cts.CampaignError("pending bytes")
        with self.assertRaisesRegex(cts.CampaignError, "pending bytes"):
            self.wait()
        self.assertAlmostEqual(self.clock, 2 + cts.ARTIFACT_SERVICE_INTERVAL)
        self.assertLessEqual(len(self.waits), 42)
        self.transport.close.assert_called_once()

    def test_interruption_closes_transport(self) -> None:
        self.transport.service.side_effect = KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            self.wait()
        self.transport.close.assert_called_once()

    # The seven lines a wedged guest repeated 236 times over 4.4 hours, while
    # the CTS process spun in the guest kernel and printed nothing itself.
    STALL_REPORT = (
        "[ 1095.744601] rcu: INFO: rcu_sched detected stalls on CPUs/tasks:\n"
        "[ 1095.585777] rcu: \t1-...0: (1 GPs behind) idle=1254/1/0x4000000000000000"
        " softirq=16642/16643 fqs=650237\n"
        "[ 1095.586004] rcu: \t(detected by 0, t=1490087 jiffies, g=47617,"
        " q=4867 ncpus=4)\n"
        "[ 1095.586004] Task dump for CPU 1:\n"
        "[ 1095.586006] task:glcts           state:R  running task     stack:0"
        "     pid:588   tgid:588   ppid:586    task_flags:0x400100 flags:0x00000004\n"
        "[ 1095.586010] Stack:\n"
        "[ 1095.586011] Call Trace:\n"
    )

    def appending(self, period: float, text: str):
        """Append ``text`` to the console every ``period`` seconds of clock."""
        due = [period]

        def service(timeout: float) -> None:
            self.advance(timeout)
            if self.clock >= due[0]:
                due[0] += period
                with self.log.open("a", encoding="utf-8") as console:
                    console.write(text)

        return service

    def test_kernel_stall_reports_do_not_hold_off_the_console_bound(self) -> None:
        # The kernel reprints on its own timer far more often than the bound,
        # so counting console growth alone leaves the guest running forever.
        self.transport.service.side_effect = self.appending(10, self.STALL_REPORT)
        with self.assertRaisesRegex(TimeoutError, "no progress for 60 seconds"):
            self.wait(timeout=300, stall_timeout=60)
        self.assertLess(self.clock, 300)
        self.transport.close.assert_called_once()

    def test_guest_output_among_stall_reports_is_still_progress(self) -> None:
        # A guest that is still running its cases must never be cut, however
        # much the kernel prints beside it.
        self.transport.service.side_effect = self.appending(
            10, self.STALL_REPORT + "[ 1096.0] uml-vk-gl-cts[588]:   Pass (Pass)\n",
        )
        with self.assertRaisesRegex(TimeoutError, "exceeded 300 seconds"):
            self.wait(timeout=300, stall_timeout=60)
        self.transport.close.assert_called_once()

    def test_console_progress_classification(self) -> None:
        stall = self.STALL_REPORT.encode()
        self.log.write_bytes(stall)
        self.assertEqual(
            cts.console_progressed(self.log, 0, len(stall)), (len(stall), False)
        )

        # A trailing partial line is held back rather than judged by half.
        partial = stall + b"[ 1100.0] uml-vk-gl-cts[588]: Test case 'x'.."
        self.log.write_bytes(partial)
        self.assertEqual(
            cts.console_progressed(self.log, 0, len(partial)), (len(stall), False)
        )

        # Completing that line is progress, and only the whole line counts.
        whole = partial + b"\n"
        self.log.write_bytes(whole)
        self.assertEqual(
            cts.console_progressed(self.log, len(stall), len(whole)),
            (len(whole), True),
        )

        # Blank lines carry nothing either way.
        blanks = b"\n\n\n"
        self.log.write_bytes(blanks)
        self.assertEqual(
            cts.console_progressed(self.log, 0, len(blanks)), (len(blanks), False)
        )

    def test_unclassifiable_console_is_treated_as_progress(self) -> None:
        # The bound must never cut a guest on a reading failure or on a flood
        # too large to classify: that volume is itself evidence of activity.
        self.assertEqual(
            cts.console_progressed(
                self.log, 0, cts.MAX_CONSOLE_PROGRESS_SCAN + 1
            ),
            (cts.MAX_CONSOLE_PROGRESS_SCAN + 1, True),
        )
        missing = self.log.with_name("absent.log")
        self.assertEqual(cts.console_progressed(missing, 0, 10), (10, True))



class WaiverTests(unittest.TestCase):
    """The granted CTS waivers are exact, scoped, guest-bound and fail-closed."""

    @staticmethod
    def leaf() -> cts.LeafIdentity:
        return cts.LeafIdentity(
            "gl45-es3-main-es-for-gl", "gl45-es3-main-es-for-gl.txt", 16, "4" * 64
        )

    def matrix(
        self,
        name: str = "es3-main",
        fbo_config: str | None = "rgba8888d24s8ms0",
        leaf: cts.LeafIdentity | None = None,
        opengl_configuration_index: int | None = 0,
        api: str = "gl46",
        xml_arguments: tuple[str, ...] | None = None,
    ) -> cts.MatrixRun:
        leaf = leaf or self.leaf()
        if xml_arguments is None:
            xml_arguments = ("--deqp-screen-rotation=unspecified",) + (
                () if fbo_config is None else
                (f"--deqp-gl-config-name={fbo_config}", "--deqp-surface-type=fbo")
            )
        configuration = cts.MustpassConfiguration(
            api=api,
            leaf=leaf,
            name=name,
            use_for_first_egl_config=True,
            screen_rotation="unspecified",
            surface_width=256,
            surface_height=256,
            base_seed=1,
            watchdog="disable",
            fbo_config=fbo_config,
            xml_arguments=xml_arguments,
        )
        return cts.MatrixRun(
            configuration=configuration,
            egl_config=cts.EglConfig(1, ("window", "pixmap", "pbuffer")),
            egl_config_index=0,
            config_inventory_sha256="1" * 64,
            selection=cts.Selection(leaf, (), 0, 8),
            mustpass_xml_path="/opt/vk-gl-cts/openglcts/mustpass.xml",
            mustpass_xml_size=1478,
            mustpass_xml_sha256="2" * 64,
            opengl_configuration_index=opengl_configuration_index,
        )

    def test_checked_waiver_list_names_only_the_es3_fbo_state_queries(self) -> None:
        waivers = cts.load_waivers()
        self.assertEqual(
            [waiver.identifier for waiver in waivers],
            ["es3-fbo-default-framebuffer-state-queries"],
        )
        waiver = waivers[0]
        self.assertEqual(
            (waiver.api, waiver.configuration_name, waiver.draw_surface_type),
            ("gl46", "es3-main", "fbo"),
        )
        self.assertEqual(waiver.vendor, "Mesa")
        self.assertEqual(waiver.granted, "2026-09-08")
        self.assertEqual(len(waiver.cases), 15)
        self.assertEqual(list(waiver.cases), sorted(set(waiver.cases)))
        for case in waiver.cases:
            self.assertRegex(
                case,
                r"^dEQP-GL45-ES3\.functional\.state_query\."
                r"(fbo\.(draw|read)_framebuffer_default_framebuffer"
                r"|fbo\.framebuffer_attachment_x_size_initial"
                r"|integers\.(draw_buffer|read_buffer|framebuffer_binding)"
                r"_get(boolean|float|integer|integer64))$",
            )
        self.assertEqual(cts.waived_cases(waivers), frozenset(waiver.cases))

        self.assertEqual(cts.applicable_waivers(self.matrix(), waivers), waivers)
        for other in (
            self.matrix(name="es31-main"),
            self.matrix(fbo_config=None),
            self.matrix(
                api="gles3", name="khr-main", opengl_configuration_index=None,
                leaf=cts.LeafIdentity("gles3", "gles3.txt", 16, "5" * 64),
            ),
        ):
            self.assertEqual(cts.applicable_waivers(other, waivers), ())

        # The pinned ES-for-GL record names a GL config without asking for the
        # framework framebuffer object, so its run observes default-framebuffer
        # state and the FBO-scoped waiver does not reach it.
        es_for_gl = self.matrix(xml_arguments=(
            "--deqp-gl-config-name=rgba8888d24s8ms0",
            "--deqp-screen-rotation=unspecified",
            "--deqp-surface-width=256",
            "--deqp-surface-height=256",
            "--deqp-watchdog=disable",
        ))
        self.assertEqual(cts.matrix_draw_surface_type(es_for_gl), "window")
        self.assertEqual(cts.applicable_waivers(es_for_gl, waivers), ())
        arguments = cts.matrix_runner_arguments(es_for_gl, "/run/uml-cts/selected.txt")
        self.assertIn("--deqp-surface-type=window", arguments)
        self.assertNotIn("--deqp-surface-type=fbo", arguments)
        self.assertFalse(
            any(argument.startswith("--deqp-gl-config-name") for argument in arguments)
        )

        payload = json.loads(cts.WAIVERS_PATH.read_text(encoding="utf-8"))
        entry = payload["waivers"][0]

        def duplicate_waiver(document: dict) -> None:
            clone = copy.deepcopy(entry)
            clone["id"] = "other"
            document["waivers"].append(clone)

        mutations = (
            (lambda document: document.update(schema_version=2), "invalid CTS waiver list"),
            (lambda document: document.update(waivers=[]), "invalid CTS waiver list"),
            (lambda document: document["waivers"][0].update(note="x"), "invalid CTS waiver entry"),
            (lambda document: document["waivers"][0]["scope"].pop("api"), "invalid CTS waiver scope"),
            (lambda document: document["waivers"][0].update(description=" "), "non-empty strings"),
            (lambda document: document["waivers"][0]["scope"].update(draw_surface_type="screen"), "unknown draw surface"),
            (lambda document: document["waivers"][0].update(renderers=[]), "renderer patterns"),
            (lambda document: document["waivers"][0]["cases"].reverse(), "sorted unique case names"),
            (lambda document: document["waivers"][0]["cases"].append(entry["cases"][-1]), "sorted unique case names"),
            (lambda document: document["waivers"][0]["cases"].append("bad case name"), "sorted unique case names"),
            (duplicate_waiver, "repeats an id or a case"),
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "waivers.json"
            for mutate, message in mutations:
                document = copy.deepcopy(payload)
                mutate(document)
                path.write_text(json.dumps(document), encoding="utf-8")
                with self.subTest(message=message), self.assertRaisesRegex(
                    cts.CampaignError, message
                ):
                    cts.load_waivers(path)
            path.write_text("{", encoding="utf-8")
            with self.assertRaisesRegex(cts.CampaignError, "cannot read CTS waiver list"):
                cts.load_waivers(path)

    def test_waiver_file_and_runner_arguments_bind_the_guest(self) -> None:
        waivers = cts.load_waivers()
        xml = cts.waiver_xml(waivers)
        self.assertTrue(
            xml.startswith('<?xml version="1.0" encoding="utf-8"?>\n<waiver_list>\n')
        )
        self.assertIn(
            '<waiver vendor="Mesa" url="docs/testing.md#opengl-es3-fbo-state-query-waiver">',
            xml,
        )
        self.assertIn(
            "<r>zink Vulkan *(Virtio-GPU Venus (*) (MESA_KOSMICKRISP))</r>", xml
        )
        self.assertEqual(xml.count("<t>"), 15)
        for case in waivers[0].cases:
            self.assertIn(f"<t>{case}</t>", xml)
        parsed = ElementTree.fromstring(xml)
        self.assertEqual(
            [element.text for element in parsed.iter("t")], list(waivers[0].cases)
        )

        odd = cts.Waiver(
            "odd", "2026-09-08", "owner", "gl46", "es3-main", "fbo",
            'V"<&', ("r<",), "u&", "d>", ("a.b",),
        )
        rendered = cts.waiver_xml((odd,))
        self.assertIn("<r>r&lt;</r>", rendered)
        self.assertIn("<description>d&gt;</description>", rendered)
        self.assertIn("url=\"u&amp;\"", rendered)
        self.assertEqual(ElementTree.fromstring(rendered).find("waiver").get("vendor"), 'V"<&')

        matrix = self.matrix()
        plain = cts.matrix_runner_arguments(matrix, "/run/uml-cts/selected.txt")
        self.assertFalse(any(argument.startswith("--deqp-waiver-file") for argument in plain))
        self.assertEqual(
            cts.matrix_runner_arguments(
                matrix, "/run/uml-cts/selected.txt", cts.GUEST_WAIVER_PATH
            ),
            plain + ["--deqp-waiver-file=/usr/local/share/uml-vk-gl-cts/waivers.xml"],
        )

        digest = hashlib.sha256(xml.encode("utf-8")).hexdigest()
        launcher = cts.build_guest_launcher(
            matrix.selection, 1, 300, erofs_contract(),
            matrix=matrix, renderer_backend="zink", waiver_sha256=digest,
        )
        self.assertIn(
            "--deqp-waiver-file=/usr/local/share/uml-vk-gl-cts/waivers.xml", launcher
        )
        self.assertIn(digest, launcher)
        self.assertIn("waiver list identity changed", launcher)
        unwaived = cts.build_guest_launcher(
            matrix.selection, 1, 300, erofs_contract(),
            matrix=matrix, renderer_backend="zink",
        )
        self.assertNotIn("--deqp-waiver-file", unwaived)
        self.assertNotIn("waiver list identity changed", unwaived)
        for arguments in (
            {"matrix": None, "waiver_sha256": digest},
            {"matrix": matrix, "waiver_sha256": "abc"},
        ):
            with self.subTest(arguments=arguments), self.assertRaisesRegex(
                cts.CampaignError, "waiver list needs"
            ):
                cts.build_guest_launcher(
                    matrix.selection, 1, 300, erofs_contract(),
                    renderer_backend="zink", **arguments,
                )

    def test_busy_stall_bound_keeps_a_computing_case_and_cuts_a_wedged_one(self) -> None:
        matrix = self.matrix()
        busy = cts.build_guest_launcher(
            matrix.selection, 1, 300, erofs_contract(),
            matrix=matrix, renderer_backend="zink", progress_timeout=600,
            progress_busy_timeout=3600,
        )
        self.assertIn("progress_timeout=600\n", busy)
        self.assertIn("progress_busy_timeout=3600\n", busy)
        self.assertIn("progress_busy_ticks=$((progress_interval * clock_ticks / 10))", busy)
        self.assertIn("cts_cpu_ticks()", busy)
        self.assertIn("pgrep -x glcts", busy)
        self.assertIn('"/proc/$cts_process/stat"', busy)
        self.assertIn("idle_seconds=0", busy)
        self.assertIn(
            "UML_CTS_PROGRESS|qpa_bytes=%s|stalled_seconds=%s|idle_seconds=%s|cpu_ticks=%s|case=%s",
            busy,
        )
        self.assertIn('[ "$idle_seconds" -ge "$progress_timeout" ]', busy)
        self.assertIn('[ "$stalled_seconds" -ge "$progress_busy_timeout" ]', busy)
        plain = cts.build_guest_launcher(
            matrix.selection, 1, 300, erofs_contract(),
            matrix=matrix, renderer_backend="zink", progress_timeout=600,
        )
        self.assertIn("progress_busy_timeout=0\n", plain)
        self.assertIn('[ "$progress_busy_timeout" -le 0 ]', plain)
        for bound in (-1, 599):
            with self.subTest(bound=bound), self.assertRaisesRegex(
                cts.CampaignError, "busy stall bound"
            ):
                cts.build_guest_launcher(
                    matrix.selection, 1, 300, erofs_contract(),
                    matrix=matrix, renderer_backend="zink", progress_timeout=600,
                    progress_busy_timeout=bound,
                )

    def test_waived_statuses_are_accepted_only_for_granted_cases(self) -> None:
        statuses = ("Pass", "NotSupported", "QualityWarning", "CompatibilityWarning")
        waivers = cts.load_waivers()
        granted = waivers[0].cases[0]
        other = waivers[0].cases[1]
        cts.validate_official_runner_results(
            {granted: "Waiver", other: "Pass", "KHR-GL46.info.vendor": "NotSupported"},
            statuses, cts.waived_cases(waivers),
        )
        for results in (
            {granted: "Fail"},
            {granted: "Crash"},
            {"KHR-GL46.info.vendor": "Waiver"},
        ):
            with self.subTest(results=results), self.assertRaisesRegex(
                cts.CampaignError, "official CTS runner rejects"
            ):
                cts.validate_official_runner_results(
                    results, statuses, cts.waived_cases(waivers)
                )
        with self.assertRaisesRegex(cts.CampaignError, "official CTS runner rejects"):
            cts.validate_official_runner_results({granted: "Waiver"}, statuses)

        header = (
            '#sessionInfo vendor "Mesa"\n'
            '#sessionInfo renderer "zink Vulkan 1.4(Virtio-GPU Venus (Apple M1 Ultra) (MESA_KOSMICKRISP))"\n'
            '#sessionInfo waiverUrls "docs/testing.md#opengl-es3-fbo-state-query-waiver"\n'
            "#beginSession\n"
        )
        cts.validate_waiver_session(header, waivers)
        cts.validate_waiver_session('#sessionInfo vendor "Mesa"\n#beginSession\n', ())
        for text, applied in (
            (header, ()),
            ('#sessionInfo vendor "Mesa"\n#beginSession\n', waivers),
            (header.replace("docs/testing.md", "elsewhere.md"), waivers),
            (header.replace('waiver"', 'waiver other"'), waivers),
        ):
            with self.subTest(text=text[-80:]), self.assertRaisesRegex(
                cts.CampaignError, "applied waivers"
            ):
                cts.validate_waiver_session(text, applied)
        with self.assertRaisesRegex(cts.CampaignError, "malformed waiver line"):
            cts.validate_waiver_session("#sessionInfo waiverUrls x\n", waivers)

if __name__ == "__main__":
    unittest.main()
