#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a bounded VK-GL-CTS shard in an authenticated rendered guest.

The authenticated rootfs owns the CTS executable, resources, Mesa, libdrm,
and WSI.  This runner injects only a first-party launcher and transient output
into the disposable OverlayFS upper layer. The default path renders Virgl on
the host's Zink/KosmicKrisp OpenGL provider. The explicit Zink path instead
requires the receipt-bound Vulkan/OpenGL derivative and proves Zink over
Virtio-GPU Venus.

Official must-pass qualification remains fail-closed while the original image
has no authoritative CTS build receipt and the complete matrices have not run.
Developer shards are still useful for bring-up and failure localization.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import binascii
from dataclasses import dataclass, field, replace
import errno
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import select
import shlex
import socket
import stat
import subprocess
import sys
import time
from typing import Any, Callable, Collection, Sequence
from xml.etree import ElementTree
from xml.sax.saxutils import escape as xml_escape, quoteattr as xml_quoteattr

sys.path.insert(0, str(Path(__file__).resolve().parent))
import virgl_capset  # noqa: E402
import vulkan_cts_receipt  # noqa: E402
import guest_process  # noqa: E402
import opengl_cts_plan  # noqa: E402
import gles_cts_plan  # noqa: E402
import run_gpu_guest as gpu_guest  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[3]
IDENTITY_PATH = Path(__file__).with_name("vk_gl_cts_identity.json")
APPLE_CAMPAIGNS_PATH = Path(__file__).with_name(
    "vk_gl_cts_apple_campaigns.json"
)
APPLE_MATRIX_PATH = Path(__file__).with_name(
    "vk_gl_cts_apple_matrix.json"
)
GLES_MUSTPASS_PATH = Path(__file__).with_name(
    "vk_gl_cts_gles_mustpass.xml"
)
GLES32_MATRIX_PATH = Path(__file__).with_name(
    "vk_gl_cts_gles32_matrix.json"
)
RUNNER_ORACLE_PATH = Path(__file__).with_name(
    "vk_gl_cts_runner_oracle.json"
)
WAIVERS_PATH = Path(__file__).with_name("vk_gl_cts_waivers.json")
GUEST_WAIVER_PATH = "/usr/local/share/uml-vk-gl-cts/waivers.xml"
GUEST_LAUNCHER_PATH = "/usr/local/libexec/uml-vk-gl-cts"
GUEST_SERVICE_NAME = "uml-vk-gl-cts.service"
GUEST_SERVICE_PATH = f"/etc/systemd/system/{GUEST_SERVICE_NAME}"
WAIVED_STATUS = "Waiver"
WAIVER_KEYS = frozenset(
    {"id", "granted", "granted_by", "scope", "vendor", "renderers", "url",
     "description", "cases"}
)
WAIVER_SCOPE_KEYS = frozenset({"api", "configuration_name", "draw_surface_type"})
WAIVER_SURFACE_TYPES = frozenset({"fbo", "window", "pbuffer", "pixmap"})
DEFAULT_EMULATOR = REPO_ROOT / ".build/presets/macos-integration/bin/uml-darwin"
DEFAULT_ROOTFS = REPO_ROOT / "rootfs.erofs"
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/vk-gl-cts"

MAX_COMPRESSED_ARTIFACT = 64 * 1024 * 1024
MAX_UNCOMPRESSED_ARTIFACT = 512 * 1024 * 1024
SHARD_ARTIFACTS = frozenset(
    {"renderer.txt", "results.qpa", "selected.txt", "summary.txt"}
)
ENUMERATED_SHARD_ARTIFACTS = SHARD_ARTIFACTS | {"expected.txt"}
# A campaign that cannot hold every enumerated case on its device transports
# exactly which ones it left out, so the same cases stay selectable elsewhere.
EXCLUDED_SHARD_ARTIFACTS = ENUMERATED_SHARD_ARTIFACTS | {"skipped.txt"}
INVENTORY_ARTIFACTS = frozenset(
    {"inventory.txt", "renderer.txt", "results.qpa", "summary.txt"}
)
CONFIG_INVENTORY_ARTIFACTS = frozenset(
    {"renderer.txt", "results.qpa", "summary.txt"}
)
ALLOWED_ARTIFACTS = (
    SHARD_ARTIFACTS
    | ENUMERATED_SHARD_ARTIFACTS
    | EXCLUDED_SHARD_ARTIFACTS
    | INVENTORY_ARTIFACTS
    | CONFIG_INVENTORY_ARTIFACTS
)
GLES_INVENTORY_SCOPE = "gles2-gles3"
GLES_INVENTORY_MODULES = ("KHR-GLES2", "KHR-GLES3")
BASELINE_GLES_APIS = ("gles2", "gles3")
CONFIG_CASES = {
    "gles2": "CTS-Configs.es2",
    "gles3": "CTS-Configs.es3",
    "gles32": "CTS-Configs.es32",
    "gl46": "CTS-Configs.gl46",
}
# The ES 3.2 matrix also runs the AOSP collections, whose GL configuration
# names the official runner resolves against ConfigList::aospConfigs. That
# list is built from EGL attributes CTS-Configs does not log, so the ES 3.2
# inventory runs dEQP-EGL.info.configs in the same session and records the
# attribute table beside the conformant list.
EGL_ATTRIBUTE_CASE = "dEQP-EGL.info.configs"
CONFIG_INVENTORY_SCOPES = {
    "gles2": ("CTS-Configs.es2",),
    "gles3": ("CTS-Configs.es3",),
    "gles32": ("CTS-Configs.es32", EGL_ATTRIBUTE_CASE),
    "gl46": ("CTS-Configs.gl46",),
}
CONFIG_INVENTORY_SCOPE_NAMES = {
    "gles2": "configs",
    "gles3": "configs",
    "gles32": "configs-egl-attributes",
    "gl46": "configs",
}
# The official matrices whose run records come from must-pass XML documents
# rather than the two-row GLES baseline split, and which therefore select one
# XML configuration per shard and bind their inventory to the emulator build.
XML_MATRIX_APIS = ("gl46", "gles32")
CASE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]*\Z")
APPLE_CAMPAIGN_ID = re.compile(r"[a-z0-9][a-z0-9-]*\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
UUID = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\Z"
)
SOFTWARE_RENDERERS = re.compile(
    r"(?:llvmpipe|softpipe|swrast|software rasterizer|xvfb)",
    re.IGNORECASE,
)
BEGIN_ARTIFACT = re.compile(
    r"UML_CTS_ARTIFACT_BEGIN\|([^|]+)\|gzip\+base64\|([0-9]+)\|([0-9a-f]{64})\Z"
)
CHUNK_ARTIFACT = re.compile(
    r"UML_CTS_ARTIFACT_CHUNK\|([^|]+)\|([0-9]+)\|([A-Za-z0-9+/=]+)\Z"
)
END_ARTIFACT = re.compile(
    r"UML_CTS_ARTIFACT_END\|([^|]+)\|([0-9]+)\Z"
)
ACK_ARTIFACT = re.compile(
    r"UML_CTS_ARTIFACT_ACK\|([^|]+)\|(begin|end|[0-9]+)\Z"
)
HOST_CONTEXT = re.compile(
    r"UML_GPU_HOST_CONTEXT\|requested=([0-9]+)\.([0-9]+)\|"
    r"created=([01])\|egl_error=([0-9]+)\|actual=([0-9]+)\.([0-9]+)\|"
    r"vendor_hex=([0-9a-f]*)\|renderer_hex=([0-9a-f]*)\|"
    r"version_hex=([0-9a-f]*)\|glsl_hex=([0-9a-f]*)\|"
    r"extensions=([0-9]+)\Z"
)
HOST_EXTENSION = re.compile(
    r"UML_GPU_HOST_EXTENSION\|requested=([0-9]+)\.([0-9]+)\|"
    r"name_hex=([0-9a-f]+)\Z"
)
CAPSET_BEGIN = re.compile(
    r"UML_GPU_CAPSET_BEGIN\|id=([0-9]+)\|version=([0-9]+)\|"
    r"size=([0-9]+)\|chunks=([0-9]+)\Z"
)
CAPSET_CHUNK = re.compile(
    r"UML_GPU_CAPSET_CHUNK\|id=([0-9]+)\|index=([0-9]+)\|"
    r"hex=([0-9a-f]+)\Z"
)
CAPSET_END = re.compile(
    r"UML_GPU_CAPSET_END\|id=([0-9]+)\|chunks=([0-9]+)\Z"
)
# The host reports this capset alongside the two Virgl ones. It carries the Venus protocol identity, not a Virgl capability record.
VENUS_CAPSET_ID = 4
NATIVE_DISPLAY_FINAL = re.compile(
    r"Native display final: guest ([0-9]+) presented ([0-9]+) "
    r"dropped ([0-9]+) cpu-fallback ([0-9]+)"
)
RESULT_STATUS = re.compile(r"<Result\s+StatusCode=\"([^\"]+)\"")
RESULT_CASE = re.compile(r"<TestCaseResult\b[^>]*\bCasePath=\"([^\"]+)\"")
EGL_CONFIG_MESSAGE = re.compile(
    r"EGL\(([1-9][0-9]*)\): "
    r"((?:window|pixmap|pbuffer)"
    r"(?:, (?:window|pixmap|pbuffer))*)\Z"
)
GUEST_ERROR = re.compile(r"UML_CTS_ERROR: ([^\r\n]+)")
HOST_BACKEND_MARKER = "Virgl Zink/KosmicKrisp provider:"
RETIRED_BACKEND_MARKERS = ("Virgl CGL", "Virgl ANGLE/Metal provider:")
DARWIN_CLEAN_STOP = "Darwin UML stopped: result=0 exitcode=0"
DIRTY_DRM_SHUTDOWN = "Memory manager not clean during takedown."
MAX_ARTIFACT_TRANSPORT_LINE = 64 * 1024
ARTIFACT_SERVICE_INTERVAL = 0.05
VIRTIO_SERIAL_PORT_NAME = "org.uml.host-serial.0"
VIRTIO_SERIAL_GUEST_DEVICE = (
    f"/dev/virtio-ports/{VIRTIO_SERIAL_PORT_NAME}"
)
ARTIFACT_TRANSPORT_LOG = "artifact-serial.log"
QPA_ARTIFACT = "results.qpa"
SUMMARY_ARTIFACT = "summary.txt"
# Every session emits its summary last, so this frame marks the moment the
# guest has nothing left to deliver.  An unkillable wedged case can then stall
# the poweroff while the kernel keeps printing, which would defeat any bound
# derived from console silence; the completed transfer gets its own deadline.
FINAL_ARTIFACT_FRAME = f"UML_CTS_ARTIFACT_END|{SUMMARY_ARTIFACT}|"
SHUTDOWN_GRACE_SECONDS = 180
# The same kernel printing defeats a mid-run bound derived from console
# silence.  A wedged guest emits an RCU stall report and its task dump on the
# kernel's own timer -- every 63 seconds, whether or not the workload still
# runs -- so console growth made only of these lines is not evidence of
# progress.  Counting it cost one shard 4.4 hours of a guest slot: the CTS
# process was spinning in the guest kernel, which starved the in-guest
# watchdog as well, leaving this bound the only thing able to end the session.
KERNEL_CONSOLE_TIMESTAMP = re.compile(r"\[\s*\d+\.\d+\]\s*")
KERNEL_STALL_DIAGNOSTICS = (
    re.compile(r"rcu:"),
    re.compile(r"Task dump for CPU\b"),
    re.compile(r"task:\S+\s+state:"),
    re.compile(r"(?:Stack|Call Trace):"),
    re.compile(r"INFO: task \S+:\d+ blocked for more than"),
    re.compile(r"watchdog: BUG: soft lockup"),
)
# Beyond this much new output the classification is skipped: that volume is
# itself evidence of activity, and the bound must never cut a working guest.
MAX_CONSOLE_PROGRESS_SCAN = 4 * 1024 * 1024
QPA_SEGMENT = re.compile(r"results\.qpa\.([0-9]{4})\Z")
MAX_QPA_SEGMENTS = 4096
QPA_CHECKPOINT_BYTES = 8 * 1024 * 1024
QPA_BOUNDARY = "#umlSessionBoundary "
SESSION_DIRECTORY = "sessions"
DEFAULT_MAX_RESUMPTIONS = 4
SESSION_OUTCOMES = ("completed", "stalled", "aborted")
# dEQP announces each case on the guest console before it runs it.  The last
# such name is the only thing an aborted session says about where it died, and
# the host keeps it as a diagnostic: it is never turned into a case result.
CONSOLE_CASE = re.compile(r"Test case '([A-Za-z0-9][A-Za-z0-9_.:-]*)'")


class CampaignError(RuntimeError):
    """A campaign input or result violated a fail-closed contract."""


CAMPAIGN_FAILURE_ENV = "UML_CTS_CAMPAIGN_EMULATOR_FAILURE"


def report_campaign_emulator_failure(reason: str) -> None:
    """Notify the owning pool only about a host-observed emulator failure."""
    setting = os.environ.get(CAMPAIGN_FAILURE_ENV)
    if setting is None:
        return
    record = json.loads(setting)
    path = Path(record["path"])
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps({"token": record["token"], "reason": reason}))
    temporary.replace(path)


CAMPAIGN_EXIT_ENV = "UML_CTS_CAMPAIGN_LEAF_EXIT"


def report_campaign_leaf_exit(code: int) -> int:
    """Leave this leaf's exit status where a later pool can still read it.

    A pool that adopts a leaf started by an earlier pool is not its parent and
    cannot wait() for it, so the status has to outlive the process table entry.
    """
    setting = os.environ.get(CAMPAIGN_EXIT_ENV)
    if setting is not None:
        record = json.loads(setting)
        path = Path(record["path"])
        temporary = path.with_suffix(".tmp")
        temporary.write_text(json.dumps(
            {"token": record["token"], "return_code": int(code)}))
        temporary.replace(path)
    return code


class GuestSessionInterrupted(CampaignError, TimeoutError):
    """A guest session ended early; its partial evidence is still attached.

    It stays a ``TimeoutError`` so every existing caller keeps its behaviour,
    and carries the console and acknowledged frames the guest had already
    delivered so a shard can continue from real evidence.  ``delivered``
    records that the guest had already transported its final acknowledged
    frame, so the interruption cost the shard no coverage at all.
    """

    def __init__(
        self,
        message: str,
        *,
        reason: str,
        transcript: str,
        artifact_transcript: str,
        delivered: bool = False,
    ) -> None:
        super().__init__(message)
        if reason not in {"stalled", "aborted"}:
            raise CampaignError(f"invalid guest interruption reason: {reason}")
        self.reason = reason
        self.transcript = transcript
        self.artifact_transcript = artifact_transcript
        self.delivered = delivered


@dataclass(frozen=True)
class QpaPrefix:
    """Complete results of one QPA session plus its unfinished trailer."""

    results: dict[str, str]
    in_flight_case: str | None
    ended_session: bool


@dataclass(frozen=True)
class SessionEvidence:
    """One guest session of a shard, with the evidence it transported."""

    index: int
    outcome: str
    resume_after: int
    qpa_text: str
    completed_cases: tuple[str, ...]
    console_case: str | None
    console: str
    artifact_transport: str
    artifacts_directory: str
    return_code: int | None
    ownership_journal: str | None = None
    shutdown_stall: bool = False

    def record(self, directory: Path) -> dict[str, Any]:
        artifacts = directory / self.artifacts_directory
        return {
            "index": self.index,
            "outcome": self.outcome,
            "resume_after": self.resume_after,
            "completed_cases": len(self.completed_cases),
            "first_case": self.completed_cases[0] if self.completed_cases else None,
            "last_case": self.completed_cases[-1] if self.completed_cases else None,
            "console_case": self.console_case,
            "return_code": self.return_code,
            "console": self.console,
            "console_sha256": sha256_file(directory / self.console),
            "artifact_transport": self.artifact_transport,
            "artifact_transport_sha256": sha256_file(
                directory / self.artifact_transport
            ),
            "artifacts_directory": self.artifacts_directory,
            "ownership_journal": self.ownership_journal,
            "shutdown_stall": self.shutdown_stall,
            "artifact_sha256": {
                path.name: sha256_file(path)
                for path in sorted(artifacts.iterdir())
            },
        }


@dataclass(frozen=True)
class LeafIdentity:
    identifier: str
    path: str
    case_count: int
    sha256: str


@dataclass(frozen=True)
class Selection:
    leaf: LeafIdentity
    exact_cases: tuple[str, ...]
    fraction_index: int
    fraction_count: int

    @property
    def expected_count(self) -> int:
        if self.exact_cases:
            return len(self.exact_cases)
        remaining = self.leaf.case_count - self.fraction_index
        if remaining <= 0:
            return 0
        return (remaining + self.fraction_count - 1) // self.fraction_count


@dataclass(frozen=True)
class MustpassConfiguration:
    api: str
    leaf: LeafIdentity
    name: str
    use_for_first_egl_config: bool
    screen_rotation: str
    surface_width: int
    surface_height: int
    base_seed: int | None
    watchdog: str
    fbo_config: str | None
    xml_arguments: tuple[str, ...]
    # An AOSP record names its GL configuration instead of an EGL
    # configuration id, and carries the surface type its resolved
    # configuration offers (None where the record names none at all).
    aosp_config: str | None = None
    aosp_surface_type: str | None = None

    @property
    def uses_fbo_surface(self) -> bool:
        """Whether the record draws into the framework-owned framebuffer.

        The CTS runner selects that surface only for a record whose own
        command line names it; a record that names a GL config without it
        keeps the surface its EGL config provides.
        """
        return "--deqp-surface-type=fbo" in self.xml_arguments


@dataclass(frozen=True)
class EglConfig:
    identifier: int
    surface_types: tuple[str, ...]

    @property
    def selected_surface_type(self) -> str:
        for surface_type in ("window", "pbuffer", "pixmap"):
            if surface_type in self.surface_types:
                return surface_type
        raise CampaignError(
            f"EGL config {self.identifier} has no supported surface type"
        )


@dataclass(frozen=True)
class MatrixRun:
    configuration: MustpassConfiguration
    egl_config: EglConfig
    egl_config_index: int
    config_inventory_sha256: str
    selection: Selection
    mustpass_xml_path: str
    mustpass_xml_size: int
    mustpass_xml_sha256: str
    opengl_configuration_index: int | None = None


@dataclass(frozen=True)
class Waiver:
    """One granted CTS waiver: its scope, renderer binding and exact cases."""

    identifier: str
    granted: str
    granted_by: str
    api: str
    configuration_name: str
    draw_surface_type: str
    vendor: str
    renderers: tuple[str, ...]
    url: str
    description: str
    cases: tuple[str, ...]


@dataclass(frozen=True)
class RunnerOracle:
    path: str
    size: int
    sha256: str
    fixture_configs: tuple[EglConfig, ...]
    successful_case_statuses: tuple[str, ...]


@dataclass(frozen=True)
class AppleCampaign:
    identifier: str
    api: str
    leaf: LeafIdentity
    cases: tuple[str, ...]
    seed: int
    shard_timeout: int

    @property
    def selection(self) -> Selection:
        return Selection(self.leaf, self.cases, 0, 1)


@dataclass(frozen=True)
class AppleMatrixShard:
    identifier: str
    api: str
    matrix: MatrixRun
    oracle: RunnerOracle
    shard_timeout: int
    manifest_sha256: str
    selected_sha256: str
    renderer_backend: str = "virgl"

    @property
    def leaf(self) -> LeafIdentity:
        return self.matrix.selection.leaf

    @property
    def selection(self) -> Selection:
        return self.matrix.selection

    @property
    def seed(self) -> int:
        return self.matrix.configuration.base_seed


@dataclass
class EncodedArtifact:
    compressed_size: int
    sha256: str
    chunks: dict[int, str] = field(default_factory=dict)
    expected_chunks: int | None = None


@dataclass(frozen=True)
class RootfsContract:
    filesystem_type: str
    size: int
    sha256: str
    glcts_sha256: str
    filesystem_uuid: str | None = None
    guest_receipt_path: str | None = None
    guest_receipt_sha256: str | None = None


