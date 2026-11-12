#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import datetime
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath

_ARTIFACTS = str(Path(__file__).resolve().parents[3] / "kernel/common/artifacts")
if _ARTIFACTS not in sys.path:
    sys.path.insert(0, _ARTIFACTS)
from contract_checks import ContractChecks, parse_config, sha256_path  # noqa: E402
from kernel_artifact import verify_config_policy  # noqa: E402

import release_inputs  # noqa: E402

_CHECKS = ContractChecks("Android release verifier")
error = _CHECKS.error
fail = _CHECKS.fail
require_exact_keys = _CHECKS.require_exact_keys
require_string = _CHECKS.require_string
require_integer = _CHECKS.require_integer
require_hex = _CHECKS.require_hex
safe_relative = _CHECKS.safe_relative
parse_env = _CHECKS.parse_env

# Schema 7 records the object IDs of the release's input paths (outer.input_trees,
# android/lineage-24/support/release/input-paths.tsv). Releases of schema 6 were
# published before that and stay valid as history; no release is built as one.
SCHEMA_VERSION = 7
LEGACY_SCHEMA_VERSION = 6
SCHEMA_VERSIONS = (LEGACY_SCHEMA_VERSION, SCHEMA_VERSION)
PRODUCT_FAMILY = "lineage-24-mainline-6.18-4k"
DIRECTORY_PREFIX = "lineage24-mainline6.18-4k-"
# The kernel artifact family of the Android product. Kernarium and CMake key
# Android mode on it.
KERNEL_FAMILY = "android-6.18"
RELEASE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
SECURITY_PATCH_RE = re.compile(r"^[0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])$")

# The Android build variants a release may be built as, with the build
# properties each one fixes. LINEAGE24_BUILD_VARIANT in metadata/lineage.env
# selects the one the tracked tree builds, and every variant-dependent check
# follows that setting.
BUILD_VARIANTS = {
    "user": {"ro.build.type": "user", "ro.debuggable": "0"},
    "userdebug": {"ro.build.type": "userdebug", "ro.debuggable": "1"},
}
# The only variant a release may be. userdebug is the diagnostics build: it carries su and
# a debuggable framework, so it is never published, signed with release keys or accepted.
RELEASE_BUILD_VARIANTS = frozenset({"user"})


def require_release_build_variant(variant: str) -> None:
    if variant not in RELEASE_BUILD_VARIANTS:
        fail(
            f"a {variant} build is a diagnostics build and cannot be published or accepted: "
            "set LINEAGE24_BUILD_VARIANT to user"
        )


# The one member of every otacerts.zip (the system's and the recovery's), by build variant. An unsigned
# build, of either variant, holds the AOSP test key's certificate under its file name: the r7 build's
# system, recovery and target-files copies each held testkey.x509.pem alone. sign_target_files_apks
# replaces it with a member named after the path it is given for the release key, which sign_release.py
# makes keys/releasekey.x509.pem. Only a user build is signed, every one of them with the one release key
# set; a userdebug build keeps the member the build wrote and is never re-signed.
OTA_CERTIFICATE_BUILD_MEMBER = "testkey.x509.pem"
OTA_CERTIFICATE_MEMBERS = {
    "user": "keys/releasekey.x509.pem",
    "userdebug": OTA_CERTIFICATE_BUILD_MEMBER,
}
OTA_CERTIFICATE_ARCHIVES = (
    "SYSTEM/etc/security/otacerts.zip",
    "RECOVERY/RAMDISK/system/etc/security/otacerts.zip",
)


def signed_ota_certificate_member(variant: str) -> str:
    """The member of a signed build's otacerts.zip; only a release variant is signed."""
    if variant not in RELEASE_BUILD_VARIANTS:
        fail(
            f"a {variant} build is not signed with the release keys: it keeps the AOSP test keys "
            "of the build and is never re-signed"
        )
    return OTA_CERTIFICATE_MEMBERS[variant]


def ota_certificate_member(variant: str, signed: bool) -> str:
    """The member both otacerts.zip hold: the build's test certificate until the build is signed."""
    if variant not in OTA_CERTIFICATE_MEMBERS:
        fail(f"no OTA certificate member is known for the build variant {variant}")
    return signed_ota_certificate_member(variant) if signed else OTA_CERTIFICATE_BUILD_MEMBER


