#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the checked Vulkan rootfs/CTS receipt and optional image."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import stat
import sys
from typing import Any, Sequence


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import patch_series  # noqa: E402


RECEIPT_PATH = Path(__file__).resolve().with_name("vulkan_cts_receipt.json")
SCHEMA_PATH = Path(__file__).resolve().with_name(
    "vulkan_cts_future_receipt.schema.json"
)
METADATA_ROOT = REPO_ROOT / "tools/rootfs/vulkan"
MANDATORY_CASES_PATH = Path(__file__).resolve().with_name(
    "vulkan_cts_mandatory_1_4_6_2.txt"
)
EXPECTED_IMAGE_SHA256 = "757c0222fea1bea9426213f446f8cc7e6ae5d220dcfb93be8fb82ca7ce7be8fc"
EXPECTED_IMAGE_SIZE = 8_589_934_592
EXPECTED_IMAGE_UUID = "2240061a-2b64-4626-b8bf-dd0f9993a4a9"
EXPECTED_BASE_SHA256 = "73e36c063e8374b68bdaca992f789d41dda59e159fd4b426dbbb5515dcf84c91"
EXPECTED_CTS_BUILD_ID = "bd277a83570fdcb78b7ca517aff035291e5a5190"
EXPECTED_CTS_RELEASE = (
    "vulkan-cts-1.4.6.2-2-gfd3b1e6a27189ae5bdc6b2f8cce38b0d130222ca"
)
EXPECTED_LOADER_BUILD_ID = "99f7dbb91bffb92392b22ad3ff624c6bb7d9fa0d"
EXPECTED_LOADER_RUNTIME = "/usr/lib/aarch64-linux-gnu/libvulkan.so.1.4.363"
EXPECTED_MESA_FILES = {
    "/usr/lib/aarch64-linux-gnu/dri/libdril_dri.so",
    "/usr/lib/aarch64-linux-gnu/gbm/dri_gbm.so",
    "/usr/lib/aarch64-linux-gnu/libEGL_mesa.so.0.0.0",
    "/usr/lib/aarch64-linux-gnu/libGLX_mesa.so.0.0.0",
    "/usr/lib/aarch64-linux-gnu/libgallium-26.2.6.so",
    "/usr/lib/aarch64-linux-gnu/libgbm.so.1.0.0",
    "/usr/lib/aarch64-linux-gnu/libvulkan_virtio.so",
    "/usr/share/drirc.d/00-mesa-defaults.conf",
    "/usr/share/drirc.d/00-venus-defaults.conf",
    "/usr/share/drirc.d/00-virtio_gpu-defaults.conf",
    "/usr/share/drirc.d/00-zink-defaults.conf",
    "/usr/share/glvnd/egl_vendor.d/50_mesa.json",
    "/usr/share/vulkan/icd.d/virtio_icd.json",
}
EXPECTED_CLOSURE_CONSUMERS = {
    "/opt/vk-gl-cts/vulkan/deqp-vk",
    "/usr/lib/aarch64-linux-gnu/dri/libdril_dri.so",
    "/usr/lib/aarch64-linux-gnu/gbm/dri_gbm.so",
    "/usr/lib/aarch64-linux-gnu/libEGL_mesa.so.0.0.0",
    "/usr/lib/aarch64-linux-gnu/libGLX_mesa.so.0.0.0",
    "/usr/lib/aarch64-linux-gnu/libgallium-26.2.6.so",
    "/usr/lib/aarch64-linux-gnu/libgbm.so.1.0.0",
    "/usr/lib/aarch64-linux-gnu/libvulkan.so.1.4.363",
    "/usr/lib/aarch64-linux-gnu/libvulkan_virtio.so",
}
TOP_LEVEL_KEYS = {
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
    "toolchain",
    "install_closure",
    "mustpass",
    "runner_oracle",
    "exclusions",
    "base_packages",
    "guest_receipt",
}
SHA256 = re.compile(r"[0-9a-f]{64}\Z")