class AcknowledgedArtifactSocket:
    """Drain the guest Virtio serial stream and acknowledge every frame."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.connection: socket.socket | None = None
        self.input = bytearray()
        self.output = bytearray()
        self.lines: list[str] = []
        self.connected = False
        self.closed = False

    def try_connect(self) -> None:
        if self.connection is not None or self.connected:
            return
        try:
            details = self.path.lstat()
        except FileNotFoundError:
            return
        if stat.S_ISLNK(details.st_mode) or not stat.S_ISSOCK(details.st_mode):
            raise CampaignError(
                f"artifact transport path is not a Unix socket: {self.path}"
            )
        if (
            stat.S_IMODE(details.st_mode) != 0o600
            or details.st_uid != os.getuid()
        ):
            raise CampaignError("artifact transport socket ownership changed")
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        connection.settimeout(0.1)
        try:
            connection.connect(str(self.path))
        except OSError as error:
            connection.close()
            if error.errno in {errno.ENOENT, errno.ECONNREFUSED}:
                return
            raise CampaignError(
                f"cannot connect to artifact transport {self.path}: {error}"
            ) from error
        connection.setblocking(False)
        self.connection = connection
        self.connected = True

    def service(self, timeout: float = 0.0) -> None:
        """Perform one receive/send with a finite, nonnegative readiness wait."""
        self.try_connect()
        if self.connection is None:
            if timeout > 0:
                time.sleep(timeout)
            return
        readable, _, _ = select.select(
            [self.connection],
            [self.connection] if self.output else [],
            [],
            timeout,
        )
        if self.connection in readable:
            try:
                payload = self.connection.recv(64 * 1024)
            except BlockingIOError:
                payload = None
            except OSError as error:
                raise CampaignError(
                    f"artifact transport receive failed: {error}"
                ) from error
            if payload == b"":
                self.connection.close()
                self.connection = None
                self.closed = True
                if self.input or self.output:
                    raise CampaignError(
                        "artifact transport closed with an incomplete frame"
                    )
            elif payload:
                self.input.extend(payload)
                self._parse_input()
        # A received frame can queue an ACK after select's writable snapshot.
        # The socket is nonblocking: try it now, retaining any unsent suffix.
        if self.connection is not None and self.output:
            try:
                written = self.connection.send(self.output)
            except BlockingIOError:
                written = 0
            except OSError as error:
                raise CampaignError(
                    f"artifact transport acknowledgement failed: {error}"
                ) from error
            if written > 0:
                del self.output[:written]

    def _parse_input(self) -> None:
        while b"\n" in self.input:
            raw_line, _, remainder = self.input.partition(b"\n")
            self.input = bytearray(remainder)
            if (
                not raw_line
                or len(raw_line) > MAX_ARTIFACT_TRANSPORT_LINE
                or raw_line.endswith(b"\r")
            ):
                raise CampaignError("artifact transport emitted an invalid line")
            try:
                line = raw_line.decode("ascii", errors="strict")
            except UnicodeDecodeError as error:
                raise CampaignError(
                    "artifact transport emitted a non-ASCII line"
                ) from error
            acknowledgement = artifact_acknowledgement(line)
            self.lines.append(line + "\n")
            self.output.extend((acknowledgement + "\n").encode("ascii"))
        if len(self.input) > MAX_ARTIFACT_TRANSPORT_LINE:
            raise CampaignError("artifact transport line exceeds its bound")

    def finish(self) -> str:
        if not self.connected:
            raise CampaignError("guest never connected the artifact transport")
        if self.input or self.output:
            raise CampaignError("artifact transport ended with pending bytes")
        return "".join(self.lines)

    def close(self) -> None:
        if self.connection is not None:
            self.connection.close()
            self.connection = None


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise CampaignError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def load_identity(path: Path = IDENTITY_PATH) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise CampaignError(f"cannot read CTS identity {path}: {error}") from error
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise CampaignError("unsupported CTS identity schema")
    return value


def cts_binary_identity(identity: dict[str, Any], path: str) -> dict[str, Any]:
    try:
        rows = identity["cts"]["binaries"]
    except (KeyError, TypeError) as error:
        raise CampaignError("CTS identity has no binary inventory") from error
    if not isinstance(rows, list):
        raise CampaignError("CTS binary inventory is not a list")
    matches = [
        row
        for row in rows
        if isinstance(row, dict) and row.get("path") == path
    ]
    if len(matches) != 1:
        raise CampaignError(f"CTS identity does not uniquely identify {path}")
    row = matches[0]
    if (
        set(row) != {"path", "size", "sha256", "build_id"}
        or type(row["size"]) is not int
        or row["size"] < 1
        or not isinstance(row["sha256"], str)
        or SHA256.fullmatch(row["sha256"]) is None
        or not isinstance(row["build_id"], str)
        or re.fullmatch(r"[0-9a-f]+", row["build_id"]) is None
    ):
        raise CampaignError(f"invalid CTS binary identity for {path}")
    return row


def exclusion_digest(paths: Sequence[str]) -> str:
    payload = "".join(f"{path}\n" for path in paths).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def load_derivative_receipt(
    path: Path,
    base_identity: dict[str, Any],
) -> tuple[dict[str, Any], RootfsContract]:
    try:
        receipt = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise CampaignError(
            f"cannot read derivative rootfs receipt {path}: {error}"
        ) from error
    if (
        isinstance(receipt, dict)
        and receipt.get("schema_version") == 4
        and receipt.get("artifact") == "uml-vulkan-zink-gl-rootfs"
    ):
        return load_zink_gl_derivative_receipt(receipt, base_identity)
    if not isinstance(receipt, dict) or set(receipt) != {
        "schema_version",
        "artifact",
        "image",
        "base_rootfs",
        "clone",
        "guest_receipt",
        "cts",
    }:
        raise CampaignError("invalid derivative rootfs receipt")
    if (
        receipt["schema_version"] != 1
        or receipt["artifact"] != "uml-gl-gles-rootfs"
    ):
        raise CampaignError("unsupported derivative rootfs receipt")

    image = receipt["image"]
    base = receipt["base_rootfs"]
    clone = receipt["clone"]
    guest_receipt = receipt["guest_receipt"]
    cts = receipt["cts"]
    sections = (image, base, clone, guest_receipt, cts)
    if not all(isinstance(value, dict) for value in sections):
        raise CampaignError("derivative rootfs receipt sections are invalid")
    if set(image) != {
        "filesystem",
        "filesystem_uuid",
        "size",
        "sha256",
        "read_only",
    }:
        raise CampaignError("invalid derivative image identity")
    if (
        image["filesystem"] != "ext4"
        or not isinstance(image["filesystem_uuid"], str)
        or UUID.fullmatch(image["filesystem_uuid"]) is None
        or type(image["size"]) is not int
        or image["size"] != 8 * 1024 * 1024 * 1024
        or not isinstance(image["sha256"], str)
        or SHA256.fullmatch(image["sha256"]) is None
        or image["read_only"] is not True
    ):
        raise CampaignError("invalid derivative image identity")
    if set(base) != {"size", "sha256"} or base != {
        "size": base_identity["rootfs"]["size"],
        "sha256": base_identity["rootfs"]["sha256"],
    }:
        raise CampaignError("derivative receipt names a different base rootfs")
    if set(clone) != {"excluded_paths", "excluded_paths_sha256"}:
        raise CampaignError("invalid derivative clone identity")
    excluded = clone["excluded_paths"]
    if (
        not isinstance(excluded, list)
        or not excluded
        or any(
            not isinstance(item, str)
            or not item.startswith("/")
            or item.endswith("/")
            for item in excluded
        )
        or excluded != sorted(set(excluded))
        or clone["excluded_paths_sha256"] != exclusion_digest(excluded)
    ):
        raise CampaignError("invalid derivative exclusion identity")
    if set(guest_receipt) != {"path", "sha256"} or (
        not isinstance(guest_receipt["path"], str)
        or not guest_receipt["path"].startswith("/usr/share/uml-rootfs/")
        or not isinstance(guest_receipt["sha256"], str)
        or SHA256.fullmatch(guest_receipt["sha256"]) is None
    ):
        raise CampaignError("invalid in-guest build receipt identity")
    if (
        set(cts)
        != {"glcts_path", "glcts_size", "glcts_sha256", "glcts_build_id"}
        or cts["glcts_path"] != "/opt/vk-gl-cts/openglcts/glcts"
        or type(cts["glcts_size"]) is not int
        or cts["glcts_size"] < 1
        or not isinstance(cts["glcts_sha256"], str)
        or SHA256.fullmatch(cts["glcts_sha256"]) is None
        or not isinstance(cts["glcts_build_id"], str)
        or re.fullmatch(r"[0-9a-f]+", cts["glcts_build_id"]) is None
    ):
        raise CampaignError("invalid derivative CTS identity")
    return receipt, RootfsContract(
        filesystem_type="ext4",
        size=image["size"],
        sha256=image["sha256"],
        glcts_sha256=cts["glcts_sha256"],
        filesystem_uuid=image["filesystem_uuid"],
        guest_receipt_path=guest_receipt["path"],
        guest_receipt_sha256=guest_receipt["sha256"],
    )


def checked_file_identity(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != {"path", "size", "sha256"}:
        raise CampaignError(f"invalid {field} identity")
    if (
        not isinstance(value["path"], str)
        or not value["path"].startswith("/")
        or type(value["size"]) is not int
        or value["size"] < 1
        or not isinstance(value["sha256"], str)
        or SHA256.fullmatch(value["sha256"]) is None
    ):
        raise CampaignError(f"invalid {field} identity")
    return value


def load_zink_gl_derivative_receipt(
    receipt: dict[str, Any],
    base_identity: dict[str, Any],
) -> tuple[dict[str, Any], RootfsContract]:
    expected_keys = {
        "schema_version",
        "artifact",
        "claim_scope",
        "base_rootfs",
        "derivative",
        "protocol",
        "mesa",
        "vulkan_headers",
        "vulkan_loader",
        "cts",
        "opengl_cts",
        "toolchain",
        "install_closure",
        "mustpass",
        "runner_oracle",
        "exclusions",
        "base_packages",
        "guest_receipt",
    }
    if (set(receipt) != expected_keys
            or receipt["schema_version"] != 4
            or receipt["artifact"] != "uml-vulkan-zink-gl-rootfs"):
        raise CampaignError("invalid Zink/OpenGL derivative receipt")
    try:
        vulkan_cts_receipt.verify_source_epoch(receipt)
    except vulkan_cts_receipt.ReceiptError as error:
        raise CampaignError(f"Zink/OpenGL source epoch changed: {error}") from error
    scope = receipt["claim_scope"]
    if (
        not isinstance(scope, dict)
        or scope.get("backend") != "venus-kosmickrisp"
        or scope.get("downstream_guest_mesa") is not True
        or scope.get("formal_conformance") is not False
    ):
        raise CampaignError("invalid Zink/OpenGL qualification scope")
    base = receipt["base_rootfs"]
    image = receipt["derivative"]
    if not isinstance(base, dict) or not isinstance(image, dict):
        raise CampaignError("invalid Zink/OpenGL image identity")
    if set(base) != {
        "filename",
        "filesystem",
        "size",
        "sha256",
        "filesystem_uuid",
        "read_only",
    } or set(image) != {
        "filename",
        "filesystem",
        "size",
        "sha256",
        "filesystem_uuid",
        "read_only",
    }:
        raise CampaignError("invalid Zink/OpenGL image identity")
    if (
        base.get("size") != base_identity["rootfs"]["size"]
        or base.get("sha256") != base_identity["rootfs"]["sha256"]
        or image.get("filename") != "rootfs-vulkan-zink-gl.ext4"
        or image.get("filesystem") != "ext4"
        or image.get("size") != 8 * 1024 * 1024 * 1024
        or not isinstance(image.get("sha256"), str)
        or SHA256.fullmatch(image["sha256"]) is None
        or not isinstance(image.get("filesystem_uuid"), str)
        or UUID.fullmatch(image["filesystem_uuid"]) is None
        or image.get("read_only") is not True
    ):
        raise CampaignError("invalid Zink/OpenGL image identity")
    glcts = receipt["opengl_cts"]
    if not isinstance(glcts, dict) or set(glcts) != {
        "release",
        "configure_argv",
        "build_targets",
        "install_prefix",
        "executable",
        "resources",
        "build_id",
        "reported_cpu",
        "reported_target",
        "mustpass",
    }:
        raise CampaignError("invalid OpenGL CTS receipt")
    executable = checked_file_identity(
        glcts.get("executable"), "OpenGL CTS executable"
    )
    if (
        glcts.get("release") != "vulkan-cts-1.4.6.2"
        or glcts.get("install_prefix") != "/opt/vk-gl-cts/openglcts"
        or glcts.get("reported_cpu") != "DE_CPU_ARM_64"
        or glcts.get("reported_target") != "Default"
        or executable["path"] != "/opt/vk-gl-cts/openglcts/glcts"
        or not isinstance(glcts.get("build_id"), str)
        or re.fullmatch(r"[0-9a-f]+", glcts["build_id"]) is None
    ):
        raise CampaignError("invalid OpenGL CTS build identity")
    resources = glcts["resources"]
    expected_resources = {
        "/opt/vk-gl-cts/openglcts/gl_cts",
        "/opt/vk-gl-cts/openglcts/gles2",
        "/opt/vk-gl-cts/openglcts/gles3",
        "/opt/vk-gl-cts/openglcts/gles31",
    }
    if not isinstance(resources, list) or {
        row.get("path") for row in resources if isinstance(row, dict)
    } != expected_resources:
        raise CampaignError("invalid OpenGL CTS resource closure")
    for index, resource in enumerate(resources):
        if not isinstance(resource, dict) or set(resource) != {
            "path",
            "file_count",
            "total_bytes",
            "verified_against",
        }:
            raise CampaignError(f"invalid OpenGL CTS resource at {index}")
        if (
            type(resource["file_count"]) is not int
            or resource["file_count"] < 1
            or type(resource["total_bytes"]) is not int
            or resource["total_bytes"] < 1
            or not isinstance(resource["verified_against"], str)
            or not resource["verified_against"]
        ):
            raise CampaignError(f"invalid OpenGL CTS resource at {index}")
    try:
        opengl_cts_plan.validate_manifest(glcts.get("mustpass"))
    except opengl_cts_plan.PlanError as error:
        raise CampaignError(f"invalid OpenGL runner scope: {error}") from error
    guest_receipt = checked_file_identity(
        receipt["guest_receipt"], "in-guest build receipt"
    )
    if guest_receipt["path"] != "/usr/share/uml-vulkan/rootfs-build-receipt.json":
        raise CampaignError("invalid in-guest build receipt path")
    oracle = receipt["runner_oracle"]
    oracle_path = REPO_ROOT / "tools/rootfs/vulkan/status-oracle.json"
    checked_oracle = json.loads(oracle_path.read_text(encoding="utf-8"))
    if (
        not isinstance(oracle, dict)
        or set(oracle) != {"file", "successful", "always_failure", "qualification_authority"}
        or oracle["file"] != {
            "path": "/usr/share/uml-vulkan/status-oracle.json",
            "size": oracle_path.stat().st_size,
            "sha256": sha256_file(oracle_path),
        }
        or oracle["qualification_authority"] is not False
        or oracle["successful"] != checked_oracle["successful"]
        or oracle["always_failure"] != checked_oracle["always_failure"]
    ):
        raise CampaignError("Zink/OpenGL released status oracle changed")
    return receipt, RootfsContract(
        filesystem_type="ext4",
        size=image["size"],
        sha256=image["sha256"],
        glcts_sha256=executable["sha256"],
        filesystem_uuid=image["filesystem_uuid"],
        guest_receipt_path=guest_receipt["path"],
        guest_receipt_sha256=guest_receipt["sha256"],
    )


def select_rootfs_contract(
    identity: dict[str, Any],
    filesystem_type: str,
    receipt_path: Path | None,
) -> tuple[dict[str, Any] | None, RootfsContract]:
    if filesystem_type == "erofs":
        if receipt_path is not None:
            raise CampaignError("a derivative receipt requires an ext4 rootfs")
        glcts = cts_binary_identity(
            identity, "/opt/vk-gl-cts/openglcts/glcts"
        )
        return None, RootfsContract(
            filesystem_type="erofs",
            size=identity["rootfs"]["size"],
            sha256=identity["rootfs"]["sha256"],
            glcts_sha256=glcts["sha256"],
        )
    if filesystem_type != "ext4":
        raise CampaignError(f"unsupported root filesystem: {filesystem_type}")
    if receipt_path is None:
        raise CampaignError("an ext4 rootfs requires a derivative receipt")
    return load_derivative_receipt(receipt_path.resolve(), identity)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def verify_staged_kosmickrisp(emulator: Path) -> str:
    """Bind CTS evidence to the actual framework and reject stale CMake staging."""
    staged = emulator.parent / "Frameworks/KosmicKrisp.framework/KosmicKrisp"
    if not staged.is_file():
        raise CampaignError(f"staged KosmicKrisp provider is missing: {staged}")
    staged_sha256 = sha256_file(staged)
    cache = next(
        (parent / "CMakeCache.txt" for parent in emulator.parents
         if (parent / "CMakeCache.txt").is_file()),
        None,
    )
    if cache is None:
        return staged_sha256
    try:
        configured = next(
            (line.partition("=")[2]
             for line in cache.read_text(encoding="utf-8").splitlines()
             if line.startswith("UML_APPLE_GPU_KOSMICKRISP_FRAMEWORK:")
             and "=" in line),
            None,
        )
    except OSError as error:
        raise CampaignError(f"cannot read emulator CMake cache {cache}: {error}") from error
    if configured is None:
        raise CampaignError("experimental emulator CMake cache omits its KosmicKrisp provider")
    framework = Path(configured)
    if not framework.is_absolute():
        framework = cache.parent / framework
    source = framework / "KosmicKrisp"
    if not source.is_file():
        raise CampaignError(f"configured KosmicKrisp provider is missing: {source}")
    if sha256_file(source) != staged_sha256:
        raise CampaignError("staged KosmicKrisp provider is stale; rebuild the emulator target")
    return staged_sha256


def artifact_socket_path(artifact_directory: Path) -> Path:
    identity = hashlib.sha256(
        os.fsencode(artifact_directory.resolve())
    ).hexdigest()[:20]
    path = Path("/private/tmp") / f"uml-cts-artifact-{identity}.sock"
    if len(os.fsencode(path)) >= 104:
        raise CampaignError("CTS artifact socket path is too long")
    return path


def validate_rootfs_file(path: Path, contract: RootfsContract) -> None:
    if not path.is_file():
        raise CampaignError(f"rootfs is not a regular file: {path}")
    details = path.stat()
    if details.st_size != contract.size:
        raise CampaignError("rootfs size does not match its frozen identity")
    if contract.filesystem_type == "ext4" and details.st_mode & (
        stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH
    ):
        raise CampaignError("derivative ext4 rootfs still has host write permission")
    if sha256_file(path) != contract.sha256:
        raise CampaignError("rootfs digest does not match its frozen identity")


def leaf_identities(identity: dict[str, Any]) -> dict[str, LeafIdentity]:
    result: dict[str, LeafIdentity] = {}
    try:
        rows = identity["mustpass"]["leaves"]
    except (KeyError, TypeError) as error:
        raise CampaignError("CTS identity has no must-pass leaves") from error
    if not isinstance(rows, list):
        raise CampaignError("CTS must-pass leaves are not a list")
    for row in rows:
        try:
            leaf = LeafIdentity(
                identifier=row["id"],
                path=row["path"],
                case_count=row["case_count"],
                sha256=row["sha256"],
            )
        except (KeyError, TypeError) as error:
            raise CampaignError("invalid CTS leaf identity") from error
        if (
            not isinstance(leaf.identifier, str)
            or not isinstance(leaf.path, str)
            or not isinstance(leaf.case_count, int)
            or leaf.case_count <= 0
            or not isinstance(leaf.sha256, str)
            or not SHA256.fullmatch(leaf.sha256)
        ):
            raise CampaignError(f"invalid CTS leaf identity: {leaf.identifier!r}")
        if leaf.identifier in result:
            raise CampaignError(f"duplicate CTS leaf identity: {leaf.identifier}")
        result[leaf.identifier] = leaf
    return result


def parse_mustpass_command(arguments: tuple[str, ...]) -> dict[str, Any]:
    values: dict[str, str] = {}
    allowed = {
        "deqp-screen-rotation",
        "deqp-surface-width",
        "deqp-surface-height",
        "deqp-base-seed",
        "deqp-gl-config-name",
        "deqp-surface-type",
        "deqp-watchdog",
    }
    for argument in arguments:
        match = re.fullmatch(r"--([a-z0-9-]+)=([^\s]+)", argument)
        if match is None or match.group(1) not in allowed:
            raise CampaignError(
                f"unsupported GLES must-pass argument: {argument!r}"
            )
        key, value = match.groups()
        if key in values:
            raise CampaignError(f"duplicate GLES must-pass argument: {key}")
        values[key] = value

    required = {
        "deqp-screen-rotation",
        "deqp-surface-width",
        "deqp-surface-height",
        "deqp-base-seed",
        "deqp-watchdog",
    }
    if not required.issubset(values):
        raise CampaignError("GLES must-pass command is incomplete")
    if values["deqp-screen-rotation"] != "unspecified":
        raise CampaignError("unsupported GLES must-pass screen rotation")
    if values["deqp-watchdog"] != "disable":
        raise CampaignError("unsupported GLES must-pass watchdog policy")
    try:
        surface_width = int(values["deqp-surface-width"])
        surface_height = int(values["deqp-surface-height"])
        base_seed = int(values["deqp-base-seed"])
    except ValueError as error:
        raise CampaignError("GLES must-pass numeric argument is invalid") from error
    if (
        surface_width == 0
        or surface_width < -1
        or surface_height == 0
        or surface_height < -1
        or base_seed < 0
        or base_seed > 0xFFFFFFFF
    ):
        raise CampaignError("GLES must-pass numeric argument is out of range")

    gl_config = values.get("deqp-gl-config-name")
    surface_type = values.get("deqp-surface-type")
    if (gl_config is None) != (surface_type is None):
        raise CampaignError("GLES must-pass FBO configuration is incomplete")
    if gl_config is not None and (
        re.fullmatch(r"[a-z0-9]+", gl_config) is None
        or surface_type != "fbo"
    ):
        raise CampaignError("unsupported GLES must-pass FBO configuration")
    return {
        "screen_rotation": values["deqp-screen-rotation"],
        "surface_width": surface_width,
        "surface_height": surface_height,
        "base_seed": base_seed,
        "watchdog": values["deqp-watchdog"],
        "fbo_config": gl_config,
    }


def load_gles_mustpass(
    identity: dict[str, Any] | None = None,
    path: Path = GLES_MUSTPASS_PATH,
) -> dict[str, tuple[MustpassConfiguration, ...]]:
    if identity is None:
        identity = load_identity()
    try:
        xml_identity = identity["mustpass"]["gles_xml"]
    except (KeyError, TypeError) as error:
        raise CampaignError("CTS identity has no GLES must-pass XML") from error
    if (
        not isinstance(xml_identity, dict)
        or set(xml_identity) != {"path", "size", "sha256"}
        or type(xml_identity["size"]) is not int
        or xml_identity["size"] < 1
        or not isinstance(xml_identity["sha256"], str)
        or SHA256.fullmatch(xml_identity["sha256"]) is None
    ):
        raise CampaignError("invalid GLES must-pass XML identity")
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise CampaignError(f"cannot read GLES must-pass XML {path}: {error}") from error
    if len(payload) != xml_identity["size"]:
        raise CampaignError("repository GLES must-pass XML size changed")
    if hashlib.sha256(payload).hexdigest() != xml_identity["sha256"]:
        raise CampaignError("repository GLES must-pass XML identity changed")
    try:
        root = ElementTree.fromstring(payload)
    except ElementTree.ParseError as error:
        raise CampaignError("repository GLES must-pass XML is invalid") from error
    if root.tag != "Mustpass" or root.attrib != {"version": "main"}:
        raise CampaignError("unsupported GLES must-pass document")
    packages = list(root)
    if len(packages) != 1:
        raise CampaignError("GLES must-pass document has an unexpected package set")
    package = packages[0]
    if package.tag != "TestPackage" or package.attrib != {
        "name": "Khronos Mustpass ES"
    }:
        raise CampaignError("unsupported GLES must-pass package")

    leaves = leaf_identities(identity)
    leaves_by_name = {Path(leaf.path).name: leaf for leaf in leaves.values()}
    rows: dict[str, list[MustpassConfiguration]] = {}
    required_attributes = {
        "caseListFile",
        "commandLine",
        "name",
        "os",
        "useForFirstEGLConfig",
    }
    for element in package:
        if (
            element.tag != "Configuration"
            or set(element.attrib) != required_attributes
            or list(element)
            or (element.text is not None and element.text.strip())
        ):
            raise CampaignError("invalid GLES must-pass configuration")
        case_list_file = element.attrib["caseListFile"]
        if Path(case_list_file).name != case_list_file:
            raise CampaignError("unsafe GLES must-pass caselist path")
        leaf = leaves_by_name.get(case_list_file)
        if leaf is None:
            raise CampaignError(
                f"unidentified GLES must-pass caselist: {case_list_file}"
            )
        if element.attrib["os"] != "any" or re.fullmatch(
            r"[a-z0-9-]+", element.attrib["name"]
        ) is None:
            raise CampaignError("unsupported GLES must-pass configuration scope")
        first_text = element.attrib["useForFirstEGLConfig"]
        if first_text not in {"True", "False"}:
            raise CampaignError("invalid GLES first-config selector")
        try:
            xml_arguments = tuple(shlex.split(element.attrib["commandLine"]))
        except ValueError as error:
            raise CampaignError("invalid GLES must-pass command line") from error
        values = parse_mustpass_command(xml_arguments)
        configuration = MustpassConfiguration(
            api=leaf.identifier,
            leaf=leaf,
            name=element.attrib["name"],
            use_for_first_egl_config=first_text == "True",
            xml_arguments=xml_arguments,
            **values,
        )
        rows.setdefault(leaf.identifier, []).append(configuration)

    result = {api: tuple(configurations) for api, configurations in rows.items()}
    for api in BASELINE_GLES_APIS:
        configurations = result.get(api, ())
        if len(configurations) != 2 or {
            item.use_for_first_egl_config for item in configurations
        } != {True, False}:
            raise CampaignError(
                f"GLES must-pass XML does not define the exact {api} split"
            )
    return result


def baseline_mustpass_configuration(
    configurations: dict[str, tuple[MustpassConfiguration, ...]],
    api: str,
    first: bool,
) -> MustpassConfiguration:
    matches = [
        item
        for item in configurations.get(api, ())
        if item.use_for_first_egl_config is first
    ]
    if len(matches) != 1:
        raise CampaignError(f"ambiguous {api} must-pass configuration split")
    return matches[0]


def matrix_runner_arguments(
    matrix: MatrixRun,
    caselist_path: str,
    waiver_path: str | None = None,
) -> list[str]:
    configuration = matrix.configuration
    arguments = [
        f"--deqp-caselist-file={caselist_path}",
        f"--deqp-screen-rotation={configuration.screen_rotation}",
        f"--deqp-surface-width={configuration.surface_width}",
        f"--deqp-surface-height={configuration.surface_height}",
        f"--deqp-watchdog={configuration.watchdog}",
    ]
    if configuration.aosp_config is not None:
        # getTestRunsForAOSPEGL and getTestRunsForAOSPES name the record's own
        # GL configuration and, for the ES records, the surface type their
        # resolved configuration offers. Neither seeds the run or names an EGL
        # configuration id.
        arguments.append(f"--deqp-gl-config-name={configuration.aosp_config}")
        if configuration.aosp_surface_type is not None:
            arguments.append(
                f"--deqp-surface-type={configuration.aosp_surface_type}"
            )
        if waiver_path is not None:
            arguments.append(f"--deqp-waiver-file={waiver_path}")
        return arguments
    arguments.append(f"--deqp-base-seed={configuration.base_seed}")
    if configuration.uses_fbo_surface:
        arguments.extend(
            (
                f"--deqp-gl-config-name={configuration.fbo_config}",
                "--deqp-surface-type=fbo",
            )
        )
    else:
        arguments.append(
            f"--deqp-surface-type={matrix.egl_config.selected_surface_type}"
        )
    arguments.extend(
        (
            f"--deqp-gl-config-id={matrix.egl_config.identifier}",
            "--deqp-gl-context-type=egl",
        )
    )
    if waiver_path is not None:
        arguments.append(f"--deqp-waiver-file={waiver_path}")
    return arguments


def load_waivers(path: Path = WAIVERS_PATH) -> tuple[Waiver, ...]:
    """Load the checked waiver list; every field is exact and fail-closed."""
    try:
        value = json.loads(
            path.read_bytes().decode("utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CampaignError(f"cannot read CTS waiver list {path}: {error}") from error
    if (
        not isinstance(value, dict)
        or set(value) != {"schema_version", "waivers"}
        or value["schema_version"] != 1
        or not isinstance(value["waivers"], list)
        or not value["waivers"]
    ):
        raise CampaignError("invalid CTS waiver list")
    waivers: list[Waiver] = []
    seen_identifiers: set[str] = set()
    seen_cases: set[str] = set()
    for row in value["waivers"]:
        if not isinstance(row, dict) or set(row) != WAIVER_KEYS:
            raise CampaignError("invalid CTS waiver entry")
        scope = row["scope"]
        if not isinstance(scope, dict) or set(scope) != WAIVER_SCOPE_KEYS:
            raise CampaignError("invalid CTS waiver scope")
        texts = [
            row["id"], row["granted"], row["granted_by"], row["vendor"],
            row["url"], row["description"], *scope.values(),
        ]
        if any(not isinstance(text, str) or not text.strip() for text in texts):
            raise CampaignError("CTS waiver fields must be non-empty strings")
        if scope["draw_surface_type"] not in WAIVER_SURFACE_TYPES:
            raise CampaignError("CTS waiver names an unknown draw surface type")
        renderers = row["renderers"]
        cases = row["cases"]
        if (
            not isinstance(renderers, list)
            or not renderers
            or any(not isinstance(item, str) or not item.strip() for item in renderers)
        ):
            raise CampaignError("CTS waiver needs renderer patterns")
        if (
            not isinstance(cases, list)
            or not cases
            or any(
                not isinstance(case, str) or CASE_NAME.fullmatch(case) is None
                for case in cases
            )
            or cases != sorted(set(cases))
        ):
            raise CampaignError("CTS waiver cases must be sorted unique case names")
        if row["id"] in seen_identifiers or seen_cases.intersection(cases):
            raise CampaignError("CTS waiver repeats an id or a case")
        seen_identifiers.add(row["id"])
        seen_cases.update(cases)
        waivers.append(
            Waiver(
                identifier=row["id"],
                granted=row["granted"],
                granted_by=row["granted_by"],
                api=scope["api"],
                configuration_name=scope["configuration_name"],
                draw_surface_type=scope["draw_surface_type"],
                vendor=row["vendor"],
                renderers=tuple(renderers),
                url=row["url"],
                description=row["description"],
                cases=tuple(cases),
            )
        )
    return tuple(waivers)


def matrix_draw_surface_type(matrix: MatrixRun) -> str:
    if matrix.configuration.aosp_config is not None:
        return matrix.configuration.aosp_surface_type or "none"
    if matrix.configuration.uses_fbo_surface:
        return "fbo"
    return matrix.egl_config.selected_surface_type


def applicable_waivers(
    matrix: MatrixRun, waivers: Sequence[Waiver]
) -> tuple[Waiver, ...]:
    """Return the waivers whose scope names exactly this matrix run.

    GL matrices carry ``gl46`` as their configuration API while their leaf is
    the XML-referenced list (``gl45-es3-main-es-for-gl``); GLES matrices carry
    their own API (``gles3``), so the configuration API is the scope key.
    """
    api = matrix.configuration.api
    surface = matrix_draw_surface_type(matrix)
    return tuple(
        waiver
        for waiver in waivers
        if waiver.api == api
        and waiver.configuration_name == matrix.configuration.name
        and waiver.draw_surface_type == surface
    )


def waived_cases(waivers: Sequence[Waiver]) -> frozenset[str]:
    return frozenset(case for waiver in waivers for case in waiver.cases)


def waiver_xml(waivers: Sequence[Waiver]) -> str:
    """Render the official CTS waiver file consumed by ``--deqp-waiver-file``."""
    lines = ['<?xml version="1.0" encoding="utf-8"?>', "<waiver_list>"]
    for waiver in waivers:
        lines.append(
            f"\t<waiver vendor={xml_quoteattr(waiver.vendor)} "
            f"url={xml_quoteattr(waiver.url)}>"
        )
        lines.append(
            f"\t\t<description>{xml_escape(waiver.description)}</description>"
        )
        lines.append("\t\t<renderer_list>")
        for renderer in waiver.renderers:
            lines.append(f"\t\t\t<r>{xml_escape(renderer)}</r>")
        lines.append("\t\t</renderer_list>")
        for case in waiver.cases:
            lines.append(f"\t\t<t>{xml_escape(case)}</t>")
        lines.append("\t</waiver>")
    lines.append("</waiver_list>")
    return "\n".join(lines) + "\n"


def validate_waiver_session(qpa_text: str, waivers: Sequence[Waiver]) -> None:
    """Require the guest runner to have applied exactly the granted waivers.

    The official runner records the URL of every waiver whose vendor and
    renderer matched in a ``#sessionInfo waiverUrls`` line; a missing line
    means the list never reached the guest or named another renderer, and a
    present line without a granted list means a foreign waiver was applied.
    """
    recorded: set[str] = set()
    for line in qpa_text.splitlines():
        if line.startswith("#sessionInfo waiverUrls "):
            urls = line[len("#sessionInfo waiverUrls "):].strip()
            if len(urls) < 2 or urls[0] != '"' or urls[-1] != '"':
                raise CampaignError("official CTS runner recorded a malformed waiver line")
            recorded.update(urls[1:-1].split())
    expected = {waiver.url for waiver in waivers}
    if recorded != expected:
        raise CampaignError(
            "official CTS runner applied waivers "
            f"{sorted(recorded)!r}, expected {sorted(expected)!r}"
        )


def load_runner_oracle(
    identity: dict[str, Any] | None = None,
    configurations: dict[
        str, tuple[MustpassConfiguration, ...]
    ] | None = None,
    path: Path = RUNNER_ORACLE_PATH,
) -> RunnerOracle:
    if identity is None:
        identity = load_identity()
    if configurations is None:
        configurations = load_gles_mustpass(identity)
    try:
        payload = path.read_bytes()
        value = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CampaignError(
            f"cannot read CTS runner oracle {path}: {error}"
        ) from error
    if not isinstance(value, dict) or set(value) != {
        "schema_version",
        "cts_release",
        "mustpass_xml_sha256",
        "unwaived_successful_case_statuses",
        "oracle_build",
        "fixture_egl_configs",
        "summaries",
    }:
        raise CampaignError("invalid CTS runner oracle")
    if (
        value["schema_version"] != 1
        or value["cts_release"] != identity["cts"]["release_label"]
        or value["mustpass_xml_sha256"]
        != identity["mustpass"]["gles_xml"]["sha256"]
    ):
        raise CampaignError("CTS runner oracle names another release")
    successful_case_statuses = value["unwaived_successful_case_statuses"]
    if successful_case_statuses != [
        "Pass",
        "NotSupported",
        "QualityWarning",
        "CompatibilityWarning",
    ]:
        raise CampaignError("CTS runner oracle has invalid success statuses")

    build = value["oracle_build"]
    build_keys = {
        "platform",
        "build_type",
        "target",
        "fixture_scope",
        "original_runner_sha256",
        "fixture_runner_sha256",
        "generated_gles_header_sha256",
        "binary_sha256",
        "omitted_unrelated_subdirectories",
    }
    if not isinstance(build, dict) or set(build) != build_keys:
        raise CampaignError("invalid CTS runner oracle build identity")
    digests = (
        build["original_runner_sha256"],
        build["fixture_runner_sha256"],
        build["generated_gles_header_sha256"],
        build["binary_sha256"],
    )
    omitted = build["omitted_unrelated_subdirectories"]
    if (
        build["platform"] != "macos-arm64"
        or build["build_type"] != "Release"
        or build["target"] != "cts-runner"
        or build["fixture_scope"]
        != "replace-platform-config-enumeration-only"
        or any(
            not isinstance(item, str) or SHA256.fullmatch(item) is None
            for item in digests
        )
        or digests[0] == digests[1]
        or not isinstance(omitted, list)
        or omitted != sorted(set(omitted))
        or any(
            not isinstance(item, str)
            or item.startswith("/")
            or ".." in Path(item).parts
            for item in omitted
        )
    ):
        raise CampaignError("invalid CTS runner oracle build identity")

    fixture_configs = egl_configs_from_json(value["fixture_egl_configs"])
    if len(fixture_configs) < 3:
        raise CampaignError("CTS runner oracle has too few EGL fixtures")
    summaries = value["summaries"]
    if not isinstance(summaries, dict) or set(summaries) != set(
        BASELINE_GLES_APIS
    ):
        raise CampaignError("CTS runner oracle has an invalid API set")
    for api in BASELINE_GLES_APIS:
        summary = summaries[api]
        if not isinstance(summary, dict) or set(summary) != {
            "runner_type",
            "exit_code",
            "summary_sha256",
            "configuration_roles",
            "runner_arguments",
        }:
            raise CampaignError(f"invalid CTS runner oracle summary for {api}")
        roles = summary["configuration_roles"]
        rows = summary["runner_arguments"]
        if (
            summary["runner_type"] != api.replace("gles", "es")
            or summary["exit_code"] != 1
            or not isinstance(summary["summary_sha256"], str)
            or SHA256.fullmatch(summary["summary_sha256"]) is None
            or roles != ["first"] + ["other"] * (len(fixture_configs) - 1)
            or not isinstance(rows, list)
            or len(rows) != len(fixture_configs)
        ):
            raise CampaignError(f"invalid CTS runner oracle summary for {api}")
        relative_caselist = (
            "gl_cts/data/mustpass/gles/khronos_mustpass/main/"
            f"{Path(leaf_identities(identity)[api].path).name}"
        )
        for index, (config, arguments) in enumerate(
            zip(fixture_configs, rows, strict=True)
        ):
            if (
                not isinstance(arguments, list)
                or not arguments
                or any(not isinstance(argument, str) for argument in arguments)
            ):
                raise CampaignError(
                    f"invalid CTS runner oracle arguments for {api}"
                )
            configuration = baseline_mustpass_configuration(
                configurations, api, index == 0
            )
            matrix = MatrixRun(
                configuration=configuration,
                egl_config=config,
                egl_config_index=index,
                config_inventory_sha256="0" * 64,
                selection=Selection(configuration.leaf, (), 0, 1),
                mustpass_xml_path=identity["mustpass"]["gles_xml"]["path"],
                mustpass_xml_size=identity["mustpass"]["gles_xml"]["size"],
                mustpass_xml_sha256=identity["mustpass"]["gles_xml"][
                    "sha256"
                ],
            )
            expected = matrix_runner_arguments(matrix, relative_caselist)
            if arguments != expected:
                raise CampaignError(
                    f"CTS runner oracle expansion changed for {api} "
                    f"config index {index}"
                )
    resolved_path = path.resolve()
    try:
        recorded_path = str(resolved_path.relative_to(REPO_ROOT))
    except ValueError:
        recorded_path = str(resolved_path)
    return RunnerOracle(
        path=recorded_path,
        size=len(payload),
        sha256=hashlib.sha256(payload).hexdigest(),
        fixture_configs=fixture_configs,
        successful_case_statuses=tuple(successful_case_statuses),
    )


def load_apple_campaigns(
    identity: dict[str, Any] | None = None,
    path: Path = APPLE_CAMPAIGNS_PATH,
) -> dict[str, AppleCampaign]:
    if identity is None:
        identity = load_identity()
    leaves = leaf_identities(identity)
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise CampaignError(
            f"cannot read Apple CTS campaigns {path}: {error}"
        ) from error
    if not isinstance(value, dict) or set(value) != {
        "schema_version",
        "campaigns",
    }:
        raise CampaignError("invalid Apple CTS campaign document")
    if value["schema_version"] != 2 or not isinstance(
        value["campaigns"], list
    ):
        raise CampaignError("unsupported Apple CTS campaign schema")

    result: dict[str, AppleCampaign] = {}
    required_keys = {
        "id",
        "api",
        "leaf",
        "cases",
        "seed",
        "shard_timeout",
    }
    for row in value["campaigns"]:
        if not isinstance(row, dict) or set(row) != required_keys:
            raise CampaignError("invalid Apple CTS campaign row")
        identifier = row["id"]
        api = row["api"]
        leaf_identifier = row["leaf"]
        cases = row["cases"]
        seed = row["seed"]
        shard_timeout = row["shard_timeout"]
        if (
            not isinstance(identifier, str)
            or not APPLE_CAMPAIGN_ID.fullmatch(identifier)
            or not isinstance(api, str)
            or not isinstance(leaf_identifier, str)
            or not isinstance(cases, list)
            or not 1 <= len(cases) <= 64
            or any(
                not isinstance(case, str) or not CASE_NAME.fullmatch(case)
                for case in cases
            )
            or len(set(cases)) != len(cases)
            or type(seed) is not int
            or seed < 0
            or seed > 0xFFFFFFFF
            or type(shard_timeout) is not int
            or shard_timeout < 1
            or shard_timeout > 3600
        ):
            raise CampaignError(f"invalid Apple CTS campaign: {identifier!r}")
        leaf = leaves.get(leaf_identifier)
        if leaf is None or api != leaf.identifier.split("-", 1)[0]:
            raise CampaignError(f"invalid Apple CTS campaign: {identifier!r}")
        campaign = AppleCampaign(
            identifier=identifier,
            api=api,
            leaf=leaf,
            cases=tuple(cases),
            seed=seed,
            shard_timeout=shard_timeout,
        )
        if campaign.identifier in result:
            raise CampaignError(
                f"duplicate Apple CTS campaign: {campaign.identifier}"
            )
        result[campaign.identifier] = campaign
    if not result:
        raise CampaignError("Apple CTS campaign allow-list is empty")
    return result


def load_apple_matrix_shards(
    identity: dict[str, Any] | None = None,
    path: Path = APPLE_MATRIX_PATH,
    receipt: dict[str, Any] | None = None,
) -> dict[str, AppleMatrixShard]:
    """Expand the pinned Apple matrix into one shard per runnable cell.

    The GLES rows come from the base image identity alone. The desktop GL row
    names a cell of the official GL46 must-pass matrix, which lives in the
    sealed derivative receipt, so it expands only when that receipt is given.
    """
    if identity is None:
        identity = load_identity()
    try:
        payload = path.read_bytes()
        value = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CampaignError(
            f"cannot read Apple CTS matrix {path}: {error}"
        ) from error
    if not isinstance(value, dict) or set(value) != {
        "schema_version",
        "config_inventory",
        "apis",
        "opengl",
    }:
        raise CampaignError("invalid Apple CTS matrix document")
    if value["schema_version"] != 3:
        raise CampaignError("unsupported Apple CTS matrix schema")

    inventory = value["config_inventory"]
    if not isinstance(inventory, dict) or set(inventory) != {
        "sha256",
        "ids",
        "surface_types",
        "selected_surface_type",
    }:
        raise CampaignError("invalid Apple CTS matrix config inventory")
    digest = inventory["sha256"]
    identifiers = inventory["ids"]
    surface_types = inventory["surface_types"]
    selected_surface_type = inventory["selected_surface_type"]
    if (
        not isinstance(digest, str)
        or SHA256.fullmatch(digest) is None
        or not isinstance(identifiers, list)
        or not identifiers
        or any(
            type(identifier) is not int or identifier < 1
            for identifier in identifiers
        )
        or len(set(identifiers)) != len(identifiers)
        or not isinstance(surface_types, list)
        or not surface_types
        or any(
            surface_type not in {"window", "pixmap", "pbuffer"}
            for surface_type in surface_types
        )
        or len(set(surface_types)) != len(surface_types)
        or selected_surface_type not in {"window", "pixmap", "pbuffer"}
    ):
        raise CampaignError("invalid Apple CTS matrix config geometry")
    configs = tuple(
        EglConfig(identifier, tuple(surface_types))
        for identifier in identifiers
    )
    if (
        any(
            config.selected_surface_type != selected_surface_type
            for config in configs
        )
        or egl_config_digest(configs) != digest
    ):
        raise CampaignError("Apple CTS matrix config digest changed")

    api_rows = value["apis"]
    if (
        not isinstance(api_rows, dict)
        or set(api_rows) != set(BASELINE_GLES_APIS)
    ):
        raise CampaignError("Apple CTS matrix API set changed")
    mustpass = load_gles_mustpass(identity)
    oracle = load_runner_oracle(identity, mustpass)
    leaves = leaf_identities(identity)
    xml_identity = identity["mustpass"]["gles_xml"]
    manifest_sha256 = hashlib.sha256(payload).hexdigest()
    result: dict[str, AppleMatrixShard] = {}
    for api in BASELINE_GLES_APIS:
        row = api_rows[api]
        if not isinstance(row, dict) or set(row) != {
            "fraction_count",
            "selected_sha256",
            "shard_timeout",
        }:
            raise CampaignError(f"invalid Apple CTS matrix row for {api}")
        fraction_count = row["fraction_count"]
        selected_sha256 = row["selected_sha256"]
        shard_timeout = row["shard_timeout"]
        leaf = leaves[api]
        if (
            type(fraction_count) is not int
            or fraction_count < 1
            or fraction_count > min(64, leaf.case_count)
            or not isinstance(selected_sha256, list)
            or len(selected_sha256) != fraction_count
            or any(
                not isinstance(digest, str)
                or SHA256.fullmatch(digest) is None
                for digest in selected_sha256
            )
            or type(shard_timeout) is not int
            or shard_timeout < 1
            or shard_timeout > 7200
        ):
            raise CampaignError(
                f"invalid Apple CTS matrix bounds for {api}"
            )
        for config_index, config in enumerate(configs):
            for fraction_index in range(fraction_count):
                identifier = (
                    f"{api}-matrix-cfg{config_index:02d}-"
                    f"{fraction_index:03d}of{fraction_count:03d}-v1"
                )
                if APPLE_CAMPAIGN_ID.fullmatch(identifier) is None:
                    raise CampaignError(
                        f"invalid Apple CTS matrix identifier: {identifier}"
                    )
                selection = Selection(
                    leaf, (), fraction_index, fraction_count
                )
                configuration = baseline_mustpass_configuration(
                    mustpass, api, config_index == 0
                )
                matrix = MatrixRun(
                    configuration=configuration,
                    egl_config=config,
                    egl_config_index=config_index,
                    config_inventory_sha256=digest,
                    selection=selection,
                    mustpass_xml_path=xml_identity["path"],
                    mustpass_xml_size=xml_identity["size"],
                    mustpass_xml_sha256=xml_identity["sha256"],
                )
                result[identifier] = AppleMatrixShard(
                    identifier=identifier,
                    api=api,
                    matrix=matrix,
                    oracle=oracle,
                    shard_timeout=shard_timeout,
                    manifest_sha256=manifest_sha256,
                    selected_sha256=selected_sha256[fraction_index],
                )
    expected_shards = len(configs) * sum(
        row["fraction_count"] for row in api_rows.values()
    )
    expected_shards += append_apple_opengl_matrix_shards(
        result, value["opengl"], configs, digest, manifest_sha256, receipt
    )
    if len(result) != expected_shards:
        raise CampaignError("Apple CTS matrix generated duplicate shards")
    return result


def append_apple_opengl_matrix_shards(
    result: dict[str, AppleMatrixShard],
    row: Any,
    configs: tuple[EglConfig, ...],
    digest: str,
    manifest_sha256: str,
    receipt: dict[str, Any] | None,
) -> int:
    """Expand the pinned desktop GL matrix cell, once per renderer backend.

    The cell is the same one the macOS lane runs, so the two stacks are
    compared on one caselist; the backend only selects which guest driver
    reaches the host.
    """
    if not isinstance(row, dict) or set(row) != {
        "egl_config_index",
        "configuration_index",
        "fraction_index",
        "fraction_count",
        "selected_sha256",
        "shard_timeout",
        "renderer_backends",
    }:
        raise CampaignError("invalid Apple CTS matrix OpenGL row")
    config_index = row["egl_config_index"]
    configuration_index = row["configuration_index"]
    fraction_index = row["fraction_index"]
    fraction_count = row["fraction_count"]
    selected_sha256 = row["selected_sha256"]
    shard_timeout = row["shard_timeout"]
    backends = row["renderer_backends"]
    if (
        type(config_index) is not int
        or not 0 <= config_index < len(configs)
        or type(configuration_index) is not int
        or configuration_index < 0
        or type(fraction_count) is not int
        or not 1 <= fraction_count <= 64
        or type(fraction_index) is not int
        or not 0 <= fraction_index < fraction_count
        or not isinstance(selected_sha256, str)
        or SHA256.fullmatch(selected_sha256) is None
        or type(shard_timeout) is not int
        or not 1 <= shard_timeout <= 7200
        or not isinstance(backends, list)
        or not backends
        or any(backend not in {"virgl", "zink"} for backend in backends)
        or len(set(backends)) != len(backends)
    ):
        raise CampaignError("invalid Apple CTS matrix OpenGL bounds")
    if receipt is None:
        return 0
    matrix = opengl_matrix_run(
        receipt, configs, digest, config_index, configuration_index,
        (fraction_index, fraction_count),
    )
    source = receipt["opengl_cts"]["mustpass"]["runner_source"]
    oracle = RunnerOracle(
        source["path"], source["size"], source["sha256"], (),
        tuple(receipt["runner_oracle"]["successful"]),
    )
    for backend in backends:
        identifier = (
            f"gl46-matrix-cfg{config_index:02d}-"
            f"xml{configuration_index:02d}-"
            f"{fraction_index:03d}of{fraction_count:03d}-{backend}-v1"
        )
        if APPLE_CAMPAIGN_ID.fullmatch(identifier) is None:
            raise CampaignError(
                f"invalid Apple CTS matrix identifier: {identifier}"
            )
        result[identifier] = AppleMatrixShard(
            identifier=identifier,
            api="gl46",
            matrix=matrix,
            oracle=oracle,
            shard_timeout=shard_timeout,
            manifest_sha256=manifest_sha256,
            selected_sha256=selected_sha256,
            renderer_backend=backend,
        )
    return len(backends)


def parse_fraction(value: str) -> tuple[int, int]:
    match = re.fullmatch(r"([0-9]+),([0-9]+)", value)
    if match is None:
        raise argparse.ArgumentTypeError("must be N,M with zero-based N")
    index, count = (int(item) for item in match.groups())
    if count < 1 or index >= count:
        raise argparse.ArgumentTypeError("fraction requires 0 <= N < M")
    return index, count


def exact_case(value: str) -> str:
    if CASE_NAME.fullmatch(value) is None:
        raise argparse.ArgumentTypeError(
            "must be one exact CTS case name without wildcards or commas"
        )
    return value


def positive_integer(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def nonnegative_integer(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
    return parsed


def parse_case_list(text: str) -> list[str]:
    cases: list[str] = []
    seen: set[str] = set()
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if CASE_NAME.fullmatch(line) is None:
            raise CampaignError(f"invalid case at line {line_number}: {line!r}")
        if line in seen:
            raise CampaignError(f"duplicate case at line {line_number}: {line}")
        seen.add(line)
        cases.append(line)
    if not cases:
        raise CampaignError("selected caselist is empty")
    return cases


def decode_qpa(payload: bytes) -> str:
    """Decode QPA control text without rejecting raw test-input bytes."""
    return payload.decode("utf-8", errors="surrogateescape")


def validate_inventory_report(
    payload: bytes,
    identity: dict[str, Any],
) -> tuple[int, str, list[str], int]:
    text = payload.decode("utf-8", errors="strict")
    values: dict[str, str] = {}
    modules: list[str] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        key, separator, value = line.partition("=")
        if not separator or not value:
            raise CampaignError(f"invalid CTS inventory row at line {line_number}")
        if key == "registered_module":
            if CASE_NAME.fullmatch(value) is None or "." in value:
                raise CampaignError(f"invalid registered module at line {line_number}")
            modules.append(value)
            continue
        if key not in {
            "schema_version",
            "scope",
            "complete",
            "exported_package_count",
            "complete_caselist_sha256",
            "complete_caselist_case_count",
        }:
            raise CampaignError(f"unknown CTS inventory key: {key}")
        if key in values:
            raise CampaignError(f"duplicate CTS inventory key: {key}")
        values[key] = value
    required = {
        "schema_version",
        "scope",
        "complete",
        "exported_package_count",
        "complete_caselist_sha256",
        "complete_caselist_case_count",
    }
    if set(values) != required:
        raise CampaignError("CTS inventory report is incomplete")
    if values["schema_version"] != "2" or values["complete"] != "1":
        raise CampaignError("CTS inventory report is not complete")
    if values["scope"] != GLES_INVENTORY_SCOPE:
        raise CampaignError("CTS inventory scope is invalid")
    if not values["exported_package_count"].isdigit():
        raise CampaignError("CTS inventory package count is invalid")
    package_count = int(values["exported_package_count"])
    if not values["complete_caselist_case_count"].isdigit():
        raise CampaignError("CTS inventory case count is invalid")
    case_count = int(values["complete_caselist_case_count"])
    digest = values["complete_caselist_sha256"]
    if (
        package_count != len(GLES_INVENTORY_MODULES)
        or case_count < 1
        or SHA256.fullmatch(digest) is None
    ):
        raise CampaignError("CTS inventory identity is invalid")
    if modules != list(GLES_INVENTORY_MODULES):
        raise CampaignError("registered CTS modules do not match the GLES scope")
    frozen_scope = identity["cts"].get("inventory_scope")
    frozen_digest = identity["cts"]["complete_caselist_sha256"]
    frozen_count = identity["cts"]["complete_caselist_case_count"]
    frozen_modules = identity["cts"]["registered_modules"]
    if frozen_scope != GLES_INVENTORY_SCOPE:
        raise CampaignError("frozen CTS inventory scope changed")
    if frozen_digest != digest:
        raise CampaignError("complete CTS caselist identity changed")
    if frozen_count != case_count:
        raise CampaignError("complete CTS caselist count changed")
    if frozen_modules != modules:
        raise CampaignError("registered CTS module set changed")
    return case_count, digest, modules, package_count


def parse_qpa(text: str) -> dict[str, str]:
    prefix = scan_qpa(text)
    if prefix.in_flight_case is not None:
        raise CampaignError(f"QPA is truncated during case {prefix.in_flight_case}")
    if not prefix.ended_session:
        raise CampaignError("QPA has no complete session trailer")
    return prefix.results


def scan_qpa(text: str) -> QpaPrefix:
    """Parse the QPA a guest transported, which may stop inside its last case.

    A guest killed while a case is running transports the bytes it had
    already written.  Every complete case block before that point is real
    evidence; the unterminated trailer is reported separately and never
    counted as a result.

    A guest whose CTS process ended its session early continues the rest of
    the same ordered caselist in a fresh process, so the transported text is
    a sequence of complete CTS session documents.  They are read as one
    ordered result stream: every case still appears exactly once, and the
    trailer that matters is the last document's.
    """
    started = False
    results: dict[str, str] = {}
    current_case: str | None = None
    body: list[str] = []
    ended_session = False
    for raw_line in text.splitlines(keepends=True):
        line = raw_line.strip()
        if line == "#beginSession":
            if current_case is not None:
                raise CampaignError(f"QPA session start inside case {current_case}")
            started = True
            ended_session = False
            continue
        if line.startswith("#beginTestCaseResult "):
            if not started:
                raise CampaignError("QPA case precedes its session start")
            if current_case is not None or ended_session:
                raise CampaignError("nested or post-session QPA case")
            current_case = line.removeprefix("#beginTestCaseResult ")
            if CASE_NAME.fullmatch(current_case) is None:
                raise CampaignError(f"invalid QPA case name: {current_case!r}")
            if current_case in results:
                raise CampaignError(f"duplicate QPA case: {current_case}")
            body = []
            continue
        if line == "#endTestCaseResult":
            if current_case is None:
                raise CampaignError("QPA case trailer has no case")
            payload = "".join(body)
            statuses = RESULT_STATUS.findall(payload)
            if len(statuses) != 1:
                raise CampaignError(
                    f"QPA case {current_case} has {len(statuses)} result statuses"
                )
            case_paths = RESULT_CASE.findall(payload)
            if case_paths and (len(case_paths) != 1 or case_paths[0] != current_case):
                raise CampaignError(f"QPA case-path mismatch for {current_case}")
            results[current_case] = statuses[0]
            current_case = None
            body = []
            continue
        if line.startswith("#terminateTestCaseResult "):
            if current_case is None:
                raise CampaignError("QPA termination has no case")
            status = line.removeprefix("#terminateTestCaseResult ")
            if not status or not re.fullmatch(r"[A-Za-z]+", status):
                raise CampaignError(f"invalid QPA termination status: {status!r}")
            results[current_case] = status
            current_case = None
            body = []
            continue
        if line == "#endSession":
            if current_case is not None:
                raise CampaignError(f"QPA ended during case {current_case}")
            if not started:
                raise CampaignError("QPA trailer precedes its session start")
            ended_session = True
            continue
        if current_case is not None and not line.startswith(QPA_BOUNDARY):
            body.append(raw_line)

    if not started:
        raise CampaignError("QPA does not contain a session start")
    return QpaPrefix(results, current_case, ended_session)


def qpa_session_split(text: str) -> tuple[str, str]:
    """Return a guest's QPA prologue and its complete case blocks.

    The prologue is everything through the first ``#beginSession``.  Each
    document contributes the case blocks up to its last terminated case, so
    a truncated trailer never reaches a merged transcript, and a guest that
    continued its caselist in a fresh CTS process contributes every document
    it transported under one prologue.
    """
    lines = text.splitlines(keepends=True)
    prologue: int | None = None
    start: int | None = None
    end = 0
    kept = 0
    parts: list[str] = []
    for index, raw_line in enumerate(lines):
        line = raw_line.strip()
        if line == "#beginSession":
            if start is not None:
                parts.append("".join(lines[start:end]))
                parts.append(
                    f"{QPA_BOUNDARY}CTS session continues at index {kept}\n"
                )
            if prologue is None:
                prologue = index + 1
            start = end = index + 1
            continue
        if start is None:
            continue
        if line == "#endTestCaseResult" or line.startswith(
            "#terminateTestCaseResult "
        ):
            end = index + 1
            kept += 1
    if prologue is None or start is None:
        raise CampaignError("QPA does not contain a session start")
    parts.append("".join(lines[start:end]))
    return "".join(lines[:prologue]), "".join(parts)


def merge_qpa_sessions(sessions: Sequence["SessionEvidence"]) -> str:
    """Concatenate per-session QPA prefixes into one auditable transcript.

    A single session is returned byte for byte.  Otherwise the first
    session's header is kept and each session contributes only its complete
    case blocks: an aborted session ends at its last transported result and
    the next session re-runs from there, so the transcript stays a gapless
    record of what guests actually executed.
    """
    if not sessions:
        raise CampaignError("QPA merge requires at least one session")
    if [session.index for session in sessions] != list(range(len(sessions))):
        raise CampaignError("QPA merge requires gapless ordered sessions")
    if len(sessions) == 1:
        return sessions[0].qpa_text
    # A guest can be aborted before it checkpoints anything, so the release
    # header comes from the earliest session that transported a QPA at all.
    prologue = next(
        (
            qpa_session_split(session.qpa_text)[0]
            for session in sessions
            if session.qpa_text
        ),
        None,
    )
    if prologue is None:
        raise CampaignError("no shard session transported a QPA")
    parts = [prologue]
    for session in sessions:
        if session.index:
            parts.append(
                f"{QPA_BOUNDARY}session {session.index} "
                f"resumes at index {session.resume_after}\n"
            )
        if session.qpa_text:
            parts.append(qpa_session_split(session.qpa_text)[1])
    parts.append("#endSession\n")
    return "".join(parts)


def validate_qpa_union(qpa: str, selected: Sequence[str]) -> dict[str, str]:
    expected = set(selected)
    if len(expected) != len(selected):
        raise CampaignError("expected caselist contains duplicates")
    results = parse_qpa(qpa)
    actual = set(results)
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing or unexpected:
        details: list[str] = []
        if missing:
            details.append(f"missing={missing[:5]!r}")
        if unexpected:
            details.append(f"unexpected={unexpected[:5]!r}")
        raise CampaignError("incomplete QPA case union: " + ", ".join(details))
    if list(results) != list(selected):
        raise CampaignError("QPA case order does not match the selected caselist")
    return results


def qpa_case_xml(qpa: str, case: str) -> ElementTree.Element:
    begin = f"#beginTestCaseResult {case}"
    end = "#endTestCaseResult"
    lines = qpa.splitlines()
    starts = [index for index, line in enumerate(lines) if line == begin]
    if len(starts) != 1:
        raise CampaignError(f"QPA does not contain one XML payload for {case}")
    start = starts[0] + 1
    try:
        finish = lines.index(end, start)
    except ValueError as error:
        raise CampaignError(f"QPA XML payload is truncated for {case}") from error
    payload = "\n".join(lines[start:finish]).strip()
    if not payload:
        raise CampaignError(f"QPA XML payload is empty for {case}")
    try:
        root = ElementTree.fromstring(payload)
    except ElementTree.ParseError as error:
        raise CampaignError(f"QPA XML payload is invalid for {case}") from error
    if root.tag != "TestCaseResult" or root.attrib != {
        "CasePath": case,
        "Version": "0.3.4",
        "CaseType": "SelfValidate",
    }:
        raise CampaignError(f"QPA XML payload names another case for {case}")
    return root


def egl_config_digest(configs: Sequence[EglConfig]) -> str:
    payload = json.dumps(
        [
            {
                "id": config.identifier,
                "surface_types": list(config.surface_types),
            }
            for config in configs
        ],
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def egl_attribute_digest(attributes: Sequence[dict[str, Any]]) -> str:
    payload = json.dumps(
        list(attributes), sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def parse_egl_config_attributes(qpa: str) -> list[dict[str, Any]]:
    """The EGL attribute table dEQP-EGL.info.configs logs, in EGL order.

    ConfigList::aospConfigs keeps the configurations that render and conform
    to the context API, with their colour, depth, stencil, sample and surface
    attributes. Those are exactly the attributes this case already reports,
    so the run plan reconstructs the list from them rather than approximating
    it.
    """
    root = qpa_case_xml(qpa, EGL_ATTRIBUTE_CASE)
    sets = [element for element in root.findall("EglConfigSet")
            if element.attrib.get("Name") == "EGL-configs"]
    if len(sets) != 1:
        raise CampaignError("EGL attribute QPA has no unique config set")
    integers = {"id": "ConfigID", "red": "RedSize", "green": "GreenSize",
                "blue": "BlueSize", "alpha": "AlphaSize", "depth": "DepthSize",
                "stencil": "StencilSize", "samples": "Samples"}
    attributes: list[dict[str, Any]] = []
    seen: set[int] = set()
    for element in sets[0]:
        if element.tag != "EglConfig" or list(element):
            raise CampaignError("EGL attribute QPA has an invalid config row")
        row: dict[str, Any] = {}
        for key, name in integers.items():
            value = element.attrib.get(name)
            if value is None or re.fullmatch(r"(0|[1-9][0-9]{0,6})", value) is None:
                raise CampaignError(
                    f"EGL attribute QPA has an invalid {name} attribute"
                )
            row[key] = int(value)
        if row["id"] < 1 or row["id"] in seen:
            raise CampaignError("EGL attribute QPA has a duplicate config ID")
        seen.add(row["id"])
        for key, name in (("renderable", "RenderableType"),
                          ("conformant", "Conformant")):
            value = element.attrib.get(name)
            if value is None or any(
                re.fullmatch(r"[A-Z0-9_]+", bit) is None
                for bit in value.split("|")
            ):
                raise CampaignError(
                    f"EGL attribute QPA has an invalid {name} attribute"
                )
            row[key] = value.split("|")
        surfaces = element.attrib.get("SurfaceTypes")
        if surfaces is None or any(
            re.fullmatch(r"[A-Z0-9_]+", bit) is None
            for bit in surfaces.split("|")
        ):
            raise CampaignError(
                "EGL attribute QPA has an invalid SurfaceTypes attribute"
            )
        bits = set(surfaces.split("|"))
        row["surface_types"] = [
            name for bit, name in gles_cts_plan.SURFACE_BITS if bit in bits
        ]
        if set(row) != gles_cts_plan.ATTRIBUTE_KEYS:
            raise CampaignError("EGL attribute QPA row is incomplete")
        attributes.append(row)
    if not attributes:
        raise CampaignError("EGL attribute QPA enumerated no configs")
    return attributes


def parse_egl_config_qpa(qpa: str, api: str) -> tuple[EglConfig, ...]:
    case = CONFIG_CASES.get(api)
    if case is None:
        raise CampaignError(f"unsupported EGL config inventory API: {api}")
    results = parse_qpa(qpa)
    if set(results) != set(CONFIG_INVENTORY_SCOPES[api]):
        raise CampaignError(f"config inventory QPA is not the {api} scope")
    validate_required_passes(results)
    root = qpa_case_xml(qpa, case)
    sections = [
        element
        for element in root.findall("Section")
        if element.attrib.get("Name") == "Configs"
    ]
    if len(sections) != 1:
        raise CampaignError("CTS config QPA has no unique Configs section")
    section = sections[0]
    if section.attrib != {"Name": "Configs", "Description": "Configs"}:
        raise CampaignError("CTS config QPA has an invalid Configs section")
    configs: list[EglConfig] = []
    seen: set[int] = set()
    for element in section:
        if element.tag != "Text" or element.attrib or element.text is None:
            raise CampaignError("CTS config QPA has an invalid config row")
        match = EGL_CONFIG_MESSAGE.fullmatch(element.text)
        if match is None:
            raise CampaignError(
                f"CTS config QPA has an unsupported row: {element.text!r}"
            )
        identifier = int(match.group(1))
        surface_types = tuple(match.group(2).split(", "))
        if (
            identifier in seen
            or len(surface_types) != len(set(surface_types))
        ):
            raise CampaignError("CTS config QPA has a duplicate config entry")
        seen.add(identifier)
        config = EglConfig(identifier, surface_types)
        config.selected_surface_type
        configs.append(config)
    if not configs:
        raise CampaignError("CTS config QPA enumerated no usable configs")
    return tuple(configs)


def status_totals(results: dict[str, str]) -> dict[str, int]:
    totals: dict[str, int] = {}
    for status in results.values():
        totals[status] = totals.get(status, 0) + 1
    return totals


def validate_required_passes(results: dict[str, str]) -> None:
    non_pass = {
        case: status for case, status in results.items() if status != "Pass"
    }
    if non_pass:
        first_case = sorted(non_pass)[0]
        raise CampaignError(
            "required CTS case did not pass: "
            f"{first_case}={non_pass[first_case]}"
        )


def validate_official_runner_results(
    results: dict[str, str],
    successful_statuses: Sequence[str],
    waived: Collection[str] = (),
) -> None:
    """Accept only the runner's success statuses, plus ``Waiver`` for the
    exact cases a granted waiver names; a waived case that still fails, and a
    waived status on any other case, are rejected."""
    accepted = set(successful_statuses)
    waived_set = set(waived)
    nonconformant = {
        case: status
        for case, status in results.items()
        if status not in accepted
        and not (status == WAIVED_STATUS and case in waived_set)
    }
    if nonconformant:
        first_case = sorted(nonconformant)[0]
        raise CampaignError(
            "official CTS runner rejects case result: "
            f"{first_case}={nonconformant[first_case]}"
        )


NATIVE_X11_MARKERS = (
    "x11_server=xorg", "x11_dri3=1", "x11_present=1", "x11_glamor=1",
    "x11_connected=1", "x11_drm_fd=1", "x11_blanking=off",
    "device=/dev/dri/card0", "device=/dev/dri/renderD128",
)


def validate_native_x11_probe(text: str) -> None:
    """Require evidence of native accelerated Xorg, never a substitute server."""
    if re.search(r"xwayland|xvfb", text, re.IGNORECASE):
        raise CampaignError("software or forbidden renderer: Xwayland/Xvfb")
    lines = set(text.splitlines())
    for marker in NATIVE_X11_MARKERS:
        if marker not in lines:
            raise CampaignError(f"native Xorg renderer probe omits {marker}")


def validate_renderer_probe(
    text: str,
    renderer_backend: str = "virgl",
    host_provider: str = "zink-kosmickrisp",
) -> None:
    lower = text.lower()
    validate_native_x11_probe(text)
    forbidden = SOFTWARE_RENDERERS.search(text)
    if forbidden is not None:
        raise CampaignError(f"software or forbidden renderer: {forbidden.group(0)}")
    client_renderers = "\n".join(
        line for line in lower.splitlines()
        if line.startswith("opengl renderer string:")
    )
    if renderer_backend == "virgl":
        if "virgl" not in client_renderers:
            raise CampaignError("renderer probe did not report Virgl")
        # The guest renderer string carries the host GL renderer in
        # parentheses, so a Mesa host provider is visible to the guest.
        if host_provider == "zink-kosmickrisp" and (
            "zink" not in client_renderers or "kosmickrisp" not in client_renderers
        ):
            raise CampaignError(
                "renderer probe did not report Virgl over Zink/KosmicKrisp"
            )
    elif renderer_backend == "zink":
        if "zink" not in client_renderers or "virtio-gpu venus" not in client_renderers:
            raise CampaignError(
                "renderer probe did not report Zink over Virtio-GPU Venus"
            )
    else:
        raise CampaignError(f"unsupported renderer backend: {renderer_backend}")
    if f"guest_page_size={gpu_guest.GUEST_PAGE_SIZE}" not in text:
        raise CampaignError(
            f"renderer probe did not prove {gpu_guest.GUEST_PAGE_SIZE}-byte "
            "guest pages"
        )


def host_backend_label(renderer: str) -> str:
    """Name the host stack a run renders on, for run metadata and evidence."""
    return "venus-kosmickrisp" if renderer == "zink" else "zink-kosmickrisp"


def validate_host_backend(transcript: str) -> None:
    for marker in RETIRED_BACKEND_MARKERS:
        if marker in transcript:
            raise CampaignError(f"retired host GL provider was selected: {marker}")
    if HOST_BACKEND_MARKER not in transcript:
        raise CampaignError("Zink/KosmicKrisp host-provider marker is missing")


def report_marker(line: str, prefix: str) -> str | None:
    offset = line.find(prefix)
    if offset < 0:
        return None
    return line[offset:].strip()


def decode_report_text(value: str, field: str) -> str:
    if len(value) % 2 != 0:
        raise CampaignError(f"host capability {field} has odd-length hex")
    try:
        decoded = bytes.fromhex(value).decode("utf-8", errors="strict")
    except (ValueError, UnicodeDecodeError) as error:
        raise CampaignError(
            f"host capability {field} is not valid UTF-8 hex"
        ) from error
    if "\x00" in decoded or "\n" in decoded or "\r" in decoded:
        raise CampaignError(f"host capability {field} contains control data")
    return decoded


def parse_gpu_capability_report(transcript: str) -> dict[str, Any]:
    """Decode the host capability report the emulator prints at start-up.

    The host also reports the Venus capset. It is not a Virgl capset and has
    no Virgl layout, so it is recorded by identity alone and kept out of
    `virgl_capsets`. A transcript from a host that published Venus only on
    request has none, so the report carries it only when present.
    """
    contexts: dict[tuple[int, int], dict[str, Any]] = {}
    context_extensions: dict[tuple[int, int], list[str]] = {}
    capsets: dict[int, dict[str, Any]] = {}
    for raw_line in transcript.replace("\r", "").splitlines():
        marker = report_marker(raw_line, "UML_GPU_HOST_")
        if marker is None:
            marker = report_marker(raw_line, "UML_GPU_CAPSET_")
        if marker is None:
            continue
        match = HOST_CONTEXT.fullmatch(marker)
        if match is not None:
            (
                major_text,
                minor_text,
                created_text,
                error_text,
                actual_major_text,
                actual_minor_text,
                vendor_hex,
                renderer_hex,
                version_hex,
                glsl_hex,
                extension_count_text,
            ) = match.groups()
            requested = (int(major_text), int(minor_text))
            if requested in contexts:
                raise CampaignError(
                    f"duplicate host-context probe: {requested[0]}.{requested[1]}"
                )
            contexts[requested] = {
                "requested": list(requested),
                "created": created_text == "1",
                "egl_error": int(error_text),
                "actual": [int(actual_major_text), int(actual_minor_text)],
                "vendor": decode_report_text(vendor_hex, "vendor"),
                "renderer": decode_report_text(renderer_hex, "renderer"),
                "version": decode_report_text(version_hex, "version"),
                "shading_language_version": decode_report_text(
                    glsl_hex, "shading-language version"
                ),
                "extension_count": int(extension_count_text),
            }
            context_extensions[requested] = []
            continue
        match = HOST_EXTENSION.fullmatch(marker)
        if match is not None:
            major_text, minor_text, extension_hex = match.groups()
            requested = (int(major_text), int(minor_text))
            if requested not in contexts:
                raise CampaignError("host extension preceded its context probe")
            context_extensions[requested].append(
                decode_report_text(extension_hex, "extension")
            )
            continue
        match = CAPSET_BEGIN.fullmatch(marker)
        if match is not None:
            identifier_text, version_text, size_text, chunks_text = match.groups()
            identifier = int(identifier_text)
            if identifier in capsets:
                raise CampaignError(f"duplicate Virgl capset start: {identifier}")
            size = int(size_text)
            chunks = int(chunks_text)
            if size < 1 or size > 64 * 1024 or chunks < 1 or chunks > 256:
                raise CampaignError(f"invalid Virgl capset bounds: {identifier}")
            capsets[identifier] = {
                "maximum_version": int(version_text),
                "size": size,
                "expected_chunks": chunks,
                "chunks": {},
                "ended": False,
            }
            continue
        match = CAPSET_CHUNK.fullmatch(marker)
        if match is not None:
            identifier_text, index_text, payload = match.groups()
            identifier = int(identifier_text)
            if identifier not in capsets:
                raise CampaignError("Virgl capset chunk preceded its header")
            index = int(index_text)
            chunks = capsets[identifier]["chunks"]
            if index in chunks:
                raise CampaignError(
                    f"duplicate Virgl capset chunk: {identifier}[{index}]"
                )
            if len(payload) % 2 != 0:
                raise CampaignError("Virgl capset chunk has odd-length hex")
            try:
                chunks[index] = bytes.fromhex(payload)
            except ValueError as error:
                raise CampaignError("Virgl capset chunk has invalid hex") from error
            continue
        match = CAPSET_END.fullmatch(marker)
        if match is not None:
            identifier_text, chunks_text = match.groups()
            identifier = int(identifier_text)
            if identifier not in capsets or capsets[identifier]["ended"]:
                raise CampaignError("Virgl capset trailer has no unique header")
            if int(chunks_text) != capsets[identifier]["expected_chunks"]:
                raise CampaignError("Virgl capset trailer count changed")
            capsets[identifier]["ended"] = True
            continue
        raise CampaignError(f"malformed GPU capability marker: {marker[:120]}")

    if set(contexts) != {(4, 6)}:
        raise CampaignError(
            "host capability report omitted the desktop GL 4.6 core request"
        )
    serialized_contexts: list[dict[str, Any]] = []
    for requested in sorted(contexts):
        context = contexts[requested]
        extensions = context_extensions[requested]
        if extensions != sorted(set(extensions)):
            raise CampaignError("host extension report is not sorted and unique")
        if len(extensions) != context.pop("extension_count"):
            raise CampaignError("host extension count changed during capture")
        context["extensions"] = extensions
        if context["created"]:
            if context["egl_error"] != 0x3000:
                raise CampaignError("successful host context has an EGL error")
            if tuple(context["actual"]) < requested:
                raise CampaignError("host context is below its requested GL version")
            for field in (
                "vendor",
                "renderer",
                "version",
                "shading_language_version",
            ):
                if not context[field]:
                    raise CampaignError(
                        f"successful host context omitted its {field}"
                    )
        elif (
            context["egl_error"] == 0x3000
            or context["actual"] != [0, 0]
            or extensions
        ):
            raise CampaignError("failed host context has contradictory evidence")
        serialized_contexts.append(context)

    if not {1, 2} <= set(capsets) <= {1, 2, VENUS_CAPSET_ID}:
        raise CampaignError("GPU capability report omitted a Virgl capset")
    serialized_capsets: list[dict[str, Any]] = []
    serialized_venus: dict[str, Any] | None = None
    for identifier in sorted(capsets):
        capset = capsets[identifier]
        expected_chunks = capset["expected_chunks"]
        if not capset["ended"] or set(capset["chunks"]) != set(
            range(expected_chunks)
        ):
            raise CampaignError(f"Virgl capset {identifier} is incomplete")
        payload = b"".join(
            capset["chunks"][index] for index in range(expected_chunks)
        )
        if len(payload) != capset["size"]:
            raise CampaignError(f"Virgl capset {identifier} size changed")
        row: dict[str, Any] = {
            "id": identifier,
            "maximum_version": capset["maximum_version"],
            "size": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "data_hex": payload.hex(),
        }
        if identifier == VENUS_CAPSET_ID:
            serialized_venus = row
            continue
        try:
            row["decoded"] = virgl_capset.decode_capset(identifier, payload)
        except ValueError as error:
            raise CampaignError(str(error)) from error
        serialized_capsets.append(row)
    report: dict[str, Any] = {
        "host_contexts": serialized_contexts,
        "virgl_capsets": serialized_capsets,
    }
    if serialized_venus is not None:
        report["venus_capset"] = serialized_venus
    return report


def parse_native_display_final(transcript: str) -> dict[str, int]:
    if "Native display: direct Metal texture presentation" not in transcript:
        raise CampaignError("native display never accepted direct Metal presentation")
    matches = NATIVE_DISPLAY_FINAL.findall(transcript)
    if len(matches) != 1:
        raise CampaignError("native display did not emit one final frame report")
    guest, presented, dropped, cpu_fallback = (
        int(value) for value in matches[0]
    )
    if guest < 1 or presented < 1:
        raise CampaignError("native display final frame totals are empty")
    if cpu_fallback != 0:
        raise CampaignError("native display used CPU scanout fallback")
    return {
        "guest_frames": guest,
        "presented_frames": presented,
        "dropped_frames": dropped,
        "cpu_fallback_frames": cpu_fallback,
    }


def validate_drm_shutdown(transcript: str) -> None:
    if DIRTY_DRM_SHUTDOWN in transcript:
        raise CampaignError(
            "Virtio-GPU memory manager was not clean during guest shutdown"
        )


def validate_guest_shutdown(transcript: str) -> None:
    validate_drm_shutdown(transcript)
    if DARWIN_CLEAN_STOP not in transcript:
        raise CampaignError("campaign omitted the clean Darwin guest stop marker")


def artifact_marker(line: str) -> str | None:
    offset = line.find("UML_CTS_ARTIFACT_")
    if offset < 0:
        return None
    if offset != 0:
        prefix = line[:offset]
        if not prefix[-1].isspace() or not prefix.rstrip().endswith(":"):
            return None
    return line[offset:].strip()


def artifact_acknowledgement(line: str) -> str:
    begin = BEGIN_ARTIFACT.fullmatch(line)
    if begin is not None:
        return f"UML_CTS_ARTIFACT_ACK|{begin.group(1)}|begin"
    chunk = CHUNK_ARTIFACT.fullmatch(line)
    if chunk is not None:
        return f"UML_CTS_ARTIFACT_ACK|{chunk.group(1)}|{chunk.group(2)}"
    end = END_ARTIFACT.fullmatch(line)
    if end is not None:
        return f"UML_CTS_ARTIFACT_ACK|{end.group(1)}|end"
    raise CampaignError("artifact transport emitted a non-frame line")


def assemble_qpa_segments(decoded: dict[str, bytes]) -> dict[str, bytes]:
    """Join transported QPA checkpoints into the single canonical artifact.

    Segments are byte ranges of the growing guest QPA, emitted in order from
    offset zero.  Callers keep seeing one ``results.qpa``; the split only
    exists so a killed guest still leaves the prefix it had produced.
    """
    segments = sorted(name for name in decoded if QPA_SEGMENT.fullmatch(name))
    if not segments:
        return decoded
    if QPA_ARTIFACT in decoded:
        raise CampaignError(
            "QPA checkpoint segments cannot accompany a whole-file QPA"
        )
    if len(segments) > MAX_QPA_SEGMENTS:
        raise CampaignError("QPA checkpoint segment count exceeds its bound")
    indices = [int(QPA_SEGMENT.fullmatch(name).group(1)) for name in segments]
    if indices != list(range(len(indices))):
        raise CampaignError("QPA checkpoint segments are not a gapless prefix")
    payload = b"".join(decoded[name] for name in segments)
    if len(payload) > MAX_UNCOMPRESSED_ARTIFACT:
        raise CampaignError("assembled QPA exceeds the output bound")
    assembled = {
        name: value for name, value in decoded.items() if name not in set(segments)
    }
    assembled[QPA_ARTIFACT] = payload
    return assembled


def decode_artifacts(
    transcript: str,
    *,
    partial: bool = False,
    rejected: list[str] | None = None,
) -> dict[str, bytes]:
    """Decode the acknowledged artifact frames a transcript carries.

    ``partial`` is the salvage lane, for a guest that died mid-transfer: it
    keeps every complete artifact and drops the unfinished trailer.  A frame
    group can also be complete and still carry nothing -- a guest whose
    filesystem filled framed a zero-byte artifact with the empty string's
    digest -- which is never a valid ``gzip+base64`` payload.  Strict decoding
    refuses the transcript; the salvage lane drops that artifact and names it
    in ``rejected``, so a caller reports the loss instead of inferring it from
    a short result set.
    """

    encoded: dict[str, EncodedArtifact] = {}
    dropped: list[str] = []
    for raw_line in transcript.replace("\r", "").splitlines():
        marker = artifact_marker(raw_line)
        if marker is None:
            continue
        begin = BEGIN_ARTIFACT.fullmatch(marker)
        if begin is not None:
            name, size_text, digest = begin.groups()
            if name not in ALLOWED_ARTIFACTS and QPA_SEGMENT.fullmatch(name) is None:
                raise CampaignError(f"unsafe or unexpected artifact name: {name}")
            if name in encoded or name in dropped:
                raise CampaignError(f"duplicate artifact start: {name}")
            size = int(size_text)
            if size < 1 or size > MAX_COMPRESSED_ARTIFACT:
                if not partial:
                    raise CampaignError(
                        f"artifact {name} has invalid compressed size"
                    )
                dropped.append(name)
                continue
            encoded[name] = EncodedArtifact(size, digest)
            continue
        chunk = CHUNK_ARTIFACT.fullmatch(marker)
        if chunk is not None:
            name, index_text, payload = chunk.groups()
            if name in dropped:
                continue
            if name not in encoded:
                raise CampaignError(f"artifact chunk precedes start: {name}")
            index = int(index_text)
            if index in encoded[name].chunks:
                raise CampaignError(f"duplicate artifact chunk: {name}[{index}]")
            encoded[name].chunks[index] = payload
            continue
        end = END_ARTIFACT.fullmatch(marker)
        if end is not None:
            name, count_text = end.groups()
            if name in dropped:
                continue
            if name not in encoded:
                raise CampaignError(f"artifact end precedes start: {name}")
            if encoded[name].expected_chunks is not None:
                raise CampaignError(f"duplicate artifact end: {name}")
            encoded[name].expected_chunks = int(count_text)
            continue
        raise CampaignError(f"malformed artifact marker: {marker[:120]}")

    if rejected is not None:
        rejected.extend(dropped)
    if not encoded and not partial:
        raise CampaignError("campaign emitted no artifacts")
    decoded: dict[str, bytes] = {}
    for name, artifact in encoded.items():
        count = artifact.expected_chunks
        # An interrupted guest can leave its last frame group unfinished. The
        # partial lane keeps every acknowledged, complete artifact and drops
        # only that trailer; ordinary runs still fail closed.
        if partial and (
            count is None or count < 1 or set(artifact.chunks) != set(range(count))
        ):
            if rejected is not None:
                rejected.append(name)
            continue
        if count is None or count < 1:
            raise CampaignError(f"artifact {name} is missing its trailer")
        if set(artifact.chunks) != set(range(count)):
            raise CampaignError(f"artifact {name} has an incomplete chunk union")
        encoded_size = sum(len(chunk) for chunk in artifact.chunks.values())
        if encoded_size > ((MAX_COMPRESSED_ARTIFACT + 2) // 3) * 4:
            raise CampaignError(f"artifact {name} exceeds the encoded-data bound")
        try:
            compressed = b"".join(
                base64.b64decode(artifact.chunks[index], validate=True)
                for index in range(count)
            )
        except (ValueError, binascii.Error) as error:
            raise CampaignError(f"artifact {name} has invalid base64") from error
        if len(compressed) != artifact.compressed_size:
            raise CampaignError(f"artifact {name} compressed size mismatch")
        if hashlib.sha256(compressed).hexdigest() != artifact.sha256:
            raise CampaignError(f"artifact {name} checksum mismatch")
        try:
            with gzip.GzipFile(fileobj=io.BytesIO(compressed)) as stream:
                payload = stream.read(MAX_UNCOMPRESSED_ARTIFACT + 1)
        except (OSError, EOFError) as error:
            raise CampaignError(f"artifact {name} is not complete gzip data") from error
        if len(payload) > MAX_UNCOMPRESSED_ARTIFACT:
            raise CampaignError(f"artifact {name} exceeds the output bound")
        decoded[name] = payload
    return assemble_qpa_segments(decoded)


def write_artifacts(artifacts: dict[str, bytes], directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=False)
    root = directory.resolve()
    for name, payload in artifacts.items():
        if name not in ALLOWED_ARTIFACTS:
            raise CampaignError(f"refusing unsafe artifact name: {name}")
        destination = (root / name).resolve()
        if destination.parent != root:
            raise CampaignError(f"artifact escapes output directory: {name}")
        destination.write_bytes(payload)


def artifact_transport_program(
    *,
    error_prefix: str,
    base64_line_length: int,
    missing_source_status: int,
    console_transport: bool = False,
) -> str:
    """The guest side of the acknowledged artifact transport.

    Every launcher shares this text, because the defect it closes lived in two
    independent copies of it: nothing checked whether ``gzip`` or ``base64``
    had actually produced anything.  A device guest whose tmpfs filled while a
    QPA checkpoint was being written therefore framed a zero-byte artifact --
    a valid-looking frame group carrying the empty string's digest -- reported
    success, and advanced past 17 MB of results that were never transported.

    So each producer's status is checked, the compressed stream is measured
    back through the decompressor against the byte count it was meant to hold,
    and a failure leaves nothing behind: the caller keeps its offset and the
    bytes go out in the next attempt.  ``missing_source_status`` is what a
    caller wants for a source file that does not exist, which differs between
    launchers.

    ``artifact_framed`` says whether the failure happened with a frame group
    already on the wire.  Only a failure before the first frame may be
    retried: re-sending a name the host has already opened is a duplicate
    start, which fails its decode outright, salvage lane included.

    ``prepare_channel`` opens the virtio-serial ``channel`` as descriptor 3
    and ``emit_frame`` sends one frame through it, failing unless the host
    acknowledges exactly that frame.  The caller defines ``output``,
    ``channel``, ``channel_ready=0`` and ``artifact_framed=0``.  With
    ``console_transport`` it also defines ``artifact_transport``, and a
    console launcher prints its frames without acknowledgement.
    """

    console_open = (
        '    [ "$artifact_transport" = virtio-serial ] || return 0\n'
        if console_transport else ""
    )
    console_frame = (
        '    if [ "$artifact_transport" = console ]; then\n'
        "        printf '%s\\n' \"$frame\"\n"
        "        return 0\n"
        "    fi\n"
        if console_transport else ""
    )
    return f"""prepare_channel()
{{
{console_open}    [ "$channel_ready" -eq 0 ] || return 0
    remaining=60
    while [ ! -c "$channel" ]; do
        [ "$remaining" -gt 0 ] || return 1
        remaining=$((remaining - 1))
        sleep 1
    done
    exec 3<> "$channel" || return 1
    channel_ready=1
}}