def configured_build_variant(android_root: Path) -> str:
    variant = parse_env(android_root / "metadata/lineage.env")["LINEAGE24_BUILD_VARIANT"]
    if variant not in BUILD_VARIANTS:
        fail(f"tracked Android build variant is not supported: {variant}")
    return variant


PAYLOAD_SPECS = {
    "boot/kernel-command-line.txt": ("kernel-command-line", "utf8-text"),
    "guest/partition-layout.json": ("partition-layout", "json"),
    "guest/system.img": ("system-disk", "raw-gpt-read-only"),
    "kernel/System.map": ("kernel-symbol-map", "utf8-text"),
    "kernel/config": ("kernel-config", "kconfig"),
    "kernel/modules.builtin": ("built-in-module-list", "utf8-text"),
    "kernel/modules.builtin.modinfo": ("built-in-module-metadata", "binary-records"),
    "kernel/vmlinux": ("kernel-elf", "elf64-aarch64-et-dyn"),
    "provenance/source-repo-manifest.xml": ("source-resolved-manifest", "xml"),
    "provenance/build-receipt.tsv": ("build-receipt", "tsv"),
    "support/compatibility-matrix.xml": ("vendor-compatibility-matrix", "xml"),
    "support/fstab.uml": ("android-fstab", "utf8-text"),
    "support/vintf-manifest.xml": ("vendor-vintf-manifest", "xml"),
}

TOP_LEVEL_KEYS = {
    "schema_version",
    "release_id",
    "directory_name",
    "product_family",
    "acceptance",
    "published_at_utc",
    "android",
    "source",
    "kernel",
    "outer",
    "producer",
    "inventory",
}
ANDROID_KEYS = {
    "major",
    "sdk",
    "build_id",
    "build_number",
    "security_patch",
    "build_variant",
    "fingerprint",
    "vendor_api_level",
    "product",
    "release_config",
}
# Releases published after the ABI list was added carry it; earlier ones do not,
# and still validate. The host shows it as the architectures the image enables.
ANDROID_OPTIONAL_KEYS = {"abi_list"}
ABI_NAME_RE = re.compile(r"[a-z0-9_]+(?:-[a-z0-9_]+)*")
SOURCE_KEYS = {
    "manifest_repository",
    "manifest_revision",
    "manifest_commit",
    "manifest_tree",
    "aosp_tag",
    "base_manifest_sha256",
    "projects_manifest_sha256",
    "routes",
}
ROUTE_KEYS = {
    "route_id",
    "manifest_remote",
    "manifest_project",
    "checkout_path",
    "base_commit",
    "base_tree",
    "final_tree",
    "patch_count",
    "series_manifest_sha256",
}
KERNEL_KEYS = {
    "family",
    "repository",
    "branch",
    "base_commit",
    "base_tree",
    "final_tree",
    "patch_count",
    "patch_manifest_sha256",
    "release",
    "page_size",
    "abi_version",
    "abi_fingerprint",
    "elf_role",
}
OUTER_KEYS = {"revision", "dirty"}
OUTER_KEYS_WITH_INPUTS = OUTER_KEYS | {"input_trees"}
PRODUCER_KEYS = {"entry_point", "functional_options", "builder"}
BUILDER_KEYS = {
    "os",
    "architecture",
    "jdk",
    "clang",
    "linker",
    "darwin_sdk",
    "android_build_tools",
}
INVENTORY_KEYS = {"path", "role", "format", "size"}

# The accepted descriptor's keys in file order, each with whether its value is
# written as a bare number. A schema 7 descriptor adds the input trees.
INPUT_TREES_KEY = "LINEAGE24_ACCEPTED_INPUT_TREES"
ACCEPTANCE_FIELDS = (
    ("LINEAGE24_ACCEPTED_SCHEMA", True),
    ("LINEAGE24_ACCEPTED_RELEASE_ID", False),
    ("LINEAGE24_ACCEPTED_DIRECTORY_NAME", False),
    ("LINEAGE24_ACCEPTED_PRODUCT_FAMILY", False),
    ("LINEAGE24_ACCEPTED_ANDROID_MAJOR", True),
    ("LINEAGE24_ACCEPTED_BUILD_VARIANT", False),
    ("LINEAGE24_ACCEPTED_SOURCE_MANIFEST_COMMIT", False),
    ("LINEAGE24_ACCEPTED_SOURCE_BASE_MANIFEST_SHA256", False),
    ("LINEAGE24_ACCEPTED_SOURCE_PROJECTS_MANIFEST_SHA256", False),
    ("LINEAGE24_ACCEPTED_KERNEL_FINAL_TREE", False),
    ("LINEAGE24_ACCEPTED_KERNEL_PATCH_MANIFEST_SHA256", False),
    ("LINEAGE24_ACCEPTED_OUTER_REVISION", False),
    ("LINEAGE24_ACCEPTED_PAGE_SIZE", True),
    ("LINEAGE24_ACCEPTED_ABI_VERSION", True),
    ("LINEAGE24_ACCEPTED_ABI_FINGERPRINT", False),
)
LEGACY_ACCEPTANCE_KEYS = {key for key, _ in ACCEPTANCE_FIELDS}
ACCEPTANCE_KEYS = LEGACY_ACCEPTANCE_KEYS | {INPUT_TREES_KEY}


