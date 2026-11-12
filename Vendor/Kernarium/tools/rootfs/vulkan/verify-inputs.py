#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
from pathlib import Path
import re
import sys
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
import patch_series  # noqa: E402


SOURCE_HEADER = (
    "component",
    "repository",
    "base_commit",
    "base_tree",
    "release_ref",
    "release_object",
    "series",
)
REQUIRED_COMPONENTS = {
    "mesa",
    "venus-protocol",
    "vk-gl-cts",
    "vulkan-headers",
    "vulkan-loader",
}
UNPATCHED_COMPONENTS = {
    "vulkan-headers",
    "vulkan-loader",
}
HEX40 = re.compile(r"[0-9a-f]{40}")


class VerificationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise VerificationError(message)


def read_tsv(path: Path) -> tuple[tuple[str, ...], list[dict[str, str]]]:
    try:
        lines = [
            line
            for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]
    except OSError as error:
        fail(f"cannot read {path}: {error}")
    if not lines:
        fail(f"TSV is empty: {path}")
    reader = csv.DictReader(lines, delimiter="\t")
    header = tuple(reader.fieldnames or ())
    rows = list(reader)
    if any(None in row or None in row.values() for row in rows):
        fail(f"TSV has a malformed row: {path}")
    return header, rows


def safe_relative(root: Path, value: str, label: str) -> Path:
    relative = Path(value)
    if relative.is_absolute() or not value or "\x00" in value:
        fail(f"{label} is not a safe relative path: {value!r}")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError:
        fail(f"{label} escapes its root: {value!r}")
    return candidate


def read_series(metadata_root: Path, series_value: str) -> Any:
    """Parse a generated series; it must list patches and record its tree."""
    series = safe_relative(metadata_root, series_value, "series path")
    if series.name != "series.tsv":
        fail(f"series path does not name a series.tsv: {series_value}")
    try:
        parsed = patch_series.read_series(series.parent)
    except patch_series.SeriesError as error:
        fail(str(error))
    if parsed.tree is None:
        fail(f"patch series records no tree: {series}")
    if not parsed.names:
        fail(f"patch series is empty: {series}")
    return parsed


def verify_series(metadata_root: Path, series_value: str) -> str:
    parsed = read_series(metadata_root, series_value)
    return "\n".join(
        patch.read_text(encoding="utf-8", errors="strict") for patch in parsed.patches
    )


def require_landmarks(payload: str, landmarks: tuple[str, ...], label: str) -> None:
    missing = [landmark for landmark in landmarks if landmark not in payload]
    if missing:
        fail(f"{label} is missing required landmarks: {missing}")


def verify_generated_ownership(
    payload: str, label: str, forbidden_prefixes: tuple[str, ...] = (),
) -> None:
    """Keep generated imports out and retained shader binaries in one topic."""
    generated: set[str] = set()
    for line in payload.splitlines():
        if not line.startswith("diff "):
            continue
        # Only unquoted two-path Git headers are supported. Reject other forms
        # explicitly instead of silently leaving their paths unaudited.
        match = re.fullmatch(r'diff --git a/([^\s"\\]+) b/([^\s"\\]+)', line)
        if match is None:
            fail(f"{label} has an unsupported or quoted diff header: {line}")
        # A rename affects both paths; an ordinary edit names the same path
        # twice and must count only once for generated-file ownership.
        for path in sorted(set(match.groups())):
            if path.startswith(forbidden_prefixes):
                fail(f"{label} imports a build-only generated input: {path}")
            if path.endswith((".spv", "_spirv.h", "_spirv.hpp")):
                if path in generated:
                    fail(f"{label} repeats a generated file across topics: {path}")
                generated.add(path)