emit_frame()
{{
    frame="$1"
    expected_ack="$2"
{console_frame}    [ "$channel_ready" -eq 1 ] || return 1
    printf '%s\\n' "$frame" >&3 || return 1
    actual_ack=$(timeout --signal=TERM 30 head -n 1 <&3) || return 1
    [ "$actual_ack" = "$expected_ack" ] || {{
        echo '{error_prefix} artifact acknowledgement changed'
        return 1
    }}
}}

artifact_discard()
{{
    rm -f "$output/$1.gz" "$output/$1.gz.b64"
}}

compress_artifact()
{{
    if ! gzip -c > "$output/$1.gz"; then
        printf '{error_prefix} artifact %s could not be compressed\\n' "$1"
        artifact_discard "$1"
        return 1
    fi
    artifact_bytes=$(gzip -dc "$output/$1.gz" | wc -c | tr -d ' ')
    if [ "$artifact_bytes" -ne "$2" ]; then
        printf '{error_prefix} artifact %s holds %s of %s bytes\\n' \\
            "$1" "$artifact_bytes" "$2"
        artifact_discard "$1"
        return 1
    fi
}}

emit_compressed()
{{
    name="$1"
    compressed="$output/$name.gz"
    encoded="$compressed.b64"
    size=$(wc -c < "$compressed" | tr -d ' ')
    if [ "$size" -lt 1 ]; then
        printf '{error_prefix} artifact %s compressed to nothing\\n' "$name"
        artifact_discard "$name"
        return 1
    fi
    if ! base64 -w {base64_line_length} "$compressed" > "$encoded"; then
        printf '{error_prefix} artifact %s could not be encoded\\n' "$name"
        artifact_discard "$name"
        return 1
    fi
    digest=$(sha256sum "$compressed" | cut -d ' ' -f 1)
    chunks=$(wc -l < "$encoded" | tr -d ' ')
    frame=$(printf 'UML_CTS_ARTIFACT_BEGIN|%s|gzip+base64|%s|%s' \\
        "$name" "$size" "$digest")
    artifact_framed=1
    emit_frame "$frame" "UML_CTS_ARTIFACT_ACK|$name|begin" || {{
        artifact_discard "$name"
        return 1
    }}
    index=0
    while IFS= read -r chunk; do
        frame=$(printf 'UML_CTS_ARTIFACT_CHUNK|%s|%s|%s' \\
            "$name" "$index" "$chunk")
        emit_frame "$frame" "UML_CTS_ARTIFACT_ACK|$name|$index" || {{
            artifact_discard "$name"
            return 1
        }}
        index=$((index + 1))
    done < "$encoded"
    frame=$(printf 'UML_CTS_ARTIFACT_END|%s|%s' "$name" "$chunks")
    emit_frame "$frame" "UML_CTS_ARTIFACT_ACK|$name|end" || {{
        artifact_discard "$name"
        return 1
    }}
    artifact_framed=0
    artifact_discard "$name"
}}