def load_manifest(release_root: Path) -> dict:
    path = release_root / "release-manifest.json"
    if not path.is_file() or path.is_symlink():
        fail("release-manifest.json is missing or unsafe")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot read release manifest: {error}")
    return validate_manifest(value)


def validate_manifest(value: object) -> dict:
    manifest = require_exact_keys(value, TOP_LEVEL_KEYS, "release manifest")
    schema_version = manifest["schema_version"]
    if isinstance(schema_version, bool) or schema_version not in SCHEMA_VERSIONS:
        fail("release manifest schema version is unsupported")
    release_id = require_string(manifest["release_id"], "release ID")
    if RELEASE_ID_RE.fullmatch(release_id) is None:
        fail("release ID has an unsafe format")
    directory_name = require_string(manifest["directory_name"], "directory name")
    if directory_name != DIRECTORY_PREFIX + release_id:
        fail("release directory name does not match the release ID")
    if manifest["product_family"] != PRODUCT_FAMILY:
        fail("release product family is not Android 17 mainline 6.18 4 KiB")
    if manifest["acceptance"] != "accepted":
        fail("release is not marked accepted")
    require_string(manifest["published_at_utc"], "publication timestamp")

    android = manifest["android"]
    if isinstance(android, dict) and "abi_list" in android:
        abi_list = android["abi_list"]
        if (
            not isinstance(abi_list, list)
            or not 1 <= len(abi_list) <= 8
            or any(
                not isinstance(abi, str) or ABI_NAME_RE.fullmatch(abi) is None
                for abi in abi_list
            )
            or len(set(abi_list)) != len(abi_list)
        ):
            fail("android.abi_list is not a list of distinct ABI names")
        android = {
            key: item for key, item in android.items()
            if key not in ANDROID_OPTIONAL_KEYS
        }
    android = require_exact_keys(android, ANDROID_KEYS, "android")
    if require_integer(android["major"], "Android major") != 17:
        fail("Android major is not 17")
    if require_integer(android["sdk"], "Android SDK") < 37:
        fail("Android SDK is below 37")
    for key in ("build_id", "build_variant", "fingerprint", "product", "release_config"):
        require_string(android[key], f"android.{key}")
    if android["build_variant"] not in BUILD_VARIANTS:
        fail("Android build variant is not user or userdebug")
    require_release_build_variant(android["build_variant"])
    require_integer(android["vendor_api_level"], "vendor API level", minimum=202604)
    if require_integer(android["build_number"], "Android build number", minimum=1) > 9_999_999_999:
        fail("Android build number has more than ten digits")
    patch_level = require_string(android["security_patch"], "Android security patch level")
    if SECURITY_PATCH_RE.fullmatch(patch_level) is None:
        fail("Android security patch level is not a YYYY-MM-DD date")
    try:
        datetime.date.fromisoformat(patch_level)
    except ValueError:
        fail("Android security patch level is not a calendar date")

    source = require_exact_keys(manifest["source"], SOURCE_KEYS, "source")
    for key in ("manifest_repository", "manifest_revision", "aosp_tag"):
        require_string(source[key], f"source.{key}")
    require_hex(source["manifest_commit"], 40, "source manifest commit")
    require_hex(source["manifest_tree"], 40, "source manifest tree")
    require_hex(source["base_manifest_sha256"], 64, "base manifest SHA-256")
    require_hex(source["projects_manifest_sha256"], 64, "projects manifest SHA-256")
    validate_routes(source["routes"], "source")

    kernel = require_exact_keys(manifest["kernel"], KERNEL_KEYS, "kernel")
    if kernel["family"] != KERNEL_FAMILY:
        fail(f"kernel family is not {KERNEL_FAMILY}")
    for key in ("repository", "branch", "release"):
        require_string(kernel[key], f"kernel.{key}")
    for key in ("base_commit", "base_tree", "final_tree"):
        require_hex(kernel[key], 40, f"kernel.{key}")
    require_hex(kernel["patch_manifest_sha256"], 64, "kernel patch manifest SHA-256")
    require_integer(kernel["patch_count"], "kernel patch count", minimum=1)
    if require_integer(kernel["page_size"], "kernel page size") != 4096:
        fail("kernel page size is not 4096")
    require_integer(kernel["abi_version"], "kernel ABI version", minimum=1)
    abi_fingerprint = require_string(kernel["abi_fingerprint"], "kernel ABI fingerprint")
    if re.fullmatch(r"0x[0-9a-f]{16}", abi_fingerprint) is None:
        fail("kernel ABI fingerprint has an unsafe format")
    if kernel["elf_role"] != "darwin-uml":
        fail("kernel ELF role is not darwin-uml")

    outer = require_exact_keys(
        manifest["outer"],
        OUTER_KEYS_WITH_INPUTS if schema_version == SCHEMA_VERSION else OUTER_KEYS,
        "outer",
    )
    require_hex(outer["revision"], 40, "outer revision")
    if outer["dirty"] is not False:
        fail("accepted release records a dirty outer project")
    if schema_version == SCHEMA_VERSION:
        validate_input_trees(outer["input_trees"])

    producer = require_exact_keys(manifest["producer"], PRODUCER_KEYS, "producer")
    if producer["entry_point"] != "android/lineage-24/build/publish-release.sh":
        fail("release records an unexpected producer entry point")
    options = producer["functional_options"]
    if not isinstance(options, list) or not options:
        fail("producer functional options must be a nonempty array")
    for index, option in enumerate(options):
        require_string(option, f"producer option {index}")
    builder = require_exact_keys(producer["builder"], BUILDER_KEYS, "producer.builder")
    for key in BUILDER_KEYS:
        value = require_string(builder[key], f"producer.builder.{key}")
        if re.search(r"(^|[ (])/[^ )]+", value):
            fail(f"producer.builder.{key} stores an absolute filesystem path")
    if builder["architecture"] != "x86_64":
        fail("release was not produced on an x86_64 builder")

    inventory = manifest["inventory"]
    if not isinstance(inventory, list):
        fail("release inventory must be an array")
    expected_paths = set(PAYLOAD_SPECS)
    seen_paths: set[str] = set()
    ordered_paths: list[str] = []
    for index, raw_entry in enumerate(inventory):
        entry = require_exact_keys(raw_entry, INVENTORY_KEYS, f"inventory entry {index}")
        path = safe_relative(entry["path"], f"inventory entry {index} path")
        if path in seen_paths:
            fail(f"duplicate inventory path: {path}")
        seen_paths.add(path)
        ordered_paths.append(path)
        if path not in PAYLOAD_SPECS:
            fail(f"undeclared release payload: {path}")
        role, file_format = PAYLOAD_SPECS[path]
        if entry["role"] != role or entry["format"] != file_format:
            fail(f"inventory role or format differs for {path}")
        require_integer(entry["size"], f"inventory size for {path}", minimum=1)
    if seen_paths != expected_paths:
        fail(f"release inventory differs from its fixed contract: {sorted(expected_paths - seen_paths)}")
    if ordered_paths != sorted(ordered_paths):
        fail("release inventory is not sorted by path")
    return manifest