class ReceiptError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise ReceiptError(message)


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            fail(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read JSON {path}: {error}")
    if not isinstance(value, dict):
        fail(f"JSON value is not an object: {path}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            while block := source.read(4 * 1024 * 1024):
                digest.update(block)
    except OSError as error:
        fail(f"cannot hash {path}: {error}")
    return digest.hexdigest()


def absolute_guest_path(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value.startswith("/") or "\x00" in value:
        fail(f"{field} is not an absolute guest path")
    parts = Path(value).parts[1:]
    if not parts or any(part in {"", ".", ".."} for part in parts):
        fail(f"{field} is not a normalized guest path")
    return value


def positive_integer(value: Any, field: str) -> int:
    if type(value) is not int or value < 1:
        fail(f"{field} is not a positive integer")
    return value


def file_record(value: Any, field: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != {"path", "size", "sha256"}:
        fail(f"{field} is not an exact file record")
    absolute_guest_path(value["path"], f"{field}.path")
    positive_integer(value["size"], f"{field}.size")
    if not isinstance(value["sha256"], str) or SHA256.fullmatch(value["sha256"]) is None:
        fail(f"{field}.sha256 is invalid")
    return value


def image_record(value: Any, field: str) -> dict[str, Any]:
    keys = {
        "filename",
        "filesystem",
        "size",
        "sha256",
        "filesystem_uuid",
        "read_only",
    }
    if not isinstance(value, dict) or set(value) != keys:
        fail(f"{field} is not an exact image record")
    filename = value["filename"]
    if not isinstance(filename, str) or not filename or Path(filename).name != filename:
        fail(f"{field}.filename is invalid")
    positive_integer(value["size"], f"{field}.size")
    if not isinstance(value["sha256"], str) or SHA256.fullmatch(value["sha256"]) is None:
        fail(f"{field}.sha256 is invalid")
    if value["read_only"] is not True:
        fail(f"{field} is not read-only")
    return value


def read_tsv(path: Path) -> list[dict[str, str]]:
    lines = [
        line
        for line in path.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    rows = list(csv.DictReader(lines, delimiter="\t"))
    if not rows or any(None in row or None in row.values() for row in rows):
        fail(f"invalid TSV: {path}")
    return rows


def verify_source_lock(
    receipt_source: Any,
    lock: dict[str, str],
    field: str,
    *,
    patched_tree: str | None,
) -> None:
    if not isinstance(receipt_source, dict):
        fail(f"{field} is not an object")
    for name in ("repository", "base_commit", "base_tree"):
        if receipt_source.get(name) != lock[name]:
            fail(f"{field}.{name} differs from sources.tsv")
    if lock["release_ref"] != "-" and (
        receipt_source.get("release_ref") != lock["release_ref"]
        or receipt_source.get("release_object") != lock["release_object"]
    ):
        fail(f"{field} release tag differs from sources.tsv")
    if patched_tree is None:
        if receipt_source.get("patched") is not False:
            fail(f"{field} is not explicitly unpatched")
        if "patched_tree" in receipt_source or "patches" in receipt_source:
            fail(f"{field} has unexpected downstream patch identity")
        return
    # The tree a series records is its identity: a reformatted or folded series
    # that produces the same tree needs no new seal.
    if receipt_source.get("patched_tree") != patched_tree:
        fail(f"{field}.patched_tree changed")


def recorded_tree(lock: dict[str, str]) -> str:
    try:
        tree = patch_series.read_series((METADATA_ROOT / lock["series"]).parent).tree
    except patch_series.SeriesError as error:
        fail(str(error))
    if tree is None:
        fail(f"{lock['component']} series records no tree")
    return tree


def verify_source_epoch(receipt: dict[str, Any]) -> None:
    """Check the shared source/series epoch without selecting an image or build."""
    locks = {
        row["component"]: row for row in read_tsv(METADATA_ROOT / "sources.tsv")
    }
    for field, component in (
        ("protocol", "venus-protocol"),
        ("mesa", "mesa"),
        ("vulkan_headers", "vulkan-headers"),
        ("vulkan_loader", "vulkan-loader"),
        ("cts", "vk-gl-cts"),
    ):
        lock = locks[component]
        verify_source_lock(
            receipt.get(field),
            lock,
            field,
            patched_tree=recorded_tree(lock) if lock["series"] != "-" else None,
        )
    # Mesa's established receipt shape identifies its patches through the tree
    # and complete series; protocol and CTS additionally carry this boolean.
    for field in ("protocol", "cts"):
        if receipt[field].get("patched") is not True:
            fail(f"{field} is not explicitly patched")
    generation = receipt["protocol"].get("generation")
    if not isinstance(generation, dict) or set(generation) != {
            "configure_argv", "registry_sha256", "driver_headers"}:
        fail("protocol generation identity is absent or malformed")
    if generation["registry_sha256"] != "55ec60950cfb18c3575dcf5fd52741b2bb70eb1e466408f803a91049973ee6fb":
        fail("protocol generation registry differs from the pinned SDK")
    argv = generation["configure_argv"]
    headers = generation["driver_headers"]
    sdk_option = "-Dvulkan-headers-dir="
    if (not isinstance(argv, list) or len(argv) != 6 or
            any(not isinstance(value, str) or not value for value in argv) or
            argv[:2] != ["meson", "setup"] or
            argv[4] not in {"--wrap-mode=nofallback", "--wrap-mode=nodownload"} or
            not argv[5].startswith(sdk_option)):
        fail("protocol generation command is not the exact offline Meson setup")
    definitions = load_json(SCHEMA_PATH)["$defs"]
    path_pattern = definitions["protocolInputPath"]["pattern"]
    for field, value in (("build", argv[2]), ("source", argv[3]),
                         ("SDK", argv[5][len(sdk_option):])):
        if re.fullmatch(path_pattern, value) is None:
            fail(f"protocol generation {field} directory is not a normalized absolute path")
    mesa_argv = receipt["mesa"].get("configure_argv")
    if (not isinstance(mesa_argv, list) or
            any(not isinstance(value, str) or not value for value in mesa_argv)):
        fail("Mesa protocol generation binding command is malformed")
    # Count split -D spellings too, so they cannot hide a duplicate override.
    bindings = [value for value in mesa_argv if value.partition("=")[0] in {
        "-Dvenus-protocol-dir", "venus-protocol-dir",
    }]
    if bindings != [f"-Dvenus-protocol-dir={argv[2]}"]:
        fail("Mesa protocol generation binding must name the generated build exactly once")
    expected_headers = definitions["protocolDriverHeader"]["enum"]
    if not isinstance(headers, dict) or set(headers) != set(expected_headers):
        fail("protocol generated header inventory changed")
    for digest in headers.values():
        if not isinstance(digest, str) or SHA256.fullmatch(digest) is None:
            fail("protocol generated header identity is malformed")


def validate_receipt(path: Path = RECEIPT_PATH) -> dict[str, Any]:
    receipt = load_json(path)
    if set(receipt) != TOP_LEVEL_KEYS:
        fail(f"receipt top-level keys changed: {sorted(set(receipt) ^ TOP_LEVEL_KEYS)}")
    if receipt["schema_version"] != 2 or receipt["artifact"] != "uml-vulkan-rootfs":
        fail("unsupported Vulkan receipt identity")
    schema = load_json(SCHEMA_PATH)
    if schema.get("$id") != "https://uml-darwin.invalid/schemas/vulkan-cts-receipt-v2.json":
        fail("receipt schema identity changed")

    scope = receipt["claim_scope"]
    if scope != {
        "backend": "venus-kosmickrisp",
        "downstream_guest_mesa": True,
        "formal_conformance": False,
        "purpose": "Receipt-gated CTS qualification of the Venus/KosmicKrisp Vulkan path; not a Khronos conformance submission or claim.",
    }:
        fail("Vulkan receipt claim scope changed")
    base = image_record(receipt["base_rootfs"], "base_rootfs")
    if (
        base["filename"] != "rootfs.erofs"
        or base["filesystem"] != "erofs"
        or base["size"] != 3_802_316_800
        or base["sha256"] != EXPECTED_BASE_SHA256
        or base["filesystem_uuid"] != "b84fcbe2-0673-4126-8f81-bdee45997306"
    ):
        fail("base rootfs identity changed")
    derivative = image_record(receipt["derivative"], "derivative")
    if (
        derivative["filename"] != "rootfs-vulkan-venus.ext4"
        or derivative["filesystem"] != "ext4"
        or derivative["size"] != EXPECTED_IMAGE_SIZE
        or derivative["sha256"] != EXPECTED_IMAGE_SHA256
        or derivative["filesystem_uuid"] != EXPECTED_IMAGE_UUID
    ):
        fail("Vulkan derivative identity changed")

    verify_source_epoch(receipt)

    mesa = receipt["mesa"]
    if (
        mesa.get("release") != "mesa-26.2.6"
        or mesa.get("install_prefix") != "/usr"
        or mesa.get("reported_api_version") != "1.4.363"
        or mesa.get("portability_driver") is not False
        or not isinstance(mesa.get("configure_argv"), list)
        or not isinstance(mesa.get("build_targets"), list)
    ):
        fail("Mesa receipt identity changed")
    mesa_files = mesa.get("files")
    if not isinstance(mesa_files, list):
        fail("Mesa file receipt is absent")
    checked_mesa_files = [file_record(row, "mesa.files[]") for row in mesa_files]
    if {row["path"] for row in checked_mesa_files} != EXPECTED_MESA_FILES:
        fail("Mesa install inventory changed")

    headers = receipt["vulkan_headers"]
    if (
        headers.get("release") != "vulkan-sdk-1.4.363.0"
        or headers.get("installed_in_guest") is not False
        or not isinstance(headers.get("configure_argv"), list)
        or not isinstance(headers.get("build_targets"), list)
    ):
        fail("Vulkan-Headers build identity changed")

    loader = receipt["vulkan_loader"]
    if (
        loader.get("release") != "vulkan-sdk-1.4.363.0"
        or loader.get("install_prefix") != "/usr"
        or loader.get("reported_api_version") != "1.4.363"
        or loader.get("build_id") != EXPECTED_LOADER_BUILD_ID
        or loader.get("soname") != "libvulkan.so.1"
        or loader.get("symlinks")
        != {
            "/usr/lib/aarch64-linux-gnu/libvulkan.so": "libvulkan.so.1",
            "/usr/lib/aarch64-linux-gnu/libvulkan.so.1": "libvulkan.so.1.4.363",
        }
        or not isinstance(loader.get("configure_argv"), list)
        or not isinstance(loader.get("build_targets"), list)
        or not isinstance(loader.get("install_argv"), list)
    ):
        fail("Vulkan-Loader build identity changed")
    loader_runtime = file_record(loader.get("runtime"), "vulkan_loader.runtime")
    if loader_runtime["path"] != EXPECTED_LOADER_RUNTIME:
        fail("Vulkan-Loader runtime path changed")

    cts = receipt["cts"]
    if (
        cts.get("release") != "vulkan-cts-1.4.6.2"
        or cts.get("build_id") != EXPECTED_CTS_BUILD_ID
        or cts.get("reported_cpu") != "DE_CPU_ARM_64"
        or cts.get("reported_release") != EXPECTED_CTS_RELEASE
        or cts.get("reported_target") != "Default"
        or cts.get("install_prefix") != "/opt/vk-gl-cts/vulkan"
    ):
        fail("CTS build identity changed")
    executable = file_record(cts.get("executable"), "cts.executable")
    if executable["path"] != "/opt/vk-gl-cts/vulkan/deqp-vk":
        fail("CTS executable path changed")
    resources = cts.get("resources")
    expected_resources = {
        "/opt/vk-gl-cts/vulkan/amber",
        "/opt/vk-gl-cts/vulkan/vulkan",
        "/opt/vk-gl-cts/vulkan/vk-default",
    }
    if not isinstance(resources, list) or {
        row.get("path") for row in resources if isinstance(row, dict)
    } != expected_resources:
        fail("CTS resource closure changed")
    for index, row in enumerate(resources):
        if not isinstance(row, dict) or set(row) != {
            "path",
            "file_count",
            "total_bytes",
            "verified_against",
        }:
            fail(f"cts.resources[{index}] is malformed")
        absolute_guest_path(row["path"], f"cts.resources[{index}].path")
        positive_integer(row["file_count"], f"cts.resources[{index}].file_count")
        positive_integer(row["total_bytes"], f"cts.resources[{index}].total_bytes")
        if not isinstance(row["verified_against"], str) or not row["verified_against"]:
            fail(f"cts.resources[{index}].verified_against is empty")

    closure = receipt["install_closure"]
    if not isinstance(closure, list) or len(closure) != 140:
        fail("dynamic install closure count changed")
    closure_keys: set[tuple[str, str]] = set()
    for index, row in enumerate(closure):
        if not isinstance(row, dict) or set(row) != {
            "consumer",
            "needed",
            "resolved_path",
            "file",
        }:
            fail(f"install_closure[{index}] is malformed")
        consumer = absolute_guest_path(row["consumer"], f"install_closure[{index}].consumer")
        resolved = absolute_guest_path(
            row["resolved_path"], f"install_closure[{index}].resolved_path"
        )
        needed = row["needed"]
        if not isinstance(needed, str) or not needed:
            fail(f"install_closure[{index}].needed is empty")
        record = file_record(row["file"], f"install_closure[{index}].file")
        if record["path"] != resolved or (consumer, needed) in closure_keys:
            fail("dynamic install closure is inconsistent or duplicated")
        closure_keys.add((consumer, needed))
    if {consumer for consumer, _ in closure_keys} != EXPECTED_CLOSURE_CONSUMERS:
        fail("dynamic install closure consumers changed")

    mustpass = receipt["mustpass"]
    expected_mustpass_keys = {
        "top_level",
        "mandatory",
        "leaves",
        "leaf_count",
        "case_count",
        "unique_case_count",
        "duplicate_occurrence_count",
        "coverage_semantics",
    }
    if not isinstance(mustpass, dict) or set(mustpass) != expected_mustpass_keys:
        fail("mustpass receipt shape changed")
    top_level = file_record(mustpass["top_level"], "mustpass.top_level")
    mandatory = file_record(mustpass["mandatory"], "mustpass.mandatory")
    if top_level["path"] != "/opt/vk-gl-cts/vulkan/vk-default.txt" or mandatory[
        "path"
    ] != "/opt/vk-gl-cts/vulkan/vk-fraction-mandatory-tests.txt":
        fail("mustpass entry paths changed")
    mandatory_cases = [
        line
        for line in MANDATORY_CASES_PATH.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    if (
        len(mandatory_cases) != 21
        or len(set(mandatory_cases)) != 21
        or mandatory["size"] != MANDATORY_CASES_PATH.stat().st_size
        or mandatory["sha256"] != sha256_file(MANDATORY_CASES_PATH)
    ):
        fail("checked mandatory CTS case list changed")
    leaves = mustpass["leaves"]
    if not isinstance(leaves, list):
        fail("mustpass leaves are absent")
    checked_leaves = [file_record(row, "mustpass.leaves[]") for row in leaves]
    leaf_paths = [row["path"] for row in checked_leaves]
    if (
        mustpass["leaf_count"] != 98
        or len(checked_leaves) != 98
        or len(set(leaf_paths)) != 98
        or any(not path.startswith("/opt/vk-gl-cts/vulkan/vk-default/") for path in leaf_paths)
        or mustpass["case_count"] != 3_247_552
        or mustpass["unique_case_count"] != 3_247_552
        or mustpass["duplicate_occurrence_count"] != 0
        or not isinstance(mustpass["coverage_semantics"], str)
        or not mustpass["coverage_semantics"]
    ):
        fail("mustpass expansion identity changed")

    oracle = receipt["runner_oracle"]
    oracle_source = load_json(METADATA_ROOT / "status-oracle.json")
    if not isinstance(oracle, dict) or oracle.get("successful") != oracle_source.get(
        "successful"
    ) or oracle.get("always_failure") != oracle_source.get("always_failure"):
        fail("runner status oracle changed")
    if oracle.get("qualification_authority") is not False:
        fail("runner oracle claims qualification authority")
    oracle_file = file_record(oracle.get("file"), "runner_oracle.file")
    if (
        oracle_file["path"] != "/usr/share/uml-vulkan/status-oracle.json"
        or oracle_file["sha256"]
        != hashlib.sha256(
            (METADATA_ROOT / "status-oracle.json").read_bytes()
        ).hexdigest()
    ):
        fail("installed status oracle identity changed")

    exclusions = [
        line
        for line in (METADATA_ROOT / "exclusions.txt").read_text(
            encoding="utf-8"
        ).splitlines()
        if line and not line.startswith("#")
    ]
    if receipt["exclusions"] != exclusions:
        fail("rootfs exclusion set changed")
    packages = receipt["base_packages"]
    if packages != {
        "libdrm2:arm64": "2.4.124-2",
        "libegl-mesa0:arm64": "25.0.7-2+deb13u1",
        "libgbm1:arm64": "25.0.7-2+deb13u1",
        "libgl1-mesa-dri:arm64": "25.0.7-2+deb13u1",
        "libglx-mesa0:arm64": "25.0.7-2+deb13u1",
        "libvulkan1:arm64": "1.4.309.0-1",
        "mesa-vulkan-drivers:arm64": "25.0.7-2+deb13u1",
    }:
        fail("base package versions changed")
    guest_receipt = file_record(receipt["guest_receipt"], "guest_receipt")
    if guest_receipt["path"] != "/usr/share/uml-vulkan/rootfs-build-receipt.json":
        fail("guest receipt path changed")
    return receipt


def verify_image(path: Path, receipt: dict[str, Any]) -> None:
    derivative = receipt["derivative"]
    try:
        metadata = path.stat()
    except OSError as error:
        fail(f"cannot stat Vulkan derivative {path}: {error}")
    if not path.is_file() or path.name != derivative["filename"]:
        fail("Vulkan derivative path or filename changed")
    if metadata.st_size != derivative["size"]:
        fail("Vulkan derivative size changed")
    if stat.S_IMODE(metadata.st_mode) & 0o222:
        fail("Vulkan derivative is writable")
    if sha256_file(path) != derivative["sha256"]:
        fail("Vulkan derivative digest changed")


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", type=Path, default=RECEIPT_PATH)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--quiet", action="store_true")
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    receipt = validate_receipt(options.receipt.resolve())
    if options.image is not None:
        verify_image(options.image.resolve(), receipt)
    if not options.quiet:
        message = "verified checked Vulkan CTS/rootfs receipt"
        if options.image is not None:
            message += " and read-only derivative"
        print(message)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ReceiptError as error:
        print(f"vulkan_cts_receipt.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