emit_artifact()
{{
    name="$1"
    source="$2"
    [ -f "$source" ] || return {missing_source_status}
    source_bytes=$(wc -c < "$source" | tr -d ' ')
    compress_artifact "$name" "$source_bytes" < "$source" || return 1
    emit_compressed "$name"
}}
"""


def qpa_checkpoint_program() -> str:
    """Transport the QPA bytes a previous checkpoint did not cover.

    The range is compressed straight out of the growing file.  Copying it to a
    temporary first needs as much free space as the range itself, which is
    what exhausted a device guest's tmpfs mid-run and cost that fraction both
    its last results and its summary.

    ``qpa_offset`` only advances once the segment is acknowledged, so a
    checkpoint that could not be produced or sent leaves its bytes pending for
    the next attempt instead of dropping them.
    """

    return """flush_qpa()
{
    force="$1"
    [ -f "$qpa" ] || return 0
    qpa_size=$(wc -c < "$qpa" | tr -d ' ')
    qpa_delta=$((qpa_size - qpa_offset))
    if [ "$qpa_delta" -le 0 ]; then
        if [ "$force" -ne 1 ] || [ "$qpa_segments" -ne 0 ]; then
            return 0
        fi
        qpa_delta=0
    fi
    segment=$(printf 'results.qpa.%04d' "$qpa_segments")
    if [ "$qpa_delta" -eq 0 ]; then
        : | compress_artifact "$segment" 0 || return 1
    else
        tail -c +$((qpa_offset + 1)) "$qpa" | head -c "$qpa_delta" | \\
            compress_artifact "$segment" "$qpa_delta" || return 1
    fi
    emit_compressed "$segment" || return 1
    qpa_offset=$qpa_size
    qpa_segments=$((qpa_segments + 1))
}
"""


def native_x11_setup(*, wayland: bool = False) -> str:
    """Start authenticated, DRM-backed Xorg, optionally with nested Wayland."""
    setup = r'''xorg_pid=
xorg_stamp=
xorg_launcher_pid=
xorg_launcher_stamp=
kwin_pid=
kwin_stamp=
kwin_launcher_pid=
kwin_launcher_stamp=

native_process_state()
{
    [ -r "/proc/$1/stat" ] || return 0
    sed 's/^.*) //' "/proc/$1/stat" 2>/dev/null | awk '{print $1 ":" $20}'
}

native_process_alive()
{
    [ -n "$1" ] && [ -n "$2" ] || return 1
    process_state=$(native_process_state "$1")
    case "$process_state" in
        Z:*|X:*|"") return 1 ;;
    esac
    [ "${process_state#*:}" = "$2" ]
}

native_stop_process()
{
    stop_pid=$1
    stop_stamp=$2
    stop_name=$3
    native_process_alive "$stop_pid" "$stop_stamp" || return 0
    stop_executable=
    case "$stop_name" in
        kwin) stop_executable=/usr/bin/kwin_wayland ;;
        xorg) stop_executable=/usr/lib/xorg/Xorg ;;
    esac
    if [ -n "$stop_executable" ] && [ "$(readlink -f /proc/$stop_pid/exe)" != "$stop_executable" ]; then
        printf 'UML_CTS_ERROR: native display process identity changed: %s\n' "$stop_name"
        return 1
    fi
    printf 'UML_NATIVE_DISPLAY_CLEANUP|stop=%s|pid=%s\n' "$stop_name" "$stop_pid"
    kill -TERM "$stop_pid" 2>/dev/null || return 1
    stop_remaining=10
    while native_process_alive "$stop_pid" "$stop_stamp"; do
        [ "$stop_remaining" -gt 0 ] || break
        stop_remaining=$((stop_remaining - 1))
        sleep 1
    done
    native_process_alive "$stop_pid" "$stop_stamp" || return 0
    printf 'UML_CTS_ERROR: native display cleanup timed out: %s\n' "$stop_name"
    kill -KILL "$stop_pid" 2>/dev/null || true
    stop_remaining=5
    while native_process_alive "$stop_pid" "$stop_stamp"; do
        [ "$stop_remaining" -gt 0 ] || break
        stop_remaining=$((stop_remaining - 1))
        sleep 1
    done
    return 1
}

native_display_child()
{
    child_frontier=$1
    child_depth=8
    while [ -n "$child_frontier" ] && [ "$child_depth" -gt 0 ]; do
        child_next=
        for child_parent in $child_frontier; do
            child_list="/proc/$child_parent/task/$child_parent/children"
            [ -r "$child_list" ] || continue
            for child in $(cat "$child_list"); do
                if [ "$(readlink -f /proc/$child/exe)" = "$2" ]; then
                    printf '%s\n' "$child"
                    return 0
                fi
                child_next="$child_next $child"
            done
        done
        child_frontier=$child_next
        child_depth=$((child_depth - 1))
    done
}

native_display_cleanup()
{
    cleanup_status=0
    # A startup failure can precede the readiness probe's PID capture. Only
    # inspect descendants of our still-pinned launcher in that case.
    if [ -z "$kwin_pid" ] && native_process_alive "$kwin_launcher_pid" "$kwin_launcher_stamp"; then
        kwin_pid=$(native_display_child "$kwin_launcher_pid" /usr/bin/kwin_wayland)
        if [ -n "$kwin_pid" ]; then
            kwin_stamp=$(native_process_state "$kwin_pid")
            kwin_stamp=${kwin_stamp#*:}
        fi
    fi
    if [ -z "$xorg_pid" ] && native_process_alive "$xorg_launcher_pid" "$xorg_launcher_stamp"; then
        xorg_pid=$(native_display_child "$xorg_launcher_pid" /usr/lib/xorg/Xorg)
        if [ -n "$xorg_pid" ]; then
            xorg_stamp=$(native_process_state "$xorg_pid")
            xorg_stamp=${xorg_stamp#*:}
        fi
    fi
    native_stop_process "$kwin_pid" "$kwin_stamp" kwin || cleanup_status=1
    # Never tear down the parent display while its compositor is still alive.
    if native_process_alive "$kwin_pid" "$kwin_stamp"; then return 1; fi
    native_stop_process "$kwin_launcher_pid" "$kwin_launcher_stamp" kwin-wrapper || cleanup_status=1
    if ! native_process_alive "$kwin_launcher_pid" "$kwin_launcher_stamp" && [ -n "$kwin_launcher_pid" ]; then
        wait "$kwin_launcher_pid" 2>/dev/null || true
    fi
    native_stop_process "$xorg_pid" "$xorg_stamp" xorg || cleanup_status=1
    if native_process_alive "$xorg_pid" "$xorg_stamp"; then return 1; fi
    native_stop_process "$xorg_launcher_pid" "$xorg_launcher_stamp" xorg-wrapper || cleanup_status=1
    if ! native_process_alive "$xorg_launcher_pid" "$xorg_launcher_stamp" && [ -n "$xorg_launcher_pid" ]; then
        wait "$xorg_launcher_pid" 2>/dev/null || true
    fi
    printf 'UML_NATIVE_DISPLAY_CLEANUP|status=%s\n' "$cleanup_status"
    return "$cleanup_status"
}

native_display="$output/native-display"
install -d -m 0755 "$native_display"
xdisplay=:0
xauthority="$native_display/Xauthority"
runtime="$native_display/runtime"
install -d -m 0700 -o uml -g uml "$runtime"

native_display_failure()
{
    for log in Xorg.log xorg-stderr.txt xdpyinfo.txt xset.txt glxinfo.txt kwin.txt; do
        if [ -f "$native_display/$log" ]; then
            printf 'native_display_log=%s\n' "$log" >> "$renderer"
            tail -n 40 "$native_display/$log" >> "$renderer"
        fi
    done
    printf 'UML_CTS_ERROR: native GPU display initialization failed: %s\n' "${1:-surface checks}"
    exit 1
}

# Display servers always use the image's Virgl path, independently of the
# Zink/Venus environment selected for CTS clients.
virgl_display()
{
    env -u MESA_LOADER_DRIVER_OVERRIDE -u GALLIUM_DRIVER \
        -u VK_DRIVER_FILES -u VK_ICD_FILENAMES -u LIBGL_ALWAYS_SOFTWARE \
        -u EGL_PLATFORM -u WAYLAND_DISPLAY \
        MESA_LOADER_DRIVER_OVERRIDE=virtio_gpu GALLIUM_DRIVER=virgl "$@"
}

if pgrep -x Xwayland >/dev/null || pgrep -x Xvfb >/dev/null; then
    native_display_failure
fi
# The preceding ICD JSON need not have a final newline.
printf '\n' >> "$renderer"
for device in /dev/dri/card0 /dev/dri/renderD128; do
    [ -c "$device" ] || native_display_failure "missing DRM node $device"
    printf 'device=%s\n' "$device" >> "$renderer"
    transport=$(readlink -f "/sys/class/drm/${device##*/}/device/driver")
    printf 'drm_transport=%s:%s\n' "$device" "$transport" >> "$renderer"
    # UML exposes the card and render nodes through its platform transport;
    # the modesetting/glamor and open-card checks below prove the GPU path.
    [ "$transport" = /sys/bus/platform/drivers/virtio-uml ] || \
        native_display_failure "unexpected DRM transport $transport"
done
install -d -m 1777 /tmp/.X11-unix
: > "$xauthority"
xauth -f "$xauthority" add "$xdisplay" MIT-MAGIC-COOKIE-1 "$(mcookie)" || native_display_failure
chown uml:uml "$xauthority"
chmod 0600 "$xauthority"
cat > "$native_display/xorg.conf" <<'XORG_CONFIG'
Section "ServerFlags"
    Option "AutoAddDevices" "false"
    # A CTS guest never receives input, so the screen saver and DPMS always
    # expire. A blanked screen turns the CRTC off, the Present extension then
    # has no CRTC to schedule against and falls back to its 1 Hz fake vblank,
    # and every FIFO presentation - the client's and the nested compositor's
    # alike - costs a second instead of a frame.
    Option "BlankTime" "0"
    Option "StandbyTime" "0"
    Option "SuspendTime" "0"
    Option "OffTime" "0"
EndSection
Section "Device"
    Identifier "Virtio"
    Driver "modesetting"
    Option "kmsdev" "/dev/dri/card0"
    Option "AccelMethod" "glamor"
EndSection
Section "Screen"
    Identifier "Screen"
    Device "Virtio"
    DefaultDepth 24
EndSection
XORG_CONFIG
virgl_display /usr/lib/xorg/Xorg "$xdisplay" vt1 -noreset -nolisten tcp \
    -auth "$xauthority" -config "$native_display/xorg.conf" \
    -logfile "$native_display/Xorg.log" > "$native_display/xorg-stderr.txt" 2>&1 &
xorg_launcher_pid=$!
xorg_launcher_stamp=$(native_process_state "$xorg_launcher_pid")
xorg_launcher_stamp=${xorg_launcher_stamp#*:}
remaining=25
until runuser -u uml -- env DISPLAY="$xdisplay" XAUTHORITY="$xauthority" \
    timeout 1 xdpyinfo > "$native_display/xdpyinfo.txt" 2>&1; do
    kill -0 "$xorg_launcher_pid" 2>/dev/null || native_display_failure
    [ "$remaining" -gt 0 ] || native_display_failure
    remaining=$((remaining - 1))
    sleep 1
done
# The shell function can retain a parent process, so identify the actual
# server by its executable and authenticated display argument.
xorg_pid=
for candidate in $(pgrep -x Xorg); do
    [ "$(readlink -f /proc/$candidate/exe)" = /usr/lib/xorg/Xorg ] || continue
    tr '\000' '\n' < /proc/$candidate/cmdline | grep -Fxq -- "$xauthority" || continue
    xorg_pid=$candidate
    break
done
[ -n "$xorg_pid" ] || native_display_failure
xorg_stamp=$(native_process_state "$xorg_pid")
xorg_stamp=${xorg_stamp#*:}
printf 'x11_server=xorg\nx11_pid=%s\n' "$xorg_pid" >> "$renderer"
# Settle screen blanking from a client as well: the configuration above is
# what the server starts with, and this is what it is still using when the
# first presentation is scheduled. Both must hold, so prove it rather than
# assume it.
runuser -u uml -- env DISPLAY="$xdisplay" XAUTHORITY="$xauthority" \
    timeout 10 xset s off -dpms || native_display_failure "screen blanking"
runuser -u uml -- env DISPLAY="$xdisplay" XAUTHORITY="$xauthority" \
    timeout 10 xset q > "$native_display/xset.txt" 2>&1 || \
    native_display_failure "screen blanking"
grep -Eq '^[[:space:]]*timeout:[[:space:]]+0([[:space:]]|$)' "$native_display/xset.txt" || \
    native_display_failure "screen blanking"
grep -Eq 'DPMS is Disabled' "$native_display/xset.txt" || \
    native_display_failure "screen blanking"
printf 'x11_blanking=off\n' >> "$renderer"
card_open=0
for fd in /proc/$xorg_pid/fd/*; do
    if [ "$(readlink "$fd")" = /dev/dri/card0 ]; then card_open=1; break; fi
done
[ "$card_open" -eq 1 ] || native_display_failure
grep -Eq '^[[:space:]]+DRI3$' "$native_display/xdpyinfo.txt" || native_display_failure
printf 'x11_dri3=1\n' >> "$renderer"
grep -Eq '^[[:space:]]+Present$' "$native_display/xdpyinfo.txt" || native_display_failure
printf 'x11_present=1\n' >> "$renderer"
grep -Eq 'modeset\(0\): glamor X acceleration enabled on virgl' "$native_display/Xorg.log" || native_display_failure
printf 'x11_glamor=1\n' >> "$renderer"
runuser -u uml -- env DISPLAY="$xdisplay" XAUTHORITY="$xauthority" \
    timeout 10 xrandr --query > "$native_display/xrandr.txt" || native_display_failure
grep -Eq ' connected( primary)? [0-9]+x[0-9]+\+' "$native_display/xrandr.txt" || native_display_failure
printf 'x11_connected=1\nx11_drm_fd=1\n' >> "$renderer"
virgl_display runuser -u uml -- env DISPLAY="$xdisplay" XAUTHORITY="$xauthority" \
    XDG_RUNTIME_DIR="$runtime" timeout 10 glxinfo -B > "$native_display/glxinfo.txt" 2>&1 || native_display_failure
grep -Eiq 'OpenGL renderer string: virgl' "$native_display/glxinfo.txt" || native_display_failure
if grep -Eiq 'llvmpipe|softpipe|swrast|software rasterizer' "$native_display/glxinfo.txt"; then
    native_display_failure
fi
printf 'x11_display_renderer=virgl\n' >> "$renderer"
'''
    if wayland:
        # The compositor's own idle locker has the same effect on Wayland that
        # screen blanking has on X11: the lock screen occludes every client, a
        # surface the compositor is not painting is owed no frame callback, and
        # a FIFO presentation then waits out its whole bound per frame. A CTS
        # guest never produces the input that would postpone the lock, so
        # refuse lock screen support outright and settle the autolock
        # configuration the compositor would otherwise inherit.
        setup += r'''install -d -m 0755 -o uml -g uml /home/uml/.config
cat > /home/uml/.config/kscreenlockerrc <<'KSCREENLOCKER_CONFIG'
[Daemon]
Autolock=false
LockOnResume=false
LockGrace=0
Timeout=0
KSCREENLOCKER_CONFIG
chown uml:uml /home/uml/.config/kscreenlockerrc
virgl_display runuser -u uml -- env HOME=/home/uml DISPLAY="$xdisplay" \
    XAUTHORITY="$xauthority" XDG_RUNTIME_DIR="$runtime" \
    dbus-run-session sh -c '
        printf "%s\n" "$DBUS_SESSION_BUS_ADDRESS" > "$XDG_RUNTIME_DIR/dbus-address"
        exec /usr/bin/kwin_wayland --no-lockscreen --x11-display "$DISPLAY" --socket wayland-0 --width 1280 --height 720
    ' > "$native_display/kwin.txt" 2>&1 &
kwin_launcher_pid=$!
kwin_launcher_stamp=$(native_process_state "$kwin_launcher_pid")
kwin_launcher_stamp=${kwin_launcher_stamp#*:}
remaining=60
while [ ! -S "$runtime/wayland-0" ] || [ ! -s "$runtime/dbus-address" ]; do
    kill -0 "$kwin_launcher_pid" 2>/dev/null || native_display_failure
    [ "$remaining" -gt 0 ] || native_display_failure
    remaining=$((remaining - 1))
    sleep 1
done
kwin_pid=
for candidate in $(pgrep -u "$(id -u uml)" -x kwin_wayland); do
    [ "$(readlink -f /proc/$candidate/exe)" = /usr/bin/kwin_wayland ] || continue
    tr '\000' '\n' < /proc/$candidate/cmdline | grep -Fxq -- --x11-display || continue
    tr '\000' '\n' < /proc/$candidate/environ | grep -Fxq "XDG_RUNTIME_DIR=$runtime" || continue
    kwin_pid=$candidate
    break
done
[ -n "$kwin_pid" ] || native_display_failure
tr '\000' '\n' < /proc/$kwin_pid/cmdline | grep -Fxq -- --no-lockscreen || \
    native_display_failure "compositor lock screen"
kwin_stamp=$(native_process_state "$kwin_pid")
kwin_stamp=${kwin_stamp#*:}
dbus_address=$(cat "$runtime/dbus-address")
printf 'wayland_server=kwin-x11\nwayland_lockscreen=off\nwayland_pid=%s\n' \
    "$kwin_pid" >> "$renderer"
'''
    setup += r'''if pgrep -x Xwayland >/dev/null || pgrep -x Xvfb >/dev/null; then
    native_display_failure
fi
'''
    return setup


def native_wsi_harness_command(
    options: argparse.Namespace,
    artifact_directory: Path,
    service: str,
    *,
    guest_files: gpu_guest.StagedGuestFiles,
    width: int = 1280,
    height: int = 720,
    display: str = "vnc",
    rootfs_type: str = "ext4",
) -> list[str]:
    """Boot the campaign's session target, which starts ``service``.

    ``guest_files`` gain the target; they are installed before the guest
    starts, and the caller writes them before running the command."""
    if display not in ("native", "vnc") or rootfs_type not in ("ext4", "erofs"):
        raise CampaignError("unsupported native WSI display or root filesystem")
    guest_files.add(CTS_TARGET_PATH, cts_target(service))
    command = [
        sys.executable, str(Path(__file__).with_name("run_gpu_guest.py")),
        "--emulator", str(options.emulator), "--rootfs", str(options.rootfs),
        "--rootfs-type", rootfs_type, "--root-device", "/dev/ubda",
        "--memory", options.memory, "--unit", CTS_TARGET_UNIT,
        "--timeout", "0", "--no-auto-poweroff",
        "--artifact-root", str(artifact_directory / "guest"),
        f"--virtio-serial-socket={artifact_socket_path(artifact_directory)}",
        *guest_files.arguments,
    ]
    command.extend((
        "--", f"ncpus={options.cpus}", f"--execution-engine={options.engine}",
        "ip=off", "--gpu", f"--gpu-display={display}",
        f"--gpu-width={width}", f"--gpu-height={height}",
    ))
    if display == "vnc":
        command.extend(("--gpu-vnc-bind=127.0.0.1", "--gpu-vnc-port=0"))
    return command


def shard_selection_command(target: str, selection: Selection) -> str:
    """Write the fraction's cases, in leaf order, to the named guest file."""
    return (
        "awk -v shard_index="
        f"{selection.fraction_index} "
        f"-v shard_count={selection.fraction_count} "
        "'NF && $1 !~ /^#/ { "
        "if ((seen % shard_count) == shard_index) print; ++seen }' "
        f'"$leaf" > {target}'
    )