def validate_input_trees(trees: object) -> None:
    """Check the release's recorded object IDs of its input paths."""
    if not isinstance(trees, dict) or not trees:
        fail("outer.input_trees must be a nonempty object")
    for path, object_id in trees.items():
        if (
            not isinstance(path, str)
            or not release_inputs.normalized(path)
            or not isinstance(object_id, str)
            or release_inputs.OBJECT_ID_RE.fullmatch(object_id) is None
        ):
            fail(f"outer.input_trees has a malformed entry: {path!r}")


def validate_routes(routes: object, label: str) -> None:
    """Check one route table of a release manifest."""
    if not isinstance(routes, list) or not routes:
        fail(f"{label} routes must be a nonempty array")
    seen_routes: set[str] = set()
    for index, raw_route in enumerate(routes):
        route = require_exact_keys(raw_route, ROUTE_KEYS, f"{label} route {index}")
        route_id = require_string(route["route_id"], f"{label} route {index} ID")
        if route_id in seen_routes:
            fail(f"duplicate {label} route ID: {route_id}")
        seen_routes.add(route_id)
        require_string(route["manifest_remote"], f"{label} route {route_id} remote")
        for key in ("manifest_project", "checkout_path"):
            safe_relative(route[key], f"{label} route {route_id} {key}")
        for key in ("base_commit", "base_tree", "final_tree"):
            require_hex(route[key], 40, f"{label} route {route_id} {key}")
        require_integer(
            route["patch_count"], f"{label} route {route_id} patch count", minimum=1
        )
        require_hex(
            route["series_manifest_sha256"], 64,
            f"{label} route {route_id} series manifest SHA-256",
        )