def verify(metadata_root: Path, outer_root: Path) -> None:
    header, rows = read_tsv(metadata_root / "sources.tsv")
    if header != SOURCE_HEADER:
        fail(f"source-lock header changed: {header}")
    components = [row["component"] for row in rows]
    if set(components) != REQUIRED_COMPONENTS or len(components) != len(set(components)):
        fail(f"source-lock components changed: {components}")

    series_payloads: dict[str, str] = {}
    for row in rows:
        component = row["component"]
        if not row["repository"].startswith("https://"):
            fail(f"{component} repository is not HTTPS")
        for field in ("base_commit", "base_tree"):
            if HEX40.fullmatch(row[field]) is None:
                fail(f"{component} {field} is not a full Git object ID")
        release_ref = row["release_ref"]
        release_object = row["release_object"]
        if (release_ref == "-") != (release_object == "-"):
            fail(f"{component} release ref/object pair is incomplete")
        if release_object != "-" and HEX40.fullmatch(release_object) is None:
            fail(f"{component} release object is not a full Git object ID")
        if row["series"] == "-":
            if component not in UNPATCHED_COMPONENTS:
                fail(f"component must carry a generated patch series: {component}")
        else:
            series_payloads[component] = verify_series(
                metadata_root, row["series"]
            )

    # Only what the local series still adds. venus-protocol v1.1.3 serializes
    # VK_KHR_maintenance8, VK_KHR_maintenance10 and vkCmdEndRendering2KHR
    # itself, so those moved from this series to the pinned base.
    require_landmarks(
        series_payloads["venus-protocol"],
        (
            "VK_KHR_device_address_commands",
            "VK_COMMAND_TYPE_vkCmdCopyMemoryKHR_EXT",
        ),
        "Venus protocol series",
    )
    verify_generated_ownership(
        series_payloads["venus-protocol"], "Venus protocol series",
        ("include/vulkan/", "include/vk_video/", "xmls/vk.xml"),
    )
    verify_generated_ownership(
        series_payloads["mesa"], "guest Mesa series", ("src/virtio/venus-protocol/",),
    )
    provider_payload = verify_series(outer_root / "dependencies", "patches/mesa/series.tsv")
    verify_generated_ownership(provider_payload, "KosmicKrisp series")
    cts_payload = series_payloads["vk-gl-cts"]
    require_landmarks(
        cts_payload,
        (
            "m_bufferSize - r.bufferImageCopy.bufferOffset",
            "Exclusive compute queue not supported.",
            "5e8b107ad4461730c208c4cc69f08a965c70ceea",
        ),
        "VK-GL-CTS series",
    )

    mesa_payload = series_payloads["mesa"]
    require_landmarks(
        mesa_payload,
        (
            "Always observe the renderer's creation result",
            "vn_host_copy_blocksize",
            "venus: stop advertising DRM display acquisition",
            "venus: preserve renderer conformance version",
            "zink_use_venus_implicit_dmabuf",
        ),
        "Mesa series",
    )
    for obsolete in (
        "VK_KHR_portability_subset",
        "is_portability_driver",
        "MoltenVK",
    ):
        if obsolete in mesa_payload:
            fail(f"Mesa series retains obsolete portability path: {obsolete}")

    renderer_names = read_series(
        outer_root / "dependencies", "patches/virglrenderer/series.tsv"
    ).names
    renderer_payload = verify_series(
        outer_root / "dependencies", "patches/virglrenderer/series.tsv"
    )
    verify_generated_ownership(
        renderer_payload, "renderer series", ("src/venus/venus-protocol/",),
    )
    require_landmarks(
        renderer_payload,
        (
            "vulkan-library-file",
            "VK_EXT_external_memory_host",
            "vkr_pipeline_add_array",
            "vkr_host_copy_footprint",
        ),
        "host renderer series",
    )
    obsolete_renderer_topics = (
        "venus-transport-portability-subset-structures",
        "venus-cap-Metal-renderer-at-Vulkan-1.3",
    )
    for name in renderer_names:
        if any(topic in name for topic in obsolete_renderer_topics):
            fail(f"host renderer retains obsolete topic: {name}")


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    script_root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--metadata-root", type=Path, default=script_root)
    parser.add_argument("--outer-root", type=Path, default=script_root.parents[2])
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    options = parse_arguments(argv)
    try:
        verify(options.metadata_root.resolve(), options.outer_root.resolve())
    except (OSError, UnicodeError, VerificationError) as error:
        print(f"verify-inputs.py: {error}", file=sys.stderr)
        return 1
    print("verified Vulkan guest source locks and patch series")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