def build_guest_launcher(
    selection: Selection | None,
    seed: int,
    timeout: int,
    rootfs_contract: RootfsContract,
    *,
    config_inventory_api: str | None = None,
    matrix: MatrixRun | None = None,
    artifact_transport: str = "console",
    renderer_backend: str = "virgl",
    resume_after: int = 0,
    checkpoint_bytes: int = 0,
    progress_timeout: int = 0,
    progress_interval: int = 15,
    waiver_sha256: str | None = None,
    progress_busy_timeout: int = 0,
) -> str:
    if waiver_sha256 is not None and (
        matrix is None or re.fullmatch(r"[0-9a-f]{64}", waiver_sha256) is None
    ):
        raise CampaignError("a waiver list needs an official matrix run and its digest")
    if config_inventory_api is not None and config_inventory_api not in CONFIG_CASES:
        raise CampaignError(
            f"unsupported EGL config inventory API: {config_inventory_api}"
        )
    if config_inventory_api is not None and selection is not None:
        raise CampaignError("config inventory cannot carry a must-pass selection")
    if matrix is not None and (
        selection is None or selection != matrix.selection
    ):
        raise CampaignError("matrix run does not match its selected shard")
    if matrix is not None and config_inventory_api is not None:
        raise CampaignError("matrix and config inventory modes are exclusive")
    if artifact_transport not in {"console", "virtio-serial"}:
        raise CampaignError("unsupported CTS artifact transport")
    if renderer_backend not in {"virgl", "zink"}:
        raise CampaignError("unsupported CTS renderer backend")
    if resume_after and selection is None:
        raise CampaignError("only a selected shard can resume after a case")
    if resume_after < 0 or checkpoint_bytes < 0 or progress_timeout < 0:
        raise CampaignError("shard resumption inputs must not be negative")
    if progress_busy_timeout < 0 or (
        progress_busy_timeout and progress_busy_timeout < progress_timeout
    ):
        raise CampaignError(
            "the busy stall bound must be zero or at least the stall timeout"
        )
    if progress_interval < 1:
        raise CampaignError("shard progress interval must be positive")
    checkpoint = 1 if (selection is not None and checkpoint_bytes > 0) else 0
    glcts = "/opt/vk-gl-cts/openglcts/glcts"
    archive = "/opt/vk-gl-cts/openglcts"
    output = "/run/uml-cts"
    selected = f"{output}/selected.txt"
    qpa = f"{output}/results.qpa"
    renderer = f"{output}/renderer.txt"
    summary = f"{output}/summary.txt"
    inventory_report = f"{output}/inventory.txt"
    config_caselist = f"{output}/configs.txt"
    hierarchy_inventory = selection is None and config_inventory_api is None
    config_inventory = config_inventory_api is not None
    derivative_checks = ""
    if rootfs_contract.filesystem_type == "ext4":
        derivative_checks = f"""
actual_lower_uuid=$(findmnt -n -o UUID -T /.uml-lower)
[ "$actual_lower_uuid" = {shlex.quote(rootfs_contract.filesystem_uuid)} ] || {{
    echo 'UML_CTS_ERROR: derivative filesystem UUID changed'
    exit 1
}}
actual_receipt_sha=$(sha256sum \
    {shlex.quote(rootfs_contract.guest_receipt_path)} | cut -d ' ' -f 1)
[ "$actual_receipt_sha" = \
    {shlex.quote(rootfs_contract.guest_receipt_sha256)} ] || {{
    echo 'UML_CTS_ERROR: in-guest derivative receipt changed'
    exit 1
}}
"""
    if hierarchy_inventory:
        export_pattern = f"{output}/${{packageName}}-cases.${{typeExtension}}"
        inventory_commands = []
        for module in GLES_INVENTORY_MODULES:
            inventory_commands.append(
                shlex.join(
                    [
                        glcts,
                        "--deqp-runmode=txt-caselist",
                        f"--deqp-case={module}.*",
                        f"--deqp-caselist-export-file={export_pattern}",
                        f"--deqp-archive-dir={archive}",
                        "--deqp-gl-context-type=egl",
                        "--deqp-egl-display-type=x11",
                        "--deqp-duplicate-case-name-check=enable",
                        f"--deqp-log-filename={qpa}",
                    ]
                )
            )
        command = ["/bin/sh", "-c", " &&\n".join(inventory_commands)]
    elif config_inventory:
        inventory_cases = CONFIG_INVENTORY_SCOPES[config_inventory_api]
        command = [
            glcts,
            (f"--deqp-case={inventory_cases[0]}" if len(inventory_cases) == 1
             else f"--deqp-caselist-file={config_caselist}"),
            "--deqp-gl-context-type=egl",
            f"--deqp-archive-dir={archive}",
            "--deqp-log-flush=enable",
            f"--deqp-log-filename={qpa}",
        ]
    elif matrix is not None:
        command = [
            glcts,
            *matrix_runner_arguments(
                matrix,
                selected,
                GUEST_WAIVER_PATH if waiver_sha256 is not None else None,
            ),
            f"--deqp-archive-dir={archive}",
            "--deqp-crashhandler=disable",
            "--deqp-test-oom=disable",
            "--deqp-log-images=disable",
            "--deqp-log-shader-sources=disable",
            "--deqp-log-flush=enable",
            "--deqp-terminate-on-fail=disable",
            f"--deqp-log-filename={qpa}",
        ]
    else:
        command = [
            glcts,
            "--deqp-gl-context-type=egl",
            f"--deqp-caselist-file={selected}",
            f"--deqp-archive-dir={archive}",
            "--deqp-watchdog=disable",
            "--deqp-crashhandler=disable",
            "--deqp-test-oom=disable",
            "--deqp-log-images=disable",
            "--deqp-log-shader-sources=disable",
            "--deqp-log-flush=enable",
            "--deqp-terminate-on-fail=disable",
            f"--deqp-base-seed={seed}",
            f"--deqp-log-filename={qpa}",
        ]
    if not hierarchy_inventory:
        command.append("--deqp-egl-display-type=x11")
    quoted_command = " \\\n    ".join(shlex.quote(argument) for argument in command)
    leaf_path = "" if selection is None else selection.leaf.path
    leaf_identity_check = ""
    mustpass_identity_check = ""
    renderer_environment = (
        "export MESA_LOADER_DRIVER_OVERRIDE=zink"
        if renderer_backend == "zink"
        else "unset MESA_LOADER_DRIVER_OVERRIDE"
    )
    renderer_check = (
        """grep -Eiq '^OpenGL renderer string:.*zink' "$renderer" &&
grep -Eiq '^OpenGL renderer string:.*virtio-gpu venus' "$renderer" || {
    echo 'UML_CTS_ERROR: renderer is not Zink over Virtio-GPU Venus'
    exit 1
}
if grep -Eiq 'llvmpipe|softpipe|swrast|software rasterizer|xvfb' "$renderer"; then
    echo 'UML_CTS_ERROR: software or forbidden renderer selected'
    exit 1
fi"""
        if renderer_backend == "zink"
        else """grep -Eiq '^OpenGL renderer string:.*virgl' "$renderer" || {
    echo 'UML_CTS_ERROR: renderer is not Virgl'
    exit 1
}
grep -Eiq '^OpenGL renderer string:.*zink' "$renderer" &&
grep -Eiq '^OpenGL renderer string:.*kosmickrisp' "$renderer" || {
    echo 'UML_CTS_ERROR: renderer is not Virgl over Zink/KosmicKrisp'
    exit 1
}
if grep -Eiq 'llvmpipe|softpipe|swrast|software rasterizer|xvfb' "$renderer"; then
    echo 'UML_CTS_ERROR: software or forbidden renderer selected'
    exit 1
fi"""
    )
    prepare_command = ""
    post_run_command = ""
    if hierarchy_inventory:
        post_run_command = """
LC_ALL=C
export LC_ALL
: > "$selected"
exported_packages=0
for package_caselist in "$output"/*-cases.txt; do
    [ -f "$package_caselist" ] || continue
    exported_packages=$((exported_packages + 1))
done
if [ "$exported_packages" -ne 2 ]; then
    echo "UML_CTS_ERROR: GLES inventory exported $exported_packages packages, expected 2"
    status=1
fi
for inventory_package in KHR-GLES2 KHR-GLES3; do
    package_caselist="$output/$inventory_package-cases.txt"
    if [ ! -s "$package_caselist" ]; then
        echo "UML_CTS_ERROR: GLES inventory package $inventory_package is empty"
        status=1
        continue
    fi
    if ! awk -v expected="$inventory_package" '
        $1 == "TEST:" || $1 == "GROUP:" {
            split($2, part, ".")
            if (part[1] != expected) exit 1
            seen = 1
        }
        END { if (!seen) exit 1 }
    ' "$package_caselist"; then
        echo "UML_CTS_ERROR: GLES inventory package $inventory_package contains another module"
        status=1
    fi
    cat "$package_caselist" >> "$selected"
done
complete=0
[ "$status" -ne 0 ] || complete=1
caselist_digest=$(sha256sum "$selected" | cut -d ' ' -f 1)
caselist_count=$(awk '$1 == "TEST:" { count++ } END { print count + 0 }' "$selected")
{
    printf 'schema_version=2\n'
    printf 'scope=gles2-gles3\n'
    printf 'complete=%s\n' "$complete"
    printf 'exported_package_count=%s\n' "$exported_packages"
    printf 'complete_caselist_sha256=%s\n' "$caselist_digest"
    printf 'complete_caselist_case_count=%s\n' "$caselist_count"
    printf 'registered_module=KHR-GLES2\n'
    printf 'registered_module=KHR-GLES3\n'
} > "$inventory_report"
chown uml:uml "$inventory_report" "$renderer"
"""
    elif config_inventory and len(CONFIG_INVENTORY_SCOPES[config_inventory_api]) > 1:
        # A scope wider than one case needs a caselist: --deqp-case selects a
        # single filter, and both cases must share one session so one QPA
        # carries the conformant list and the attribute table together.
        payload = "".join(
            f"{case}\n"
            for case in CONFIG_INVENTORY_SCOPES[config_inventory_api]
        )
        prepare_command = f"""
printf '%s' {shlex.quote(payload)} > {shlex.quote(config_caselist)}
chown uml:uml {shlex.quote(config_caselist)}
"""
    elif config_inventory:
        pass
    elif not selection.exact_cases:
        selection_command = shard_selection_command('"$selected"', selection)
    elif matrix is not None:
        # An official shard narrowed to named cases. Derive the shard exactly
        # as the fraction path does, keep the requested cases in the shard's
        # own order, and require the result to be the request itself: that
        # proves every case belongs to this shard, that none repeats, and that
        # the request is in the shard's order.
        request = "".join(f"{case}\n" for case in selection.exact_cases)
        selection_command = "\n".join([
            shard_selection_command('"$selected.shard"', selection),
            f'printf \'%s\' {shlex.quote(request)} > "$selected.request"',
            'grep -Fx -f "$selected.request" "$selected.shard" > "$selected" || true',
            'if ! cmp -s "$selected" "$selected.request"; then',
            "    echo 'UML_CTS_ERROR: exact cases are not an ordered subset "
            "of the selected matrix shard'",
            "    exit 1",
            "fi",
            'rm -f -- "$selected.shard" "$selected.request"',
        ])
    else:
        selection_lines = [': > "$selected"']
        selection_lines.extend(
            f"if ! grep -Fx -- {shlex.quote(case)} \"$leaf\" >> "
            '"$selected"; then '
            "echo 'UML_CTS_ERROR: exact case is absent from selected "
            "must-pass leaf'; exit 1; fi"
            for case in selection.exact_cases
        )
        selection_command = "\n".join(selection_lines)
    if selection is not None:
        leaf_identity_check = f"""
actual_leaf_sha=$(sha256sum "$leaf" | cut -d ' ' -f 1)
[ "$actual_leaf_sha" = {shlex.quote(selection.leaf.sha256)} ] || {{
    echo 'UML_CTS_ERROR: selected must-pass leaf identity changed'
    exit 1
}}
"""
        resume_command = ""
        if resume_after:
            # A resumed session re-derives the same ordered selection from the
            # same authenticated leaf and drops only the cases the earlier
            # sessions already accounted for.
            resume_command = f"""
tail -n +{resume_after + 1} "$selected" > "$selected.resume"
mv "$selected.resume" "$selected"
"""
        expected_after_resume = selection.expected_count - resume_after
        if expected_after_resume < 1:
            raise CampaignError("shard resumption selects no remaining cases")
        prepare_command = f"""
{selection_command}
{resume_command}
selected_count=$(grep -Evc '^[[:space:]]*(#|$)' "$selected")
[ "$selected_count" -eq {expected_after_resume} ] || {{
    echo "UML_CTS_ERROR: selected $selected_count cases, expected {expected_after_resume}"
    exit 1
}}
chown uml:uml "$selected" "$renderer"
"""
    if matrix is not None:
        mustpass_identity_check = f"""
actual_mustpass_size=$(wc -c < {shlex.quote(matrix.mustpass_xml_path)} | tr -d ' ')
[ "$actual_mustpass_size" -eq {matrix.mustpass_xml_size} ] || {{
    echo 'UML_CTS_ERROR: must-pass XML size changed'
    exit 1
}}
actual_mustpass_sha=$(sha256sum \
    {shlex.quote(matrix.mustpass_xml_path)} | cut -d ' ' -f 1)
[ "$actual_mustpass_sha" = {shlex.quote(matrix.mustpass_xml_sha256)} ] || {{
    echo 'UML_CTS_ERROR: must-pass XML identity changed'
    exit 1
}}
"""
        if waiver_sha256 is not None:
            mustpass_identity_check += f"""
actual_waiver_sha=$(sha256sum {shlex.quote(GUEST_WAIVER_PATH)} | cut -d ' ' -f 1)
[ "$actual_waiver_sha" = {shlex.quote(waiver_sha256)} ] || {{
    echo 'UML_CTS_ERROR: waiver list identity changed'
    exit 1
}}
"""

    transport_functions = artifact_transport_program(
        error_prefix="UML_CTS_ERROR:",
        base64_line_length=1024,
        missing_source_status=0,
        console_transport=True,
    )
    checkpoint_function = qpa_checkpoint_program()
    return f"""#!/bin/sh
set -eu
output={shlex.quote(output)}
leaf={shlex.quote(leaf_path)}
selected={shlex.quote(selected)}
qpa={shlex.quote(qpa)}
renderer={shlex.quote(renderer)}
summary={shlex.quote(summary)}
inventory_report={shlex.quote(inventory_report)}
hierarchy_inventory_mode={1 if hierarchy_inventory else 0}
selected_mode={1 if selection is not None else 0}
artifact_transport={shlex.quote(artifact_transport)}
channel={shlex.quote(VIRTIO_SERIAL_GUEST_DEVICE)}
channel_ready=0
checkpoint_mode={checkpoint}
checkpoint_bytes={checkpoint_bytes}
progress_timeout={progress_timeout}
progress_busy_timeout={progress_busy_timeout}
progress_interval={progress_interval}
clock_ticks=$(getconf CLK_TCK 2>/dev/null || echo 100)
[ "$clock_ticks" -gt 0 ] 2>/dev/null || clock_ticks=100
progress_busy_ticks=$((progress_interval * clock_ticks / 10))
prerun_emitted=0
artifact_framed=0
qpa_offset=0
qpa_segments=0
checkpoint_failures=0
progress_timed_out=0
status=1

{transport_functions}
{checkpoint_function}
emit_prerun()
{{
    [ "$prerun_emitted" -eq 0 ] || return 0
    emit_artifact renderer.txt "$renderer" || return 1
    if [ "$hierarchy_inventory_mode" -eq 1 ]; then
        emit_artifact inventory.txt "$inventory_report" || return 1
    elif [ "$selected_mode" -eq 1 ]; then
        emit_artifact selected.txt "$selected" || return 1
    fi
    prerun_emitted=1
}}

cts_cpu_ticks()
{{
    # User plus system clock ticks of every running CTS process (all threads):
    # a case computing in the guest keeps consuming CPU while a wedged wait
    # on the host consumes none.
    cts_ticks_total=0
    for cts_process in $(pgrep -x glcts 2>/dev/null); do
        cts_ticks=$(sed -n 's/^[^)]*) //p' "/proc/$cts_process/stat" 2>/dev/null | \
            awk '{{ print $12 + $13 }}')
        [ -n "$cts_ticks" ] && cts_ticks_total=$((cts_ticks_total + cts_ticks))
    done
    printf '%s\n' "$cts_ticks_total"
}}

watch_cts()
{{
    watched_pid="$1"
    watched_size=0
    stalled_seconds=0
    idle_seconds=0
    watched_ticks=$(cts_cpu_ticks)
    while kill -0 "$watched_pid" 2>/dev/null; do
        sleep "$progress_interval"
        watched_current=$(wc -c < "$qpa" 2>/dev/null | tr -d ' ')
        [ -n "$watched_current" ] || watched_current=0
        watched_case=$(tail -c 1048576 "$qpa" 2>/dev/null | \
            sed -n 's/^#beginTestCaseResult //p' | tail -n 1)
        if [ "$watched_current" -ne "$watched_size" ]; then
            watched_size=$watched_current
            stalled_seconds=0
        else
            stalled_seconds=$((stalled_seconds + progress_interval))
        fi
        current_ticks=$(cts_cpu_ticks)
        if [ "$current_ticks" -ge $((watched_ticks + progress_busy_ticks)) ]; then
            idle_seconds=0
        else
            idle_seconds=$((idle_seconds + progress_interval))
        fi
        watched_ticks=$current_ticks
        printf 'UML_CTS_PROGRESS|qpa_bytes=%s|stalled_seconds=%s|idle_seconds=%s|cpu_ticks=%s|case=%s\n' \
            "$watched_current" "$stalled_seconds" "$idle_seconds" \
            "$current_ticks" "$watched_case"
        # A checkpoint that fails keeps its bytes pending, so the watch stays
        # up and the next one carries them. Only a channel that keeps failing
        # is worth giving up on, and the exit trap still makes a final try.
        if [ $((watched_current - qpa_offset)) -ge "$checkpoint_bytes" ] && \
           [ "$checkpoint_failures" -lt 3 ]; then
            if flush_qpa 0; then
                checkpoint_failures=0
            else
                checkpoint_failures=$((checkpoint_failures + 1))
                [ "$artifact_framed" -eq 0 ] || checkpoint_failures=3
                echo 'UML_CTS_ERROR: QPA checkpoint transfer failed'
            fi
        fi
        # A case with no QPA output is cut when it also burns no CPU for the
        # stall timeout, or, while it keeps computing, at the busy bound.
        if [ "$progress_timeout" -gt 0 ] && \
           [ "$stalled_seconds" -ge "$progress_timeout" ]; then
            if [ "$progress_busy_timeout" -le 0 ] || \
               [ "$idle_seconds" -ge "$progress_timeout" ] || \
               [ "$stalled_seconds" -ge "$progress_busy_timeout" ]; then
                progress_timed_out=1
                return 0
            fi
        fi
    done
}}

finish()
{{
    trap - EXIT HUP INT TERM
    # The exit path never stops early. It is the only place the host's final
    # frame can come from, and errexit here once turned a single failed write
    # on a full guest filesystem into a session with no summary at all.
    set +e
    printf 'UML_CTS_STATUS|rc=%s\n' "$status"
    transfer_status=0
    if ! prepare_channel; then
        echo 'UML_CTS_ERROR: artifact transport did not become ready'
        transfer_status=1
    fi
    emit_prerun || transfer_status=1
    if [ "$checkpoint_mode" -eq 1 ]; then
        flush_qpa 1 || transfer_status=1
    else
        emit_artifact results.qpa "$qpa" || transfer_status=1
    fi
    # The results are transported; what is left of them in the guest is only
    # holding space the summary may need.
    if [ "$checkpoint_mode" -eq 1 ]; then
        : > "$qpa" 2>/dev/null
    fi
    printf 'cts_rc=%s\n' "$status" > "$summary" || \
        echo 'UML_CTS_ERROR: the summary could not be written'
    # Sent last, and never withheld: a session that lost results still
    # reports its verdict, and the host's wait ends on this frame.
    emit_artifact summary.txt "$summary" || transfer_status=1
    if [ "$channel_ready" -eq 1 ]; then
        exec 3>&-
    fi
    if [ "$transfer_status" -ne 0 ]; then
        echo 'UML_CTS_ERROR: acknowledged artifact transfer failed'
    fi
    if command -v native_display_cleanup >/dev/null; then
        native_display_cleanup || echo 'UML_CTS_ERROR: native display cleanup failed'
    fi
    sync
    systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1
}}
trap finish EXIT HUP INT TERM

install -d -m 0755 -o uml -g uml "$output"
actual_page_size=$(getconf PAGESIZE)
[ "$actual_page_size" -eq {gpu_guest.GUEST_PAGE_SIZE} ] || {{
    echo "UML_CTS_ERROR: guest page size $actual_page_size does not match {gpu_guest.GUEST_PAGE_SIZE}"
    exit 1
}}
printf 'guest_page_size=%s\n' "$actual_page_size" >> "$renderer"
{derivative_checks}
{mustpass_identity_check}
{leaf_identity_check}
actual_glcts_sha=$(sha256sum {shlex.quote(glcts)} | cut -d ' ' -f 1)
[ "$actual_glcts_sha" = {shlex.quote(rootfs_contract.glcts_sha256)} ] || {{
    echo 'UML_CTS_ERROR: glcts identity changed'
    exit 1
}}

{native_x11_setup()}
{renderer_environment}
unset WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
export EGL_PLATFORM=x11
runuser -u uml -- env \
    HOME=/home/uml DISPLAY="$xdisplay" XAUTHORITY="$xauthority" \
    XDG_RUNTIME_DIR="$runtime" glxinfo -B >> "$renderer" 2>&1 || {{
        echo 'UML_CTS_ERROR: glxinfo failed'
        exit 1
    }}
runuser -u uml -- env \
    HOME=/home/uml DISPLAY="$xdisplay" XAUTHORITY="$xauthority" \
    XDG_RUNTIME_DIR="$runtime" eglinfo -B -p x11 >> "$renderer" 2>&1 || true
{renderer_check}

{prepare_command}

if [ "$checkpoint_mode" -eq 1 ]; then
    prepare_channel || {{
        echo 'UML_CTS_ERROR: artifact transport did not become ready'
        exit 1
    }}
    emit_prerun || {{
        echo 'UML_CTS_ERROR: acknowledged artifact transfer failed'
        exit 1
    }}
fi

set +e
if [ "$checkpoint_mode" -eq 1 ]; then
    runuser -u uml -- env \
        HOME=/home/uml DISPLAY="$xdisplay" EGL_PLATFORM=x11 \
        XAUTHORITY="$xauthority" XDG_RUNTIME_DIR="$runtime" \
        timeout --signal=TERM --kill-after=30 {timeout} \
        {quoted_command} &
    cts_pid=$!
    watch_cts "$cts_pid"
    if [ "$progress_timed_out" -eq 1 ]; then
        # Never signal a wedged case from inside the guest: the checkpointed
        # prefix is already transported and the host owns the recovery.
        status=124
    else
        wait "$cts_pid"
        status=$?
    fi
else
    runuser -u uml -- env \
        HOME=/home/uml DISPLAY="$xdisplay" EGL_PLATFORM=x11 \
        XAUTHORITY="$xauthority" XDG_RUNTIME_DIR="$runtime" \
        timeout --signal=TERM --kill-after=30 {timeout} \
        {quoted_command}
    status=$?
fi
set -e
{post_run_command}
exit "$status"
"""