def kernel_series_provenance(outer_root: Path) -> tuple[int, str]:
    """Return the mainline line's patch count and ordered patch-content digest.

    The count and digest cover the shared series and the line's overlay, which
    together turn the pinned upstream commit into the line's tree.
    """
    tooling = str(outer_root / "kernel/common/tooling")
    if tooling not in sys.path:
        sys.path.insert(0, tooling)
    import kernel_series  # noqa: PLC0415

    try:
        return kernel_series.provenance(
            kernel_series.load_line(outer_root / KERNEL_METADATA)
        )
    except kernel_series.SeriesError as error:
        fail(f"kernel series are invalid: {error}")


def series_provenance(outer_root: Path, *directories: Path) -> tuple[int, str]:
    """Return the patch count and ordered patch-content digest of generated series."""
    scripts = str(outer_root / "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    import patch_series  # noqa: PLC0415

    try:
        return patch_series.series_provenance(list(directories))
    except patch_series.SeriesError as error:
        fail(f"generated patch series are invalid: {error}")


def read_routes(projects: Path, android_root: Path, label: str) -> list[dict]:
    """Return a route table's release-manifest routes, with series provenance."""
    lines = [
        line for line in projects.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    routes = []
    for row in csv.DictReader(lines, delimiter="\t"):
        patch_count, series_digest = series_provenance(
            android_root.parents[1], android_root / row["series_dir"]
        )
        routes.append({
            "route_id": row["route_id"],
            "manifest_remote": row["manifest_remote"],
            "manifest_project": row["manifest_project"],
            "checkout_path": row["checkout_path"],
            "base_commit": row["base_commit"],
            "base_tree": row["base_tree"],
            "final_tree": row["final_tree"],
            "patch_count": patch_count,
            "series_manifest_sha256": series_digest,
        })
    if not routes:
        fail(f"{label} projects manifest has no routes")
    return routes


# The tracked kernel source of the Android product: the mainline line, built
# with its Android overlay.
KERNEL_METADATA = "kernel/mainline-6.18/patches/metadata.env"
KERNEL_CONFIG_DIRECTORY = "kernel/mainline-6.18/config"

TRACKED_SECTIONS = {
    "android": "Android",
    "source": "source",
    "kernel": "kernel",
    "outer": "outer project",
}


def tracked_identity(android_root: Path) -> dict[str, dict]:
    """Return the release-manifest fields the tracked tree determines.

    The publisher adds each section's per-build fields: the source routes,
    and the kernel's release, page size and ELF role, and the outer project's
    revision. The input trees are the object IDs of the listed input paths at
    the outer project's HEAD, which the publisher requires clean. The verifier
    compares a release published from this tree against these fields.
    """
    outer_root = android_root.parents[1]
    lineage = parse_env(android_root / "metadata/lineage.env")
    kernel = parse_env(outer_root / KERNEL_METADATA)
    abi = parse_env(outer_root / "kernel/common/abi/identity.env")
    kernel_count, kernel_digest = kernel_series_provenance(outer_root)
    try:
        input_trees = release_inputs.object_ids(
            outer_root,
            release_inputs.input_paths(outer_root / release_inputs.LIST_RELATIVE),
        )
    except release_inputs.InputError as error:
        fail(f"cannot record the release input trees: {error}")
    return {
        "android": {"build_variant": configured_build_variant(android_root)},
        "source": {
            "manifest_repository": lineage["LINEAGE24_MANIFEST_REPOSITORY"],
            "manifest_revision": lineage["LINEAGE24_MANIFEST_REVISION"],
            "manifest_commit": lineage["LINEAGE24_MANIFEST_COMMIT"],
            "manifest_tree": lineage["LINEAGE24_MANIFEST_TREE"],
            "aosp_tag": lineage["LINEAGE24_AOSP_TAG"],
            "base_manifest_sha256": lineage["LINEAGE24_BASE_MANIFEST_SHA256"],
            "projects_manifest_sha256": sha256_path(android_root / "patches/projects.tsv"),
        },
        "kernel": {
            "family": KERNEL_FAMILY,
            "repository": kernel["UML_KERNEL_UPSTREAM_REPOSITORY"],
            "branch": kernel["UML_KERNEL_UPSTREAM_BRANCH"],
            "base_commit": kernel["UML_KERNEL_UPSTREAM_COMMIT"],
            "base_tree": kernel["UML_KERNEL_UPSTREAM_TREE"],
            "final_tree": kernel["UML_KERNEL_FINAL_TREE"],
            "patch_count": kernel_count,
            "patch_manifest_sha256": kernel_digest,
            "abi_version": int(abi["DARWIN_UML_ABI_VERSION"]),
            "abi_fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
        },
        "outer": {"input_trees": input_trees},
    }


def verify_tracked_identity(manifest: dict, android_root: Path) -> None:
    if manifest["schema_version"] != SCHEMA_VERSION:
        fail(
            f"a schema {manifest['schema_version']} release records no input "
            "trees, so it cannot be the tracked tree's own release"
        )
    for section, expected in tracked_identity(android_root).items():
        for key, value in expected.items():
            if manifest[section][key] != value:
                fail(
                    f"release {TRACKED_SECTIONS[section]} identity differs "
                    f"from tracked {key}"
                )


def acceptance_values(manifest: dict) -> dict[str, str]:
    """Return the accepted descriptor's values for a manifest, by key."""
    values = {
        "LINEAGE24_ACCEPTED_SCHEMA": str(manifest["schema_version"]),
        "LINEAGE24_ACCEPTED_RELEASE_ID": manifest["release_id"],
        "LINEAGE24_ACCEPTED_DIRECTORY_NAME": manifest["directory_name"],
        "LINEAGE24_ACCEPTED_PRODUCT_FAMILY": manifest["product_family"],
        "LINEAGE24_ACCEPTED_ANDROID_MAJOR": str(manifest["android"]["major"]),
        "LINEAGE24_ACCEPTED_BUILD_VARIANT": manifest["android"]["build_variant"],
        "LINEAGE24_ACCEPTED_SOURCE_MANIFEST_COMMIT": manifest["source"]["manifest_commit"],
        "LINEAGE24_ACCEPTED_SOURCE_BASE_MANIFEST_SHA256": manifest["source"]["base_manifest_sha256"],
        "LINEAGE24_ACCEPTED_SOURCE_PROJECTS_MANIFEST_SHA256": manifest["source"]["projects_manifest_sha256"],
        "LINEAGE24_ACCEPTED_KERNEL_FINAL_TREE": manifest["kernel"]["final_tree"],
        "LINEAGE24_ACCEPTED_KERNEL_PATCH_MANIFEST_SHA256": manifest["kernel"]["patch_manifest_sha256"],
        "LINEAGE24_ACCEPTED_OUTER_REVISION": manifest["outer"]["revision"],
        "LINEAGE24_ACCEPTED_PAGE_SIZE": str(manifest["kernel"]["page_size"]),
        "LINEAGE24_ACCEPTED_ABI_VERSION": str(manifest["kernel"]["abi_version"]),
        "LINEAGE24_ACCEPTED_ABI_FINGERPRINT": manifest["kernel"]["abi_fingerprint"],
    }
    if manifest["schema_version"] == SCHEMA_VERSION:
        values[INPUT_TREES_KEY] = release_inputs.render_trees(manifest["outer"]["input_trees"])
    return values


def render_acceptance(manifest: dict) -> str:
    """Return the accepted-release.env text that selects a release."""
    values = acceptance_values(manifest)
    lines = [
        "# SPDX-License-Identifier: GPL-2.0-or-later",
        "# Reviewed static LineageOS 24 mainline 6.18 4 KiB runtime release identity.",
    ]
    for key, numeric in ACCEPTANCE_FIELDS:
        lines.append(f"{key}={values[key]}" if numeric else f"{key}='{values[key]}'")
    if INPUT_TREES_KEY in values:
        lines.append(f"{INPUT_TREES_KEY}='{values[INPUT_TREES_KEY]}'")
    return "\n".join(lines) + "\n"


def verify_acceptance(manifest: dict, acceptance_path: Path) -> None:
    acceptance = parse_env(acceptance_path)
    expected = acceptance_values(manifest)
    if set(acceptance) != set(expected):
        fail("accepted-release descriptor keys differ from the fixed contract")
    for key, value in expected.items():
        if acceptance.get(key) != value:
            fail(f"release differs from accepted descriptor field {key}")


def inventory_map(manifest: dict) -> dict[str, dict]:
    return {entry["path"]: entry for entry in manifest["inventory"]}


def verify_inventory(release_root: Path, manifest: dict) -> None:
    expected_files = set(PAYLOAD_SPECS) | {"release-manifest.json"}
    expected_dirs = {"."}
    for relative in expected_files:
        parent = PurePosixPath(relative).parent
        while str(parent) != ".":
            expected_dirs.add(str(parent))
            parent = parent.parent

    actual_files: set[str] = set()
    actual_dirs: set[str] = {"."}
    for root, directories, files in os.walk(release_root, topdown=True, followlinks=False):
        root_path = Path(root)
        relative_root = root_path.relative_to(release_root)
        for name in directories:
            path = root_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
                fail(f"unsafe release directory: {path.relative_to(release_root)}")
            actual_dirs.add(str((relative_root / name).as_posix()))
        for name in files:
            path = root_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
                fail(f"unsafe release payload: {path.relative_to(release_root)}")
            actual_files.add(str((relative_root / name).as_posix()))
    if actual_files != expected_files:
        fail(
            "release file set differs from its manifest "
            f"(missing={sorted(expected_files - actual_files)}, "
            f"extra={sorted(actual_files - expected_files)})"
        )
    if actual_dirs != expected_dirs:
        fail(
            "release directory set differs from its manifest "
            f"(missing={sorted(expected_dirs - actual_dirs)}, "
            f"extra={sorted(actual_dirs - expected_dirs)})"
        )

    entries = inventory_map(manifest)
    for relative in PAYLOAD_SPECS:
        observed = (release_root / relative).stat().st_size
        if observed != entries[relative]["size"]:
            fail(f"release payload size differs for {relative}")


def verify_kernel_config(
    release_root: Path,
    android_root: Path,
    *,
    enforce_tracked_policy: bool,
) -> None:
    config = parse_config(release_root / "kernel/config")
    if config.get("CONFIG_MODULES") != "n":
        fail("release kernel does not have CONFIG_MODULES=n")
    module_values = sorted(key for key, value in config.items() if value == "m")
    if module_values:
        fail(f"release kernel contains loadable-module values: {module_values[:8]}")
    if not enforce_tracked_policy:
        return
    # The mainline line owns the Android product's configuration contract.
    line_config = android_root.parents[1] / KERNEL_CONFIG_DIRECTORY
    verify_config_policy(
        release_root / "kernel/config",
        line_config / "android-required-builtins.txt",
        line_config / "android-forbidden-options.txt",
    )


def run_checked(arguments: list[str], *, env: dict[str, str] | None = None) -> None:
    result = subprocess.run(arguments, env=env, check=False)
    if result.returncode != 0:
        fail(f"verification command failed ({result.returncode}): {arguments[0]}")


def run_checked_capture(arguments: list[str]) -> str:
    result = subprocess.run(
        arguments,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        fail(
            f"verification command failed ({result.returncode}): "
            f"{arguments[0]} {result.stderr.strip()}"
        )
    return result.stdout.strip()


def verify_disk_boot_payloads(
    disk: Path,
    layout: Path,
    android_root: Path,
    *,
    structural: bool,
) -> None:
    boot_tool = android_root / "tools/boot-image.py"
    for partition in ("init_boot", "recovery"):
        run_checked([
            str(boot_tool),
            "verify",
            "--layout",
            str(layout),
            "--disk",
            str(disk),
            "--partition",
            partition,
        ])
    raw_range = run_checked_capture([
        str(boot_tool),
        "range",
        "--layout",
        str(layout),
        "--disk",
        str(disk),
        "--partition",
        "init_boot",
    ])
    try:
        ramdisk = json.loads(raw_range)
        offset = ramdisk["ramdisk_offset"]
        size = ramdisk["ramdisk_size"]
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        fail(f"cannot parse verified init_boot ramdisk range: {error}")
    if (
        isinstance(offset, bool)
        or not isinstance(offset, int)
        or isinstance(size, bool)
        or not isinstance(size, int)
        or offset < 0
        or size <= 0
    ):
        fail("verified init_boot ramdisk range is invalid")
    arguments = [str(android_root / "tools/verify-initramfs.py")]
    if structural:
        arguments.append("--structural")
    arguments.extend(["--offset", str(offset), "--size", str(size), str(disk)])
    run_checked(arguments)
    run_checked([
        str(android_root / "tools/verify-lineage-recovery.py"),
        "--android-root",
        str(android_root),
        "--layout",
        str(layout),
        "--disk",
        str(disk),
    ])


def verify_semantics(
    release_root: Path,
    android_root: Path,
    *,
    enforce_tracked_policy: bool,
) -> None:
    run_checked([
        str(android_root / "build/verify-kernel-elf.py"),
        str(release_root / "kernel/vmlinux"),
    ])
    verify_kernel_config(
        release_root,
        android_root,
        enforce_tracked_policy=enforce_tracked_policy,
    )
    if enforce_tracked_policy:
        if (release_root / "boot/kernel-command-line.txt").read_bytes() != (
            android_root / "support/boot/kernel-command-line.txt"
        ).read_bytes():
            fail("release kernel command line differs from tracked policy")

    fstab = (release_root / "support/fstab.uml").read_text(encoding="utf-8")
    for name in (
        "system",
        "system_ext",
        "product",
        "vendor",
        "odm",
        "metadata",
        "userdata",
        "misc",
    ):
        if f"/dev/block/by-name/{name}" not in fstab:
            fail(f"release fstab is missing fixed partition {name}")
    for forbidden in ("logical", "slotselect", "avb", "_dlkm"):
        if forbidden in fstab:
            fail(f"release fstab contains unsupported token: {forbidden}")

    for relative in (
        "support/vintf-manifest.xml",
        "support/compatibility-matrix.xml",
        "provenance/source-repo-manifest.xml",
    ):
        try:
            ET.parse(release_root / relative)
        except (OSError, ET.ParseError) as error:
            fail(f"invalid XML payload {relative}: {error}")

    receipt = (release_root / "provenance/build-receipt.tsv").read_text(
        encoding="utf-8"
    )
    for forbidden in ("/home/", "builder_host", "private_key", "\tssh\t", "\tscp\t"):
        if forbidden in receipt.lower():
            fail("build receipt contains builder endpoint, credential, or local path state")

    # The system disk is checked whole: its GPT, every partition's SHA-256,
    # and, for a release the tracked tree builds, that its layout is what the
    # tracked disk policy produces.
    disk = release_root / "guest/system.img"
    layout = release_root / "guest/partition-layout.json"
    arguments = [
        str(android_root / "tools/ubd-disk.py"),
        "verify",
        "--layout",
        str(layout),
        "--disk",
        str(disk),
        "--hash",
    ]
    if enforce_tracked_policy:
        arguments += ["--policy", str(android_root / "support/image/disk-policy.json")]
    run_checked(arguments)
    verify_disk_boot_payloads(
        disk,
        layout,
        android_root,
        structural=not enforce_tracked_policy,
    )

    schema = android_root / "support/release/release-manifest.schema.json"
    if not schema.is_file():
        fail("tracked release-manifest schema is missing")


def verify_release(
    release_root: Path,
    android_root: Path,
    *,
    acceptance_path: Path | None = None,
    semantic: bool = True,
) -> dict:
    if not release_root.is_absolute():
        fail("release source must be an absolute local path")
    if not release_root.is_dir() or release_root.is_symlink():
        fail("release source must be a regular local directory")
    manifest = load_manifest(release_root)
    if release_root.name != manifest["directory_name"]:
        fail("release directory basename differs from its manifest")
    if acceptance_path is None:
        verify_tracked_identity(manifest, android_root)
    else:
        verify_acceptance(manifest, acceptance_path)
        # The kernel identity a release records is the build that qualified its
        # target files. Keep it as immutable historical provenance, but do not
        # compare it to the current host/kernel bridge. The independently
        # accepted kernel artifact owns that compatibility check now, allowing
        # an old rootfs bundle's writable disk to survive a kernel-only
        # application update.
    verify_inventory(release_root, manifest)
    if semantic:
        verify_semantics(
            release_root,
            android_root,
            enforce_tracked_policy=acceptance_path is None,
        )
    return manifest