# The session every conformance campaign boots, on macOS and in Kernarium:
# multi-user.target and the campaign's service, with nothing that competes for
# its display or stalls its poweroff. The image's display manager claims its
# own VT as soon as it starts, Xorg suspends AIGLX for that switch, and a
# pending GL query then never returns; the fixed-VT getties take the same VTs.
# No suite uses the container runtime, and a short campaign that powers off
# while Docker is still loading its BPF programs leaves kworkers blocked in
# bpf_prog_free_deferred, so the guest never reaches poweroff. Conflicts= stops
# each of these units in the boot transaction, whatever enables it.
CTS_TARGET_UNIT = "uml-cts.target"
CTS_TARGET_PATH = f"/etc/systemd/system/{CTS_TARGET_UNIT}"
CTS_TARGET_CONFLICTS = (
    "display-manager.service",
    "sddm.service",
    "getty@tty0.service",
    "getty@tty1.service",
    "docker.service",
    "docker.socket",
    "containerd.service",
)
SERVICE_UNIT_NAME = re.compile(r"[A-Za-z0-9@_.:-]+\.service")


def cts_target(service: str) -> str:
    """The campaign's session target, which starts ``service``."""
    if SERVICE_UNIT_NAME.fullmatch(service) is None:
        raise CampaignError(f"not a service unit name: {service!r}")
    return (
        "[Unit]\n"
        "Description=Darwin UML conformance campaign session\n"
        "Requires=multi-user.target\n"
        "After=multi-user.target\n"
        f"Wants={service}\n"
        f"Conflicts={' '.join(CTS_TARGET_CONFLICTS)}\n"
        "AllowIsolate=yes\n"
    )


CTS_SERVICE = """[Unit]
Description=Darwin UML VK-GL-CTS campaign
After=systemd-user-sessions.service
Wants=systemd-user-sessions.service

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/uml-vk-gl-cts
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
"""


def gl_scope_launch_environment(options: argparse.Namespace) -> tuple[str, ...]:
    """Name the host switches this run's stack needs, in a stable order.

    The host always publishes Venus and the host-visible blob aperture, which
    guest Zink and a GL46 Virgl guest's GL_ARB_buffer_storage rely on. A
    Virgl guest also reports its host provider.
    """
    if options.renderer == "zink":
        return ()
    return ("UML_GPU_CAPABILITY_REPORT",)


def plan_harness_command(
    options: argparse.Namespace,
    selection: Selection | None,
    artifact_directory: Path,
    *,
    resume_after: int = 0,
) -> tuple[list[str], gpu_guest.StagedGuestFiles]:
    """The guest harness command and the files it installs, unwritten.

    The launcher, its service and any waiver list reach the guest as files;
    their digests are in the command, so it records exactly what ran."""
    applied_waivers = getattr(options, "applied_waivers", ())
    waiver_text = waiver_xml(applied_waivers) if applied_waivers else None
    guest_files = gpu_guest.StagedGuestFiles(
        artifact_directory / gpu_guest.GUEST_FILE_DIRECTORY
    )
    if waiver_text is not None:
        guest_files.add(GUEST_WAIVER_PATH, waiver_text)
    guest_files.add(
        GUEST_LAUNCHER_PATH,
        build_guest_launcher(
            selection,
            options.seed,
            options.shard_timeout,
            options.rootfs_contract,
            config_inventory_api=options.config_inventory,
            matrix=options.matrix_run,
            artifact_transport="virtio-serial",
            renderer_backend=options.renderer,
            resume_after=resume_after,
            checkpoint_bytes=QPA_CHECKPOINT_BYTES,
            progress_timeout=options.console_stall_timeout,
            progress_busy_timeout=options.console_stall_busy_timeout,
            waiver_sha256=(
                hashlib.sha256(waiver_text.encode("utf-8")).hexdigest()
                if waiver_text is not None
                else None
            ),
        ),
        0o755,
    )
    guest_files.add(GUEST_SERVICE_PATH, CTS_SERVICE)
    command = native_wsi_harness_command(
        options, artifact_directory, GUEST_SERVICE_NAME,
        guest_files=guest_files,
        width=options.width, height=options.height,
        display=options.display, rootfs_type=options.rootfs_type,
    )
    launch_environment = [
        f"{name}=1" for name in gl_scope_launch_environment(options)
    ]
    if options.display == "native":
        launch_environment.append("UML_GPU_FRAME_STATS=1")
    return ["env", *launch_environment, *command], guest_files


def build_harness_command(
    options: argparse.Namespace,
    selection: Selection | None,
    artifact_directory: Path,
    *,
    resume_after: int = 0,
) -> list[str]:
    """The recorded guest harness command; building it writes nothing."""
    return plan_harness_command(
        options, selection, artifact_directory, resume_after=resume_after
    )[0]


def prepare_harness_command(
    options: argparse.Namespace,
    selection: Selection | None,
    artifact_directory: Path,
    *,
    resume_after: int = 0,
) -> list[str]:
    """Write the files a harness command installs and return the command."""
    command, guest_files = plan_harness_command(
        options, selection, artifact_directory, resume_after=resume_after
    )
    guest_files.write()
    return command


def stop_guest(process: subprocess.Popen[bytes]) -> None:
    guest_process.stop_process(process, guest_process.HARNESS_STOP)


def console_progressed(log_path: Path, start: int, end: int) -> tuple[int, bool]:
    """Classify console bytes ``[start, end)`` as progress or stall noise.

    Returns how far the console has been consumed and whether the new output
    carries evidence that the guest is still working.  Only whole lines are
    classified; a trailing partial line is left for the next observation so a
    line is never judged on half of itself.
    """
    if end - start > MAX_CONSOLE_PROGRESS_SCAN:
        return end, True
    try:
        with log_path.open("rb") as console:
            console.seek(start)
            chunk = console.read(end - start)
    except OSError:
        # An unreadable console is not evidence that the guest stopped.
        return end, True
    boundary = chunk.rfind(b"\n")
    if boundary < 0:
        return start, False
    consumed = start + boundary + 1
    for line in chunk[:boundary].decode("utf-8", errors="replace").splitlines():
        body = KERNEL_CONSOLE_TIMESTAMP.sub("", line, count=1).strip()
        if body and not any(
            pattern.match(body) for pattern in KERNEL_STALL_DIAGNOSTICS
        ):
            return consumed, True
    return consumed, False


def wait_for_guest(
    process: subprocess.Popen[bytes],
    log_path: Path,
    timeout: int,
    artifact_socket_path: Path,
    *,
    console_stall_timeout: int = 0,
    shutdown_grace: int = SHUTDOWN_GRACE_SECONDS,
) -> tuple[str, str]:
    started = time.monotonic()
    deadline = started + timeout
    next_heartbeat = started + 30
    last_console_size = 0
    last_artifact_frames = 0
    last_progress = started
    scanned_frames = 0
    delivered: float | None = None
    transport = AcknowledgedArtifactSocket(artifact_socket_path)

    def interrupted(error: BaseException, reason: str) -> GuestSessionInterrupted:
        # Keep every acknowledged frame and console byte: a shard resumption
        # is only truthful when it starts from evidence the guest really sent.
        return GuestSessionInterrupted(
            str(error),
            reason=reason,
            transcript=(
                log_path.read_text(encoding="utf-8", errors="replace")
                if log_path.exists()
                else ""
            ),
            artifact_transcript="".join(transport.lines),
            delivered=delivered is not None,
        )

    try:
        while (remaining := deadline - time.monotonic()) > 0:
            transport.service(timeout=min(ARTIFACT_SERVICE_INTERVAL, remaining))
            if process.poll() is not None:
                break
            now = time.monotonic()
            if delivered is None and scanned_frames < len(transport.lines):
                if any(
                    line.startswith(FINAL_ARTIFACT_FRAME)
                    for line in transport.lines[scanned_frames:]
                ):
                    delivered = now
                scanned_frames = len(transport.lines)
            if (
                delivered is not None
                and shutdown_grace > 0
                and now - delivered >= shutdown_grace
            ):
                raise interrupted(
                    TimeoutError(
                        "VK-GL-CTS guest delivered its complete evidence but "
                        f"did not stop within {shutdown_grace} seconds"
                    ),
                    "stalled",
                )
            if now >= next_heartbeat:
                size = log_path.stat().st_size if log_path.exists() else 0
                artifact_frames = len(transport.lines)
                if size < last_console_size:
                    # Rotated or truncated: resynchronise rather than judge a
                    # console that is still being written.
                    last_console_size = size
                    console_progress = True
                elif size > last_console_size:
                    last_console_size, console_progress = console_progressed(
                        log_path, last_console_size, size
                    )
                else:
                    console_progress = False
                if (
                    console_progress
                    or artifact_frames != last_artifact_frames
                ):
                    last_artifact_frames = artifact_frames
                    last_progress = now
                elif (
                    console_stall_timeout > 0
                    and (size > 0 or artifact_frames > 0)
                    and now - last_progress >= console_stall_timeout
                ):
                    raise interrupted(
                        TimeoutError(
                            "VK-GL-CTS guest console and artifact transport made "
                            "no progress for "
                            f"{console_stall_timeout} seconds"
                        ),
                        "stalled",
                    )
                print(
                    "VK-GL-CTS guest is running; "
                    f"console_bytes={size} "
                    f"artifact_frames={artifact_frames}",
                    flush=True,
                )
                next_heartbeat = now + 30
        else:
            raise interrupted(
                TimeoutError(f"VK-GL-CTS guest exceeded {timeout} seconds"),
                "stalled",
            )
        process.wait(timeout=5)
        drain_deadline = time.monotonic() + 2
        while (
            transport.connection is not None
            and (remaining := drain_deadline - time.monotonic()) > 0
        ):
            transport.service(timeout=min(ARTIFACT_SERVICE_INTERVAL, remaining))
        artifact_transcript = transport.finish()
    except GuestSessionInterrupted:
        raise
    except CampaignError as error:
        # A host-side abort tears the transport down mid-frame. Report it as
        # an interrupted session so the retained prefix is still usable.
        raise interrupted(error, "aborted") from error
    finally:
        transport.close()
    return (
        log_path.read_text(encoding="utf-8", errors="replace"),
        artifact_transcript,
    )


@dataclass(frozen=True)
class RawSession:
    """What one guest session left behind, however it ended."""

    transcript: str
    artifact_transcript: str
    return_code: int | None
    outcome: str
    detail: str | None
    artifacts: dict[str, bytes]
    shutdown_stall: bool = False


def session_environment(index: int, guest: Path) -> dict[str, str] | None:
    """Give each continuation guest its own exclusive ownership journal.

    The first session keeps the journal its owner armed before creating this
    leaf, so the existing pool contract is unchanged; a later session proves
    its own emulator was reaped in a journal beside its evidence.
    """
    setting = os.environ.get(guest_process.OWNERSHIP_ENV)
    if setting is None or index == 0:
        return None
    try:
        token = json.loads(setting)["token"]
    except (ValueError, KeyError, TypeError) as error:
        raise CampaignError("invalid inherited guest ownership setting") from error
    return {
        **os.environ,
        guest_process.OWNERSHIP_ENV: json.dumps(
            {
                "path": str(guest / guest_process.JOURNAL_NAME),
                "token": token,
            }
        ),
    }


def run_guest_session(
    command: Sequence[str],
    console_path: Path,
    transport_path: Path,
    socket_path: Path,
    *,
    guest_timeout: int,
    console_stall_timeout: int = 0,
    environment: dict[str, str] | None = None,
) -> RawSession:
    """Run one guest and keep whatever evidence it managed to transport."""
    outcome = "completed"
    detail: str | None = None
    artifact_transcript = ""
    return_code: int | None = None
    shutdown_stall = False
    process: subprocess.Popen[bytes] | None = None
    try:
        with console_path.open("wb") as console:
            with guest_process.managed_child(
                list(command),
                stop=stop_guest,
                stdout=console,
                stderr=subprocess.STDOUT,
                **({} if environment is None else {"env": environment}),
            ) as process:
                transcript, artifact_transcript = wait_for_guest(
                    process,
                    console_path,
                    guest_timeout,
                    socket_path,
                    console_stall_timeout=console_stall_timeout,
                )
        return_code = process.returncode
        if return_code != 0:
            outcome = "aborted"
            detail = f"guest harness exited with status {return_code}"
        elif os.path.lexists(socket_path):
            outcome = "aborted"
            detail = "emulator did not remove the artifact socket"
        else:
            try:
                validate_guest_shutdown(transcript)
            except CampaignError as error:
                outcome = "aborted"
                detail = str(error)
    except GuestSessionInterrupted as error:
        transcript = error.transcript
        artifact_transcript = error.artifact_transcript
        outcome = error.reason
        detail = str(error)
        return_code = process.returncode if process is not None else None
        # A guest that transported and had acknowledged its final frame before
        # it refused to power off cost this shard no coverage.  Its evidence is
        # complete, so the pool keeps running and the stall is recorded instead.
        shutdown_stall = error.reason == "stalled" and error.delivered
        if shutdown_stall:
            print(f"VK-GL-CTS shutdown stall: {detail}", file=sys.stderr, flush=True)
    transport_path.write_text(artifact_transcript, encoding="ascii")
    failure = None
    if os.environ.get(CAMPAIGN_FAILURE_ENV) is not None:
        if DIRTY_DRM_SHUTDOWN in transcript:
            failure = "dirty-shutdown"
        elif outcome == "stalled" and not shutdown_stall:
            failure = "stalled"
    try:
        artifacts = (
            decode_artifacts(artifact_transcript, partial=outcome != "completed")
            if artifact_transcript
            else {}
        )
        if failure:
            # Reap the guest and retain its transported prefix before asking
            # the pool to cancel peers. Never resume a known-broken emulator.
            write_artifacts(artifacts, console_path.parent / "artifacts")
    finally:
        # Malformed partial evidence must not hide a confirmed emulator stall.
        if failure:
            report_campaign_emulator_failure(failure)
    if failure:
        raise CampaignError(f"emulator failure: {failure}: {detail}")
    return RawSession(
        transcript, artifact_transcript, return_code, outcome, detail, artifacts,
        shutdown_stall,
    )


def session_paths(directory: Path, index: int) -> tuple[Path, Path, Path, Path]:
    """Return the guest, console, transport and artifact paths of one session.

    Session zero keeps the historical single-session layout so an ordinary
    shard produces exactly the files, socket and command it always did.
    """
    if index == 0:
        return (
            directory,
            directory / "console.log",
            directory / ARTIFACT_TRANSPORT_LOG,
            directory / "artifacts",
        )
    nested = directory / SESSION_DIRECTORY / f"{index:02d}"
    return (
        nested,
        nested / "console.log",
        nested / ARTIFACT_TRANSPORT_LOG,
        nested / "artifacts",
    )


def relative_session_path(directory: Path, path: Path) -> str:
    return str(path.relative_to(directory))


def run_shard_sessions(
    directory: Path,
    *,
    build_command: Callable[[int, Path], list[str]],
    resolve_expected: Callable[[dict[str, bytes]], tuple[str, ...]],
    check_session: Callable[[int, int, dict[str, bytes]], None],
    guest_timeout: int,
    console_stall_timeout: int,
    max_resumptions: int,
    on_sessions: Callable[[tuple[SessionEvidence, ...]], None] | None = None,
) -> tuple[tuple[SessionEvidence, ...], tuple[str, ...], dict[str, str]]:
    """Run a shard as one or more guest sessions and merge their results.

    A session that stalls or is killed by a host abort still delivers the QPA
    checkpoint prefix it transported.  That termination is an abort of the
    session, not a result for whatever case was in flight: the host invents no
    status, and the next session resumes at the same index the dead one
    reached, so every case after the last transported result is simply re-run.
    The last case dEQP announced on the console is kept as a diagnostic only.
    A shard that cannot get past a case exhausts its bounded resumptions and
    fails as a whole rather than blaming that case.
    """
    if max_resumptions < 0:
        raise CampaignError("resumption bound must not be negative")
    sessions: list[SessionEvidence] = []
    merged: dict[str, str] = {}
    expected: tuple[str, ...] = ()
    resume_after = 0
    while True:
        index = len(sessions)
        guest, console_path, transport_path, artifact_path = session_paths(
            directory, index
        )
        guest.mkdir(parents=True, exist_ok=True)
        environment = session_environment(index, guest)
        raw = run_guest_session(
            build_command(resume_after, guest),
            console_path,
            transport_path,
            artifact_socket_path(guest),
            guest_timeout=guest_timeout,
            console_stall_timeout=console_stall_timeout,
            environment=environment,
        )
        if raw.outcome != "completed" and DIRTY_DRM_SHUTDOWN in raw.transcript:
            raise CampaignError(
                "Virtio-GPU memory manager was not clean during guest shutdown"
            )
        qpa_text = decode_qpa(raw.artifacts.get(QPA_ARTIFACT, b""))
        if raw.outcome == "completed" and not qpa_text:
            # Never invent a terminated case for a guest that stopped cleanly:
            # an empty QPA there is a transport failure, not a hung case.
            raise CampaignError("a completed shard session transported no QPA")
        prefix = scan_qpa(qpa_text) if qpa_text else QpaPrefix({}, None, False)
        if index == 0:
            expected = resolve_expected(raw.artifacts)
            if not expected:
                raise CampaignError("shard resolved an empty expected case list")
        check_session(index, resume_after, raw.artifacts)
        completed = tuple(prefix.results)
        window = expected[resume_after : resume_after + len(completed)]
        if completed != window:
            raise CampaignError(
                "guest results left the expected traversal order at "
                f"session {index}"
            )
        # A shutdown stall arrives with the guest's complete acknowledged
        # evidence, so its session ends the shard exactly like a clean stop.
        # The stall itself stays recorded beside that session.
        shutdown_stall = raw.shutdown_stall and prefix.ended_session
        finished = prefix.ended_session and (
            raw.outcome == "completed" or shutdown_stall
        )
        outcome = "completed" if finished else raw.outcome
        if raw.outcome == "completed" and not prefix.ended_session:
            # The guest stopped cleanly but its CTS process did not finish the
            # caselist: its own progress watchdog or a crash ended the case.
            outcome = "stalled" if guest_reported_stall(raw.artifacts) else "aborted"
        position = resume_after + len(completed)
        pending = expected[position] if position < len(expected) else None
        if not finished and prefix.in_flight_case not in (None, pending):
            raise CampaignError(
                "guest QPA trailer names a case outside the traversal order"
            )
        console_case = None if finished else last_console_case(raw.transcript)
        write_artifacts(raw.artifacts, artifact_path)
        sessions.append(
            SessionEvidence(
                index=index,
                outcome=outcome,
                resume_after=resume_after,
                qpa_text=qpa_text,
                completed_cases=completed,
                console_case=console_case,
                console=relative_session_path(directory, console_path),
                artifact_transport=relative_session_path(directory, transport_path),
                artifacts_directory=relative_session_path(directory, artifact_path),
                return_code=raw.return_code,
                shutdown_stall=shutdown_stall,
                ownership_journal=(
                    None
                    if environment is None
                    else relative_session_path(
                        directory, guest / guest_process.JOURNAL_NAME
                    )
                ),
            )
        )
        merged.update(prefix.results)
        # The dead session contributed every result it transported and nothing
        # else, so the next one restarts at exactly the case it was running.
        resume_after = position
        resuming = not finished and resume_after < len(expected)
        if index == 0 and resuming:
            # The shard-level artifact set becomes the host-merged one, so the
            # first guest's own files move beside every later session's.
            nested = directory / SESSION_DIRECTORY / "00" / "artifacts"
            nested.parent.mkdir(parents=True, exist_ok=True)
            os.replace(artifact_path, nested)
            sessions[0] = replace(
                sessions[0],
                artifacts_directory=relative_session_path(directory, nested),
            )
        if on_sessions is not None:
            on_sessions(tuple(sessions))
        if not resuming:
            break
        print(
            f"VK-GL-CTS shard session {index} {outcome} after "
            f"{len(completed)} cases; the emulator exited around "
            f"{console_case}, re-running from index {resume_after}",
            file=sys.stderr,
            flush=True,
        )
        # Record what happened before refusing to continue, so an exhausted
        # shard still leaves a truthful catalogue of its guest sessions.
        if len(sessions) > max_resumptions:
            raise CampaignError(
                f"shard session aborted {len(sessions)} times at "
                f"{console_case or pending}"
            )
    return tuple(sessions), expected, merged


def last_console_case(transcript: str) -> str | None:
    """Return the last case dEQP announced on the guest console.

    This is a diagnostic, not a verdict.  The host never turns it into a
    result: a session that died while this case was on screen is re-run from
    the last case it actually transported, which may be an earlier one.
    """
    found = CONSOLE_CASE.findall(transcript)
    return found[-1] if found else None


def guest_reported_stall(artifacts: dict[str, bytes]) -> bool:
    summary = artifacts.get("summary.txt")
    if summary is None:
        return False
    return "cts_rc=124" in summary.decode("utf-8", errors="replace")


def merged_shard_artifacts(
    sessions: Sequence[SessionEvidence],
    directory: Path,
    expected_count: int,
) -> dict[str, bytes]:
    """Return the shard-level artifact set for a resumed shard.

    Every session keeps its own transported files.  The shard-level set
    reuses the first session's selection and renderer evidence and replaces
    only the QPA and the summary, whose host-merged provenance is recorded
    both inside the summary and in ``run.json``.
    """
    first = directory / sessions[0].artifacts_directory
    artifacts = {
        path.name: path.read_bytes()
        for path in sorted(first.iterdir())
        if path.name not in {QPA_ARTIFACT, SUMMARY_ARTIFACT}
    }
    # The last session that reached its own exit trap owns the guest identity
    # rows.  A shard whose final case hung still has one, because the guest
    # transports its summary before powering off.
    final = next(
        (
            candidate
            for session in reversed(sessions)
            if (
                candidate := directory
                / session.artifacts_directory
                / SUMMARY_ARTIFACT
            ).is_file()
        ),
        None,
    )
    if final is None:
        raise CampaignError("no shard session transported a summary")
    artifacts[QPA_ARTIFACT] = merge_qpa_sessions(sessions).encode(
        "utf-8", errors="surrogateescape"
    )
    artifacts[SUMMARY_ARTIFACT] = merged_summary(
        sessions, final.read_bytes(), expected_count
    )
    return artifacts


def merged_summary(
    sessions: Sequence[SessionEvidence],
    base: bytes,
    expected_count: int,
) -> bytes:
    """Return the host-merged shard summary.

    The completing guest's own identity rows are kept and the shard-wide
    counts and merge provenance are stated explicitly, so a resumed shard
    never looks like a single guest's run.  No case is host-recorded: the
    ``host_recorded_`` rows are dropped from the base summary so a guest
    cannot smuggle one into a merged one either.
    """
    overridden = (
        "cts_rc=",
        "expected_case_count=",
        "provenance=",
        "sessions=",
        "host_recorded_",
    )
    kept = [
        line
        for line in base.decode("utf-8", errors="strict").splitlines()
        if line and not line.startswith(overridden)
    ]
    rows = [
        "cts_rc=0",
        *kept,
        f"expected_case_count={expected_count}",
        "provenance=host-merged",
        f"sessions={len(sessions)}",
    ]
    return "".join(f"{row}\n" for row in rows).encode("utf-8")


def session_records(
    sessions: Sequence[SessionEvidence], directory: Path
) -> list[dict[str, Any]]:
    return [session.record(directory) for session in sessions]


def shard_provenance(sessions: Sequence[SessionEvidence]) -> str:
    """Say who authored a shard's own ``results.qpa`` and ``summary.txt``.

    Only a lone guest session that finished its whole caselist owns them
    directly.  As soon as the host has to record a terminated case, or a
    second guest contributes results, the shard-level artifacts are a host
    merge and must say so everywhere they are read.
    """
    if not sessions:
        raise CampaignError("a shard needs at least one guest session")
    if len(sessions) == 1:
        return "single-session-guest"
    return "host-merged"


def validate_session_provenance(
    directory: Path,
    metadata: dict[str, Any],
    artifacts: dict[str, bytes],
    expected_count: int,
) -> None:
    """Re-derive a multi-session shard from the evidence each guest sent.

    The merged QPA and summary are recomputed from the per-session artifacts
    and must match byte for byte, so a host-merged shard is reparseable and
    cannot silently gain or lose a result.
    """
    records = metadata.get("sessions")
    provenance = metadata.get("artifacts_provenance")
    if not isinstance(records, list) or not records:
        raise CampaignError("shard evidence omits its guest session records")
    bound = metadata.get("max_resumptions")
    if type(bound) is not int or bound < 0:
        raise CampaignError("shard evidence omits its resumption bound")
    if len(records) - 1 > bound:
        raise CampaignError("shard exceeded its recorded resumption bound")
    if metadata.get("session_count") != len(records):
        raise CampaignError("shard session count disagrees with its records")
    if provenance not in {"single-session-guest", "host-merged"}:
        raise CampaignError("shard evidence has an invalid artifact provenance")
    rebuilt: list[SessionEvidence] = []
    for index, record in enumerate(records):
        if not isinstance(record, dict) or record.get("index") != index:
            raise CampaignError("shard session records are not ordered")
        if record.get("outcome") not in SESSION_OUTCOMES:
            raise CampaignError("shard session record has an invalid outcome")
        if record.get("outcome") == "completed" and index != len(records) - 1:
            raise CampaignError("only the final shard session may be complete")
        console = directory / str(record.get("console"))
        transport = directory / str(record.get("artifact_transport"))
        session_artifacts = directory / str(record.get("artifacts_directory"))
        for path, digest in (
            (console, record.get("console_sha256")),
            (transport, record.get("artifact_transport_sha256")),
        ):
            if not path.is_file() or sha256_file(path) != digest:
                raise CampaignError(f"shard session evidence changed: {path.name}")
        if record["outcome"] != "completed" and DIRTY_DRM_SHUTDOWN in (
            console.read_text(encoding="utf-8", errors="replace")
        ):
            raise CampaignError(
                "Virtio-GPU memory manager was not clean during guest shutdown"
            )
        stored = {
            path.name: path.read_bytes()
            for path in sorted(session_artifacts.iterdir())
        }
        if {
            name: hashlib.sha256(payload).hexdigest()
            for name, payload in stored.items()
        } != record.get("artifact_sha256"):
            raise CampaignError("shard session artifact digests changed")
        if decode_artifacts(
            transport.read_text(encoding="ascii"),
            partial=record["outcome"] != "completed",
        ) != stored:
            raise CampaignError(
                "shard session transport differs from its decoded artifacts"
            )
        rebuilt.append(
            SessionEvidence(
                index=index,
                outcome=str(record["outcome"]),
                resume_after=int(record.get("resume_after", 0)),
                qpa_text=decode_qpa(stored.get(QPA_ARTIFACT, b"")),
                completed_cases=(),
                console_case=record.get("console_case"),
                console=str(record["console"]),
                artifact_transport=str(record["artifact_transport"]),
                artifacts_directory=str(record["artifacts_directory"]),
                return_code=record.get("return_code"),
                shutdown_stall=bool(record.get("shutdown_stall", False)),
            )
        )
    # An aborted session is never a result, so a shard can only end on a guest
    # that finished its caselist: anything else was retried or gave up.
    if rebuilt[-1].outcome != "completed":
        raise CampaignError(
            "a shard must end on a session that completed its caselist"
        )
    if provenance != shard_provenance(rebuilt):
        raise CampaignError("shard artifact provenance contradicts its sessions")
    if provenance == "single-session-guest":
        return
    recomputed = merged_shard_artifacts(rebuilt, directory, expected_count)
    if recomputed != artifacts:
        raise CampaignError("host-merged shard artifacts are not reproducible")


def git_state() -> dict[str, Any]:
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    dirty = subprocess.run(
        ["git", "status", "--porcelain=v1"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    return {"revision": revision, "dirty_paths": dirty}


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def egl_configs_from_json(value: Any) -> tuple[EglConfig, ...]:
    if not isinstance(value, list) or not value:
        raise CampaignError("config-inventory evidence has no EGL configs")
    configs: list[EglConfig] = []
    seen: set[int] = set()
    for row in value:
        if not isinstance(row, dict) or set(row) != {
            "id",
            "surface_types",
            "selected_surface_type",
        }:
            raise CampaignError("config-inventory evidence has an invalid row")
        identifier = row["id"]
        surfaces = row["surface_types"]
        if (
            type(identifier) is not int
            or identifier < 1
            or identifier in seen
            or not isinstance(surfaces, list)
            or not surfaces
            or any(
                surface not in {"window", "pbuffer", "pixmap"}
                for surface in surfaces
            )
            or len(surfaces) != len(set(surfaces))
        ):
            raise CampaignError("config-inventory evidence has an invalid row")
        config = EglConfig(identifier, tuple(surfaces))
        if row["selected_surface_type"] != config.selected_surface_type:
            raise CampaignError(
                "config-inventory evidence changed its selected surface type"
            )
        seen.add(identifier)
        configs.append(config)
    return tuple(configs)


def load_config_inventory_evidence(
    path: Path,
    api: str,
    page_profile: str,
    display: str,
    rootfs_contract: RootfsContract,
    runtime_identity: dict[str, str] | None = None,
    surface: tuple[int, int] = (1280, 720),
    renderer: str = "virgl",
) -> tuple[tuple[EglConfig, ...], str, list[dict[str, Any]] | None]:
    try:
        metadata = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise CampaignError(
            f"cannot read config-inventory evidence {path}: {error}"
        ) from error
    if not isinstance(metadata, dict) or metadata.get("schema_version") != 1:
        raise CampaignError("unsupported config-inventory evidence")
    expected = {
        "adapter": "macos",
        "backend": host_backend_label(renderer),
        "mode": "config-inventory",
        "api": api,
        "page_profile": page_profile,
        "display": display,
        "rootfs_type": rootfs_contract.filesystem_type,
        "rootfs_sha256": rootfs_contract.sha256,
        "glcts_sha256": rootfs_contract.glcts_sha256,
        "emulator_return_code": 0,
        "completed_cases": len(CONFIG_INVENTORY_SCOPES[api]),
        "status_totals": {"Pass": len(CONFIG_INVENTORY_SCOPES[api])},
    }
    if len(CONFIG_INVENTORY_SCOPES[api]) > 1:
        expected["config_scope"] = CONFIG_INVENTORY_SCOPE_NAMES[api]
        expected["config_cases"] = list(CONFIG_INVENTORY_SCOPES[api])
    if api in XML_MATRIX_APIS:
        if runtime_identity is None or set(runtime_identity) != {
            "emulator_sha256", "kosmickrisp_sha256",
        }:
            raise CampaignError("GL config inventory requires the exact runtime identity")
        expected.update(
            {"renderer": renderer, "surface": list(surface), **runtime_identity}
        )
    for key, value in expected.items():
        if metadata.get(key) != value:
            raise CampaignError(
                f"config-inventory evidence does not match {key}"
            )
    configs = egl_configs_from_json(metadata.get("egl_configs"))
    digest = metadata.get("egl_config_sha256")
    if (
        not isinstance(digest, str)
        or SHA256.fullmatch(digest) is None
        or digest != egl_config_digest(configs)
    ):
        raise CampaignError("config-inventory evidence digest changed")
    qpa_digest = metadata.get("qpa_sha256")
    if not isinstance(qpa_digest, str) or SHA256.fullmatch(qpa_digest) is None:
        raise CampaignError("config-inventory QPA identity is invalid")
    qpa_path = path.parent / "artifacts" / "results.qpa"
    try:
        qpa_payload = qpa_path.read_bytes()
    except OSError as error:
        raise CampaignError(
            f"cannot read config-inventory QPA {qpa_path}: {error}"
        ) from error
    if hashlib.sha256(qpa_payload).hexdigest() != qpa_digest:
        raise CampaignError("config-inventory QPA identity changed")
    qpa_text = decode_qpa(qpa_payload)
    parsed = parse_egl_config_qpa(qpa_text, api)
    if parsed != configs:
        raise CampaignError("config-inventory QPA and metadata disagree")
    attributes: list[dict[str, Any]] | None = None
    if EGL_ATTRIBUTE_CASE in CONFIG_INVENTORY_SCOPES[api]:
        attributes = parse_egl_config_attributes(qpa_text)
        if metadata.get("egl_config_attributes") != attributes:
            raise CampaignError(
                "config-inventory EGL attributes and QPA disagree"
            )
        attribute_digest = metadata.get("egl_config_attributes_sha256")
        if (
            not isinstance(attribute_digest, str)
            or SHA256.fullmatch(attribute_digest) is None
            or attribute_digest != egl_attribute_digest(attributes)
        ):
            raise CampaignError("config-inventory EGL attribute digest changed")
    if api in XML_MATRIX_APIS:
        directory = path.parent
        console = directory / "console.log"
        if sha256_file(console) != metadata.get("console_sha256"):
            raise CampaignError("config-inventory console identity changed")
        validate_guest_shutdown(console.read_text(encoding="utf-8"))
        serial = directory / ARTIFACT_TRANSPORT_LOG
        if sha256_file(serial) != metadata.get("artifact_transport_sha256"):
            raise CampaignError("config-inventory transport identity changed")
        artifacts = decode_artifacts(serial.read_text(encoding="ascii"))
        if set(artifacts) != CONFIG_INVENTORY_ARTIFACTS:
            raise CampaignError("config-inventory artifact set changed")
        if any((directory / "artifacts" / name).read_bytes() != payload
               for name, payload in artifacts.items()):
            raise CampaignError("config-inventory transport and artifacts disagree")
        if artifacts["summary.txt"].decode("utf-8").strip() != "cts_rc=0":
            raise CampaignError("config-inventory CTS process failed")
        validate_renderer_probe(
            artifacts["renderer.txt"].decode("utf-8"),
            renderer,
            host_backend_label(renderer),
        )
    return configs, digest, attributes


def opengl_matrix_run(
    receipt: dict[str, Any], configs: tuple[EglConfig, ...], digest: str,
    config_index: int, configuration_index: int, fraction: tuple[int, int],
    exact_cases: tuple[str, ...] = (),
) -> MatrixRun:
    if receipt.get("schema_version") != 4:
        raise CampaignError("GL matrix requires the XML-scoped schema-4 receipt")
    manifest = receipt["opengl_cts"]["mustpass"]
    try:
        runs = opengl_cts_plan.expand_runs(manifest, [
            {"id": config.identifier, "surface_types": list(config.surface_types)}
            for config in configs
        ])
    except opengl_cts_plan.PlanError as error:
        raise CampaignError(str(error)) from error
    matching = [run for run in runs if run["egl_config_index"] == config_index
                and run["configuration_index"] == configuration_index]
    if len(matching) != 1:
        raise CampaignError("GL XML configuration does not apply to this EGL config")
    row = manifest["configurations"][configuration_index]
    identity = next(leaf for leaf in manifest["leaves"] if leaf["id"] == row["leaf_id"])
    leaf = LeafIdentity(identity["id"], identity["path"],
                        identity["case_count"], identity["sha256"])
    document = next(item for item in manifest["documents"]
                    if item["collection"] == row["collection"])
    return MatrixRun(
        configuration=MustpassConfiguration(
            api="gl46", leaf=leaf, name=row["name"],
            use_for_first_egl_config=row["use_for_first_egl_config"],
            screen_rotation="unspecified", surface_width=row["surface_width"],
            surface_height=row["surface_height"], base_seed=row["base_seed"],
            watchdog="disable", fbo_config=row["fbo_config"],
            xml_arguments=tuple(row["xml_arguments"]),
        ),
        egl_config=configs[config_index], egl_config_index=config_index,
        config_inventory_sha256=digest,
        selection=Selection(leaf, exact_cases, *fraction),
        mustpass_xml_path=document["path"], mustpass_xml_size=document["size"],
        mustpass_xml_sha256=document["sha256"],
        opengl_configuration_index=configuration_index,
    )


def load_gles32_matrix(
    identity: dict[str, Any] | None = None,
    path: Path = GLES32_MATRIX_PATH,
) -> dict[str, Any]:
    """Build the ES 3.2 must-pass manifest from the repository's pinned XML.

    The ES caselists live only in the guest image, so their identities are
    pinned here and re-checked inside the guest before a shard runs. Every
    leaf this document shares with the CTS identity must agree with it.
    """
    if identity is None:
        identity = load_identity()
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CampaignError(f"cannot read ES 3.2 matrix {path}: {error}") from error
    if (
        not isinstance(value, dict)
        or set(value) != {
            "schema_version", "api", "captured_from_rootfs", "documents", "leaves",
        }
        or value["schema_version"] != 1
        or value["api"] != gles_cts_plan.API
        or not isinstance(value["captured_from_rootfs"], str)
        or not value["captured_from_rootfs"]
    ):
        raise CampaignError("invalid ES 3.2 matrix document")
    rows = value["documents"]
    if (
        not isinstance(rows, list)
        or len(rows) != len(gles_cts_plan.COLLECTIONS)
        or any(
            not isinstance(row, dict)
            or set(row) != {"collection", "file", "path"}
            or row["collection"] != collection
            or Path(row["file"]).name != row["file"]
            for row, (collection, *_) in zip(rows, gles_cts_plan.COLLECTIONS)
        )
    ):
        raise CampaignError("invalid ES 3.2 matrix document set")
    documents: dict[str, bytes] = {}
    for row in rows:
        document_path = path.with_name(row["file"])
        try:
            documents[row["collection"]] = document_path.read_bytes()
        except OSError as error:
            raise CampaignError(
                f"cannot read ES 3.2 must-pass XML {document_path}: {error}"
            ) from error
    known = leaf_identities(identity)
    by_path = {leaf.path: leaf for leaf in known.values()}
    leaves: dict[str, dict[str, Any]] = {}
    for leaf in value["leaves"]:
        if (
            not isinstance(leaf, dict)
            or set(leaf) != gles_cts_plan.LEAF_KEYS
            or not isinstance(leaf["path"], str)
            or leaf["path"] in leaves
            or type(leaf["case_count"]) is not int
            or leaf["case_count"] < 1
            or type(leaf["size"]) is not int
            or leaf["size"] < leaf["case_count"]
            or not isinstance(leaf["sha256"], str)
            or SHA256.fullmatch(leaf["sha256"]) is None
            or Path(leaf["path"]).name != f"{leaf['id']}.txt"
        ):
            raise CampaignError("invalid ES 3.2 matrix leaf identity")
        shared = by_path.get(leaf["path"])
        if shared is not None and (
            shared.case_count != leaf["case_count"]
            or shared.sha256 != leaf["sha256"]
        ):
            raise CampaignError(
                f"ES 3.2 matrix leaf disagrees with the CTS identity: {leaf['id']}"
            )
        leaves[leaf["path"]] = leaf
    xml_identity = identity["mustpass"]["gles_xml"]
    main = documents["main"]
    if (
        len(main) != xml_identity["size"]
        or hashlib.sha256(main).hexdigest() != xml_identity["sha256"]
    ):
        raise CampaignError("ES 3.2 main must-pass XML differs from the CTS identity")
    try:
        manifest = gles_cts_plan.build_manifest(
            documents.__getitem__, leaves.__getitem__
        )
    except (KeyError, gles_cts_plan.PlanError) as error:
        raise CampaignError(f"invalid ES 3.2 must-pass scope: {error}") from error
    if next(row["path"] for row in manifest["documents"]
            if row["collection"] == "main") != xml_identity["path"]:
        raise CampaignError("ES 3.2 main must-pass XML path changed")
    return manifest


def gles32_matrix_run(
    manifest: dict[str, Any], receipt: dict[str, Any],
    configs: tuple[EglConfig, ...], digest: str, config_index: int,
    configuration_index: int, fraction: tuple[int, int],
    exact_cases: tuple[str, ...] = (),
    attributes: list[dict[str, Any]] | None = None,
) -> MatrixRun:
    if receipt.get("schema_version") != 4:
        raise CampaignError("ES 3.2 matrix requires the XML-scoped schema-4 receipt")
    try:
        runs = gles_cts_plan.expand_runs(manifest, [
            {"id": config.identifier, "surface_types": list(config.surface_types)}
            for config in configs
        ], attributes)
    except gles_cts_plan.PlanError as error:
        raise CampaignError(str(error)) from error
    matching = [run for run in runs if run["egl_config_index"] == config_index
                and run["configuration_index"] == configuration_index]
    if len(matching) != 1:
        raise CampaignError("ES 3.2 XML configuration does not apply to this EGL config")
    run = matching[0]
    row = manifest["configurations"][configuration_index]
    identity = next(leaf for leaf in manifest["leaves"] if leaf["id"] == row["leaf_id"])
    leaf = LeafIdentity(identity["id"], identity["path"],
                        identity["case_count"], identity["sha256"])
    document = next(item for item in manifest["documents"]
                    if item["collection"] == row["collection"])
    egl_config = configs[config_index]
    if run["config_scope"] == "aosp":
        # An AOSP record resolves its own configuration out of aospConfigs,
        # which also holds the multisample configurations the conformant list
        # excludes, so its EGL configuration is not configs[config_index].
        entry = gles_cts_plan.aosp_config_list(attributes)[run["egl_config_index"]]
        egl_config = EglConfig(entry["id"], tuple(entry["surface_types"]))
    return MatrixRun(
        configuration=MustpassConfiguration(
            api=gles_cts_plan.API, leaf=leaf, name=row["name"],
            use_for_first_egl_config=row["use_for_first_egl_config"],
            screen_rotation="unspecified", surface_width=row["surface_width"],
            surface_height=row["surface_height"], base_seed=row["base_seed"],
            watchdog="disable", fbo_config=row["fbo_config"],
            xml_arguments=tuple(row["xml_arguments"]),
            aosp_config=row["aosp_config"],
            aosp_surface_type=(None if run["surface_type"] == "none"
                               else run["surface_type"]),
        ),
        egl_config=egl_config, egl_config_index=config_index,
        config_inventory_sha256=digest,
        selection=Selection(leaf, exact_cases, *fraction),
        mustpass_xml_path=document["path"], mustpass_xml_size=document["size"],
        mustpass_xml_sha256=document["sha256"],
        opengl_configuration_index=configuration_index,
    )


def matrix_metadata(options: argparse.Namespace) -> dict[str, Any]:
    matrix = options.matrix_run
    return {
        "mustpass_xml_path": matrix.mustpass_xml_path,
        "mustpass_xml_size": matrix.mustpass_xml_size,
        "mustpass_xml_sha256": matrix.mustpass_xml_sha256,
        "runner_oracle_path": options.runner_oracle.path,
        "runner_oracle_size": options.runner_oracle.size,
        "runner_oracle_sha256": options.runner_oracle.sha256,
        "successful_case_statuses": list(options.runner_oracle.successful_case_statuses),
        "configuration_name": matrix.configuration.name,
        "configuration_role": "first" if matrix.configuration.use_for_first_egl_config else "other",
        "xml_arguments": list(matrix.configuration.xml_arguments),
        "waivers": [
            {"id": waiver.identifier, "url": waiver.url, "cases": list(waiver.cases)}
            for waiver in options.applied_waivers
        ],
        "waiver_list_sha256": (
            hashlib.sha256(waiver_xml(options.applied_waivers).encode("utf-8")).hexdigest()
            if options.applied_waivers else None
        ),
        "runner_arguments": matrix_runner_arguments(
            matrix, "/run/uml-cts/selected.txt",
            GUEST_WAIVER_PATH if options.applied_waivers else None),
        "egl_config_index": matrix.egl_config_index,
        "egl_config_id": matrix.egl_config.identifier,
        "egl_surface_types": list(matrix.egl_config.surface_types),
        "egl_surface_type": (
            matrix_draw_surface_type(matrix)
            if matrix.configuration.aosp_config is not None
            else matrix.egl_config.selected_surface_type
        ),
        **({"aosp_gl_config": matrix.configuration.aosp_config}
           if matrix.configuration.aosp_config is not None else {}),
        "config_inventory_sha256": matrix.config_inventory_sha256,
        "config_inventory_run": str(options.config_inventory_run.resolve()),
        **({"opengl_configuration_index": matrix.opengl_configuration_index,
            "draw_surface_type": matrix_draw_surface_type(matrix)}
           if matrix.opengl_configuration_index is not None else {}),
    }


def api_for_leaf(identifier: str) -> str:
    return identifier.split("-", 1)[0]


HELP_EPILOG = """\
OpenGL lanes. --api gl46 --leaf gl46 --case runs core KHR-GL46.* cases;
--case repeats to run a focused set in one guest boot, in CTS traversal order.
--matrix gl46 runs one official XML-derived must-pass configuration by
--mustpass-config-index and --fraction. Adding --case narrows that shard to an
ordered subset of its own case list; the run records itself as a case subset
in its mode and artifact directory, and no campaign or matrix aggregate counts
it as the shard it was cut from. The ES-for-GL leaves (dEQP-GL45-ES3.*,
dEQP-GL45-ES31.*, KHR-Single-GL46.*) exist only as matrix shards. Seeds,
dimensions and FBO settings come from the XML records, and an ordered
CTS-Configs.gl46 inventory captured from the exact emulator, image and
provider (--config-inventory gl46, replayed with --config-inventory-run)
assigns the first and other EGL configuration roles.

--matrix gles32 runs the OpenGL ES 3.2 must-pass matrix on the EGL
configurations its own ordered inventory reports (--config-inventory gles32);
a GL46 inventory never stands in for it. That inventory, scope
configs-egl-attributes, runs CTS-Configs.es32 and dEQP-EGL.info.configs in one
guest session and records the conformant list and the EGL attribute table,
each with its own digest. Its runs come from the five ES must-pass documents
pinned beside this runner, with the caselist identities in
vk_gl_cts_gles32_matrix.json, and the guest rechecks those digests before a
shard runs. As in the pinned runner's getTestRunsForES, the NoContext and
Khronos ES collections admit every API an ES 3.2 context supports and Single
Config only KHR-Single-GLES32; NoContext and Single Config run once, on the
first configuration. The AOSP collections (dEQP-EGL, dEQP-GLES2/3/31) name a
GL configuration such as rgba8888d24s8ms0 instead of an EGL id: the plan
rebuilds ConfigList::aospConfigs from the attribute table, resolves each name
as the runner does, and refuses the plan unless that rebuild reproduces the
inventory's conformant list. An AOSP run names its GL configuration and,
outside the EGL run, its resolved surface type, never a seed or an EGL id.
These collections come last in the plan, so adding them renumbered no run;
plan order is not the runner's emission order.

Display. X11 surfaces use native Xorg with virtio DRM modesetting and
Virgl-backed glamor. The guest checks the server, its DRM descriptor, DRI3,
Present, a connected output and accelerated rendering, and turns screen
blanking off: a CTS guest gets no input, and a blanked screen drops Present to
a one-second fake vblank. Xwayland and Xvfb are rejected. Wayland selections
run the image's unmodified KWin nested on Xorg without a lock screen, because a
locked compositor owes an occluded surface no frame callback. Display
processes stop before guest shutdown. These guests boot uml-cts.target, which
conflicts with the display manager, the fixed-VT getties and the container
runtime: a short shard that powers off while Docker starts leaves kworkers
blocked in bpf_prog_free_deferred and never reaches poweroff.

Guest sessions, shared with run_vk_cts.py. The guest transports its growing
QPA as ordered checkpoint segments, so a killed guest still leaves the prefix
it produced. A segment counts only once its compressed stream has been
measured back and acknowledged, summary.txt is sent last, and a zero-byte
artifact is refused. The host ends a session whose console and artifact
transport make no progress for --console-stall-timeout seconds; RCU stall,
hung-task and soft-lockup reports do not count as progress, because a wedged
kernel reprints them on its own timer. Inside an OpenGL guest a case that
writes no QPA output while glcts burns no CPU is abandoned at the same bound,
and one that is silent but computing, such as software fp64, at
--console-stall-busy-timeout. An emulator exit aborts the session without
inventing a status for the case in flight: a fresh guest resumes at the same
index, and a shard that cannot get past a case exhausts --max-resumptions and
fails, naming it.
"""


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    identity_parser = argparse.ArgumentParser(add_help=False)
    identity_parser.add_argument(
        "--cts-identity", type=Path, default=IDENTITY_PATH
    )
    identity_options, _ = identity_parser.parse_known_args(arguments)
    identity = load_identity(identity_options.cts_identity.resolve())
    leaves = leaf_identities(identity)
    campaigns = load_apple_campaigns(identity)
    mustpass = load_gles_mustpass(identity)
    parser = argparse.ArgumentParser(
        description="run a bounded in-image VK-GL-CTS shard",
        epilog=HELP_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--cts-identity", type=Path, default=IDENTITY_PATH
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--inventory",
        action="store_true",
        help="capture the complete display-backed GLES 2/3 text caselist",
    )
    mode.add_argument(
        "--config-inventory",
        choices=tuple(CONFIG_CASES),
        help="enumerate the ordered conformant EGL configs for one API",
    )
    mode.add_argument(
        "--matrix",
        choices=tuple(CONFIG_CASES),
        help="run one exact official must-pass matrix shard",
    )
    mode.add_argument("--campaign", choices=sorted(campaigns))
    parser.add_argument("--leaf", choices=sorted(leaves))
    parser.add_argument(
        "--api",
        choices=sorted({api_for_leaf(identifier) for identifier in leaves}),
    )
    parser.add_argument("--context", choices=("egl",), default="egl")
    # A developer shard still picks cases or a fraction, never both, but an
    # official matrix shard takes its fraction and then narrows it to named
    # cases, so the pair is rejected where it does not belong instead of by
    # the parser.
    parser.add_argument(
        "--case",
        type=exact_case,
        action="append",
        help="exact CTS case, repeatable in CTS traversal order",
    )
    parser.add_argument("--fraction", type=parse_fraction)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--config-inventory-run", type=Path)
    parser.add_argument("--egl-config-index", type=nonnegative_integer)
    parser.add_argument("--mustpass-config-index", type=nonnegative_integer,
                        help="authenticated must-pass XML configuration index")
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--display", choices=("vnc", "native"), default="vnc")
    parser.add_argument(
        "--renderer",
        choices=("virgl", "zink"),
        default="virgl",
        help="guest Gallium renderer to require (default: virgl)",
    )
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=positive_integer, default=10)
    parser.add_argument("--width", type=positive_integer, default=1280)
    parser.add_argument("--height", type=positive_integer, default=720)
    parser.add_argument("--shard-timeout", type=positive_integer)
    parser.add_argument("--guest-timeout", type=positive_integer)
    parser.add_argument(
        "--console-stall-timeout",
        type=int,
        default=0,
        help="guest console/transport progress bound in seconds (0 disables)",
    )
    parser.add_argument(
        "--console-stall-busy-timeout",
        type=int,
        default=0,
        help="bound in seconds for a case that produces no QPA output but keeps "
             "the guest CTS process busy (0 cuts it at the stall timeout)",
    )
    parser.add_argument(
        "--max-resumptions",
        type=int,
        default=DEFAULT_MAX_RESUMPTIONS,
        help=(
            "fresh guests a stalled or aborted shard may continue in "
            f"(default: {DEFAULT_MAX_RESUMPTIONS}; 0 keeps one session)"
        ),
    )
    parser.add_argument("--emulator", type=Path)
    parser.add_argument("--rootfs", type=Path, default=DEFAULT_ROOTFS)
    parser.add_argument(
        "--rootfs-type",
        choices=("erofs", "ext4"),
        default="erofs",
        help="authenticated lower filesystem type (default: erofs)",
    )
    parser.add_argument(
        "--rootfs-receipt",
        type=Path,
        help="required identity receipt for a derivative ext4 rootfs",
    )
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument(
        "--qualification",
        action="store_true",
        help="qualify an authenticated GLES matrix or Zink/OpenGL leaf (not formal conformance)",
    )
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args(arguments)
    if options.console_stall_timeout < 0:
        parser.error("--console-stall-timeout must not be negative")
    if options.console_stall_busy_timeout < 0 or (
        options.console_stall_busy_timeout
        and options.console_stall_busy_timeout < options.console_stall_timeout
    ):
        parser.error(
            "--console-stall-busy-timeout must be zero or at least "
            "--console-stall-timeout"
        )
    if options.max_resumptions < 0:
        parser.error("--max-resumptions must not be negative")
    matrix_only_arguments = (
        options.config_inventory_run is not None
        or options.egl_config_index is not None
        or options.mustpass_config_index is not None
    )
    if options.inventory:
        if (
            options.campaign is not None
            or options.leaf is not None
            or options.api is not None
        ):
            parser.error("--inventory does not accept a campaign, leaf, or API")
        if options.case is not None or options.fraction is not None:
            parser.error("--inventory does not accept a case or fraction")
        if options.qualification:
            parser.error("inventory capture is diagnostic, not qualification")
        if matrix_only_arguments or options.seed is not None:
            parser.error("--inventory does not accept matrix or seed options")
    elif options.config_inventory is not None:
        if options.leaf is not None or options.api is not None:
            parser.error("--config-inventory does not accept a leaf or --api")
        if options.case is not None or options.fraction is not None:
            parser.error("--config-inventory does not accept a case or fraction")
        if matrix_only_arguments or options.seed is not None:
            parser.error(
                "--config-inventory does not accept matrix or seed options"
            )
        if options.qualification:
            parser.error(
                "config discovery is an input to, not itself, qualification"
            )
    elif options.matrix is not None:
        if options.leaf is not None or options.api is not None:
            parser.error("--matrix owns its exact leaf and API")
        if options.case is not None and len(set(options.case)) != len(options.case):
            parser.error("--case must not repeat a case")
        if options.seed is not None:
            parser.error("--matrix uses the seed from mustpass.xml")
        if options.config_inventory_run is None or options.egl_config_index is None:
            parser.error(
                "--matrix requires --config-inventory-run and "
                "--egl-config-index"
            )
        if not options.qualification:
            parser.error("--matrix requires --qualification")
        if options.display != "vnc":
            parser.error("a macOS qualification matrix requires --display vnc")
        if (options.matrix in XML_MATRIX_APIS) != (
            options.mustpass_config_index is not None
        ):
            parser.error(
                "only an XML-scoped matrix requires --mustpass-config-index: "
                + ", ".join(XML_MATRIX_APIS)
            )
    elif options.campaign is not None:
        if (
            options.leaf is not None
            or options.api is not None
            or options.case is not None
            or options.fraction is not None
        ):
            parser.error(
                "--campaign does not accept a leaf, API, case, or fraction"
            )
        if options.seed is not None or options.shard_timeout is not None:
            parser.error("--campaign owns its seed and shard timeout")
        if matrix_only_arguments:
            parser.error("--campaign does not accept matrix options")
        campaign = campaigns[options.campaign]
        options.seed = campaign.seed
        options.shard_timeout = campaign.shard_timeout
    else:
        if options.leaf is None or options.api is None:
            parser.error("a developer shard requires --leaf and --api")
        if api_for_leaf(options.leaf) != options.api:
            parser.error(
                f"--leaf {options.leaf} does not belong to --api {options.api}"
            )
        if matrix_only_arguments:
            parser.error("a developer shard does not accept matrix options")
        if options.case is not None and options.fraction is not None:
            parser.error("a developer shard takes --case or --fraction, not both")
        if options.case is not None and len(set(options.case)) != len(options.case):
            parser.error("--case must not repeat a case")
    if options.seed is None:
        options.seed = 0
    if options.shard_timeout is None:
        options.shard_timeout = 1800 if options.matrix is not None else 900
    if options.guest_timeout is None:
        options.guest_timeout = options.shard_timeout + 600
    if options.fraction is None:
        options.fraction = (0, 1)
    if options.seed < 0 or options.seed > 0xFFFFFFFF:
        parser.error("--seed must be between 0 and 4294967295")
    if (
        options.qualification
        and options.matrix is None
        and options.renderer != "zink"
        and not identity["cts"]["qualification_ready"]
    ):
        parser.error(identity["cts"]["qualification_blocker"])
    if options.emulator is None:
        options.emulator = DEFAULT_EMULATOR
    try:
        receipt, contract = select_rootfs_contract(
            identity, options.rootfs_type, options.rootfs_receipt
        )
    except CampaignError as error:
        parser.error(str(error))
    options.derivative_receipt = receipt
    options.rootfs_contract = contract
    # The GL46 scope is bound to the combined Vulkan/OpenGL derivative image
    # and its receipt, whichever stack renders it: guest Zink over Venus, or
    # the guest Virgl driver over the host's Zink provider. A developer shard
    # of the gl46 leaf is in that scope too; it asks the guest for the same
    # desktop OpenGL context an official matrix does. The ES 3.2 matrix runs
    # from the same authenticated image and shares the scope.
    options.gl46_scope = (
        options.renderer == "zink"
        or options.matrix in XML_MATRIX_APIS
        or options.config_inventory in XML_MATRIX_APIS
        or options.api == "gl46"
    )
    if options.gl46_scope and (
        options.rootfs_type != "ext4"
        or receipt is None
        or receipt.get("artifact") != "uml-vulkan-zink-gl-rootfs"
    ):
        parser.error(
            "the GL46 scope requires the authenticated combined "
            "Vulkan/OpenGL ext4 rootfs"
        )
    if options.gl46_scope:
        if options.renderer == "zink" and (
            options.inventory
            or options.config_inventory not in {None, "gl46"}
            or options.matrix not in {None, "gl46"}
            or options.campaign is not None
        ):
            parser.error("Zink accepts only the receipt-bound GL46 scope")
        receipt_leaves = receipt["opengl_cts"]["mustpass"]["leaves"]
        leaves = {
            row["id"]: LeafIdentity(
                row["id"], row["path"], row["case_count"], row["sha256"]
            )
            for row in receipt_leaves
        }
        if (options.renderer == "zink" and options.matrix is None
                and options.config_inventory is None
                and options.leaf != "gl46"):
            parser.error(
                "Zink developer shards require the active gl46 core leaf"
            )
    options.matrix_run = None
    options.runner_oracle = None
    if options.matrix is not None:
        if contract.filesystem_type != "ext4" or receipt is None:
            parser.error(
                "an official matrix requires the authenticated derivative ext4 rootfs"
            )
        try:
            configs, config_digest, config_attributes = load_config_inventory_evidence(
                options.config_inventory_run.resolve(),
                options.matrix,
                "4k",
                options.display,
                contract,
                ({"emulator_sha256": sha256_file(options.emulator.resolve()),
                  "kosmickrisp_sha256": verify_staged_kosmickrisp(options.emulator.resolve())}
                 if options.matrix in XML_MATRIX_APIS else None),
                (options.width, options.height),
                options.renderer,
            )
            if options.matrix in XML_MATRIX_APIS:
                if options.matrix == "gl46":
                    options.matrix_run = opengl_matrix_run(
                        receipt, configs, config_digest, options.egl_config_index,
                        options.mustpass_config_index, options.fraction,
                        tuple(options.case) if options.case else (),
                    )
                else:
                    options.matrix_run = gles32_matrix_run(
                        load_gles32_matrix(identity), receipt, configs,
                        config_digest, options.egl_config_index,
                        options.mustpass_config_index, options.fraction,
                        tuple(options.case) if options.case else (),
                        config_attributes,
                    )
                source = receipt["opengl_cts"]["mustpass"]["runner_source"]
                if (
                    options.matrix == gles_cts_plan.API
                    and source != gles_cts_plan.RUNNER_SOURCE
                ):
                    parser.error(
                        "the ES 3.2 run table and the sealed runner source disagree"
                    )
                options.runner_oracle = RunnerOracle(
                    source["path"], source["size"], source["sha256"], (),
                    tuple(receipt["runner_oracle"]["successful"]),
                )
            else:
                options.runner_oracle = load_runner_oracle(identity, mustpass)
        except (CampaignError, UnicodeDecodeError) as error:
            parser.error(str(error))
        if options.egl_config_index >= len(configs):
            parser.error(
                f"--egl-config-index exceeds the {len(configs)} discovered configs"
            )
        if options.matrix_run is None:
            fraction_index, fraction_count = options.fraction
            leaf = leaves[options.matrix]
            selected = Selection(
                leaf, tuple(options.case) if options.case else (),
                fraction_index, fraction_count,
            )
            xml_identity = identity["mustpass"]["gles_xml"]
            options.matrix_run = MatrixRun(
                configuration=baseline_mustpass_configuration(
                    mustpass, options.matrix, options.egl_config_index == 0
                ),
                egl_config=configs[options.egl_config_index],
                egl_config_index=options.egl_config_index,
                config_inventory_sha256=config_digest,
                selection=selected,
                mustpass_xml_path=xml_identity["path"],
                mustpass_xml_size=xml_identity["size"],
                mustpass_xml_sha256=xml_identity["sha256"],
            )
    options.waivers = ()
    options.applied_waivers = ()
    if options.matrix_run is not None:
        try:
            options.waivers = load_waivers()
        except CampaignError as error:
            parser.error(str(error))
        options.applied_waivers = applicable_waivers(
            options.matrix_run, options.waivers
        )
    options.identity = identity
    options.leaves = leaves
    options.campaigns = campaigns
    options.mustpass = mustpass
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    fraction_index, fraction_count = options.fraction
    leaf: LeafIdentity | None = None
    selection: Selection | None = None
    suffix = "inventory"
    if options.config_inventory is not None:
        api_identifier = options.config_inventory
        suffix = f"config-inventory-{api_identifier}"
    elif options.matrix is not None:
        matrix = options.matrix_run
        leaf = matrix.configuration.leaf
        selection = matrix.selection
        api_identifier = matrix.configuration.api
        leaf_identifier = leaf.identifier
        row = (f"xml{matrix.opengl_configuration_index:02d}-"
               if matrix.opengl_configuration_index is not None else "")
        suffix = (f"matrix-{api_identifier}-cfg{matrix.egl_config_index:02d}-"
                  f"{row}{fraction_index:03d}of{fraction_count:03d}")
        if selection.exact_cases:
            # A subset of a shard is not that shard, and its directory says so
            # before anything reads its metadata.
            suffix += f"-cases{len(selection.exact_cases):03d}"
    elif options.campaign is not None:
        campaign = options.campaigns[options.campaign]
        leaf = campaign.leaf
        selection = campaign.selection
        api_identifier = campaign.api
        leaf_identifier = campaign.leaf.identifier
        suffix = campaign.identifier
    elif not options.inventory:
        leaf = options.leaves[options.leaf]
        exact_cases = tuple(options.case) if options.case is not None else ()
        selection = Selection(
            leaf, exact_cases, fraction_index, fraction_count
        )
        api_identifier = options.api
        leaf_identifier = options.leaf
        suffix = options.leaf
    if selection is not None:
        if selection.expected_count < 1:
            raise CampaignError("selected fraction contains no cases")

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifact_directory = options.artifact_root.resolve() / (
        f"{timestamp}-{os.getpid()}-4k-{suffix}"
    )
    command = build_harness_command(options, selection, artifact_directory)
    if options.dry_run:
        print(shlex.join(command))
        return 0

    emulator = options.emulator.resolve()
    rootfs = options.rootfs.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        raise CampaignError(f"emulator is not executable: {emulator}")
    emulator_bytes = emulator.read_bytes()
    provider_sha256 = (
        verify_staged_kosmickrisp(emulator) if options.gl46_scope else None
    )
    if options.renderer == "virgl":
        for marker in RETIRED_BACKEND_MARKERS:
            if marker.encode() in emulator_bytes:
                raise CampaignError(
                    f"emulator contains a retired host GL provider: {marker}"
                )
        if HOST_BACKEND_MARKER.encode() not in emulator_bytes:
            raise CampaignError(
                "emulator does not contain the Zink/KosmicKrisp provider marker"
            )
    rootfs_contract = options.rootfs_contract
    if rootfs_contract.filesystem_type == "erofs":
        if not rootfs.is_file() or rootfs.stat().st_size != rootfs_contract.size:
            raise CampaignError("rootfs size does not match the frozen CTS identity")
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
    else:
        validate_rootfs_file(rootfs, rootfs_contract)

    artifact_directory.mkdir(parents=True, exist_ok=False)
    metadata = {
        "schema_version": 1,
        "git": git_state(),
        "adapter": "macos",
        "backend": host_backend_label(options.renderer),
        "renderer": options.renderer,
        "page_profile": "4k",
        "engine": options.engine,
        "memory": options.memory,
        "cpus": options.cpus,
        "display": options.display,
        "surface": [options.width, options.height],
        "context": options.context,
        "mode": (
            "inventory"
            if options.inventory
            else "config-inventory"
            if options.config_inventory is not None
            else "mustpass-matrix-case-subset"
            if options.matrix is not None and options.case
            else "mustpass-matrix-shard"
            if options.matrix is not None
            else "shard"
        ),
        "rootfs_type": rootfs_contract.filesystem_type,
        "rootfs_sha256": rootfs_contract.sha256,
        "glcts_sha256": rootfs_contract.glcts_sha256,
        "emulator_sha256": hashlib.sha256(emulator_bytes).hexdigest(),
        "qualification": options.qualification,
        "artifact_transport": {
            "type": "virtio-serial-unix-socket",
            "guest_device": VIRTIO_SERIAL_GUEST_DEVICE,
            "port_name": VIRTIO_SERIAL_PORT_NAME,
            "acknowledged_frames": True,
        },
        "launch_environment": {
            **{name: "1" for name in gl_scope_launch_environment(options)},
            **(
                {"UML_GPU_FRAME_STATS": "1"}
                if options.display == "native"
                else {}
            ),
        },
        "command": command,
    }
    if options.derivative_receipt is not None:
        metadata["rootfs_receipt"] = options.derivative_receipt
    if options.gl46_scope:
        metadata["formal_conformance"] = False
        metadata["kosmickrisp_sha256"] = provider_sha256
        metadata["rootfs_receipt_sha256"] = sha256_file(options.rootfs_receipt.resolve())
        metadata["successful_case_statuses"] = (
            options.derivative_receipt["runner_oracle"]["successful"]
            if options.qualification else ["Pass"]
        )
    if options.inventory:
        metadata["expected_artifacts"] = [
            "inventory.txt",
            "renderer.txt",
            "results.qpa",
            "summary.txt",
        ]
    elif options.config_inventory is not None:
        metadata.update(
            {
                "api": api_identifier,
                "config_case": CONFIG_CASES[api_identifier],
                **(
                    {
                        "config_scope": CONFIG_INVENTORY_SCOPE_NAMES[api_identifier],
                        "config_cases": list(CONFIG_INVENTORY_SCOPES[api_identifier]),
                    }
                    if len(CONFIG_INVENTORY_SCOPES[api_identifier]) > 1
                    else {}
                ),
                "expected_artifacts": [
                    "renderer.txt",
                    "results.qpa",
                    "summary.txt",
                ],
            }
        )
    else:
        metadata.update(
            {
                "api": api_identifier,
                "leaf": leaf_identifier,
                "leaf_sha256": leaf.sha256,
                "fraction": [fraction_index, fraction_count],
                "exact_cases": list(selection.exact_cases),
                "expected_cases": selection.expected_count,
                "seed": (
                    options.matrix_run.configuration.base_seed
                    if options.matrix is not None
                    else options.seed
                ),
            }
        )
        if options.campaign is not None:
            metadata["campaign"] = options.campaign
        if options.matrix is not None:
            metadata["matrix"] = matrix_metadata(options)
    write_json(artifact_directory / "run.json", metadata)
    log_path = artifact_directory / "console.log"
    artifact_socket = artifact_socket_path(artifact_directory)
    artifact_log_path = artifact_directory / ARTIFACT_TRANSPORT_LOG
    required_artifacts = (
        INVENTORY_ARTIFACTS
        if options.inventory
        else CONFIG_INVENTORY_ARTIFACTS
        if options.config_inventory is not None
        else SHARD_ARTIFACTS
    )

    def missing_artifacts(names: set[str], transcript: str) -> None:
        required = required_artifacts - names
        if required:
            guest_errors = GUEST_ERROR.findall(transcript)
            if guest_errors:
                raise CampaignError(f"guest campaign failed: {guest_errors[-1]}")
            raise CampaignError(f"campaign omitted artifacts: {sorted(required)!r}")
        unexpected = names - required_artifacts
        if unexpected:
            raise CampaignError(
                f"campaign emitted unexpected artifacts: {sorted(unexpected)!r}"
            )

    if selection is None:
        command = prepare_harness_command(options, selection, artifact_directory)
        with log_path.open("wb") as log:
            with guest_process.managed_child(
                command, stop=stop_guest, stdout=log, stderr=subprocess.STDOUT,
            ) as process:
                transcript, artifact_transcript = wait_for_guest(
                    process,
                    log_path,
                    options.guest_timeout,
                    artifact_socket,
                )
        if process.returncode != 0:
            raise CampaignError(
                f"guest harness exited with status {process.returncode}"
            )
        if os.path.lexists(artifact_socket):
            raise CampaignError("emulator did not remove the artifact socket")
        artifact_log_path.write_text(artifact_transcript, encoding="ascii")
        validate_guest_shutdown(transcript)
        artifacts = decode_artifacts(artifact_transcript)
        missing_artifacts(set(artifacts), transcript)
        write_artifacts(artifacts, artifact_directory / "artifacts")
        sessions = ()
        emulator_return_code = process.returncode
    else:
        def record_sessions(rows: tuple[SessionEvidence, ...]) -> None:
            metadata.update(
                {
                    "session_count": len(rows),
                    "max_resumptions": options.max_resumptions,
                    "console_stall_timeout": options.console_stall_timeout,
                    "console_stall_busy_timeout": options.console_stall_busy_timeout,
                    "artifacts_provenance": shard_provenance(rows),
                    "shutdown_stalls": sum(1 for row in rows if row.shutdown_stall),
                    "sessions": session_records(rows, artifact_directory),
                }
            )
            write_json(artifact_directory / "run.json", metadata)

        sessions, expected_cases, merged_results = run_shard_sessions(
            artifact_directory,
            build_command=lambda resume, guest: prepare_harness_command(
                options, selection, guest, resume_after=resume
            ),
            resolve_expected=lambda first: tuple(
                parse_case_list(
                    first["selected.txt"].decode("utf-8", errors="strict")
                )
                if "selected.txt" in first
                else ()
            ),
            # A stalled first guest is expected to be missing its summary;
            # the shard-level artifact set is checked once, after the merge.
            check_session=lambda index, resume, found: None,
            guest_timeout=options.guest_timeout,
            console_stall_timeout=options.console_stall_timeout,
            max_resumptions=options.max_resumptions,
            on_sessions=record_sessions,
        )
        final = sessions[-1]
        transcript = (artifact_directory / final.console).read_text(
            encoding="utf-8", errors="replace"
        )
        artifact_transcript = (
            artifact_directory / final.artifact_transport
        ).read_text(encoding="ascii")
        if shard_provenance(sessions) == "single-session-guest":
            artifacts = {
                path.name: path.read_bytes()
                for path in sorted(
                    (artifact_directory / final.artifacts_directory).iterdir()
                )
            }
        else:
            artifacts = merged_shard_artifacts(
                sessions, artifact_directory, len(expected_cases)
            )
            write_artifacts(artifacts, artifact_directory / "artifacts")
        missing_artifacts(set(artifacts), transcript)
        if list(merged_results) != list(expected_cases):
            raise CampaignError("merged shard results left the selected order")
        record_sessions(sessions)
        emulator_return_code = final.return_code
        if final.shutdown_stall:
            # The guest transported every acknowledged frame and then failed
            # to power off inside its shutdown grace.  The host reaped it and
            # keeps the complete evidence, so only the memory-manager
            # contract still applies to that transcript.
            validate_drm_shutdown(transcript)
        else:
            validate_guest_shutdown(transcript)
    metadata["artifact_transport_sha256"] = sha256_file(artifact_log_path)
    metadata["console_sha256"] = sha256_file(log_path)
    if options.renderer == "virgl":
        validate_host_backend(transcript)
        capability_report = parse_gpu_capability_report(transcript)
        if "venus_capset" not in capability_report:
            raise CampaignError("GPU capability report omitted the Venus capset")
        metadata.update(capability_report)
    if options.display == "native":
        metadata["native_display_final"] = parse_native_display_final(transcript)
    write_json(artifact_directory / "run.json", metadata)
    metadata["renderer_sha256"] = hashlib.sha256(
        artifacts["renderer.txt"]
    ).hexdigest()
    renderer_text = artifacts["renderer.txt"].decode("utf-8", errors="strict")
    summary_text = artifacts["summary.txt"].decode("utf-8", errors="strict")
    validate_renderer_probe(
        renderer_text,
        options.renderer,
        host_backend_label(options.renderer),
    )
    if sessions and shard_provenance(sessions) == "host-merged":
        # A host-merged shard always carries at least one recorded terminated
        # case; the precise per-case rejection comes from the status policy
        # below, after run.json records the complete totals.
        if "provenance=host-merged" not in summary_text or (
            "cts_rc=1" not in summary_text
        ):
            raise CampaignError("host-merged shard summary is not truthful")
    elif summary_text.strip() != "cts_rc=0":
        raise CampaignError(f"CTS process failed: {summary_text.strip()}")
    if options.inventory:
        case_count, caselist_digest, modules, package_count = validate_inventory_report(
            artifacts["inventory.txt"], options.identity
        )
        metadata.update(
            {
                "emulator_return_code": emulator_return_code,
                "complete_caselist_case_count": case_count,
                "complete_caselist_sha256": caselist_digest,
                "exported_package_count": package_count,
                "registered_modules": modules,
            }
        )
        write_json(artifact_directory / "run.json", metadata)
        print(
            f"VK-GL-CTS inventory captured {case_count} cases "
            f"from {len(modules)} modules"
        )
        print(f"Artifacts: {artifact_directory}")
        return 0

    if options.config_inventory is not None:
        qpa_payload = artifacts["results.qpa"]
        qpa_text = decode_qpa(qpa_payload)
        configs = parse_egl_config_qpa(qpa_text, options.config_inventory)
        inventory_cases = CONFIG_INVENTORY_SCOPES[options.config_inventory]
        totals = {"Pass": len(inventory_cases)}
        metadata.update(
            {
                "emulator_return_code": emulator_return_code,
                "completed_cases": len(inventory_cases),
                "status_totals": totals,
                "qpa_sha256": hashlib.sha256(qpa_payload).hexdigest(),
                "egl_config_sha256": egl_config_digest(configs),
                "egl_configs": [
                    {
                        "id": config.identifier,
                        "surface_types": list(config.surface_types),
                        "selected_surface_type": config.selected_surface_type,
                    }
                    for config in configs
                ],
            }
        )
        if EGL_ATTRIBUTE_CASE in inventory_cases:
            attributes = parse_egl_config_attributes(qpa_text)
            metadata.update(
                {
                    "egl_config_attributes": attributes,
                    "egl_config_attributes_sha256": egl_attribute_digest(attributes),
                }
            )
        write_json(artifact_directory / "run.json", metadata)
        print(
            f"VK-GL-CTS {options.config_inventory} config inventory "
            f"captured {len(configs)} EGL configs"
        )
        print(f"Artifacts: {artifact_directory}")
        return 0

    selected_text = artifacts["selected.txt"].decode("utf-8", errors="strict")
    selected_cases = parse_case_list(selected_text)
    selected_digest = hashlib.sha256(artifacts["selected.txt"]).hexdigest()
    if len(selected_cases) != selection.expected_count:
        raise CampaignError("egressed caselist count changed")
    if options.matrix is not None and selection.exact_cases:
        # The guest proved the request is an ordered subset of its shard; the
        # host holds the egressed list to the request it sent, so the evidence
        # names the cases that were asked for and no others.
        if tuple(selected_cases) != selection.exact_cases:
            raise CampaignError("egressed caselist is not the requested subset")
    qpa_text = decode_qpa(artifacts["results.qpa"])
    results = validate_qpa_union(qpa_text, selected_cases)
    totals = status_totals(results)
    metadata.update(
        {
            "emulator_return_code": emulator_return_code,
            "completed_cases": len(results),
            "status_totals": totals,
            "selected_caselist_sha256": selected_digest,
            "qpa_sha256": hashlib.sha256(artifacts["results.qpa"]).hexdigest(),
        }
    )
    waived: frozenset[str] = frozenset()
    if options.matrix is not None:
        waived = waived_cases(options.applied_waivers)
        metadata["waived_results"] = {
            case: status for case, status in results.items() if case in waived
        }
    write_json(artifact_directory / "run.json", metadata)
    if options.matrix is not None:
        validate_waiver_session(qpa_text, options.applied_waivers)
        validate_official_runner_results(
            results, options.runner_oracle.successful_case_statuses, waived
        )
    elif options.renderer == "zink" and options.qualification:
        validate_official_runner_results(
            results, options.derivative_receipt["runner_oracle"]["successful"]
        )
    else:
        validate_required_passes(results)
    print(f"VK-GL-CTS completed {len(results)} cases: {totals}")
    print(f"Artifacts: {artifact_directory}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(report_campaign_leaf_exit(main()))
    except KeyboardInterrupt:
        print("run_vk_gl_cts.py: interrupted; guest stopped", file=sys.stderr)
        raise SystemExit(report_campaign_leaf_exit(130)) from None
    except (CampaignError, OSError, subprocess.SubprocessError, TimeoutError) as error:
        print(f"run_vk_gl_cts.py: {error}", file=sys.stderr)
        raise SystemExit(report_campaign_leaf_exit(1)) from error
