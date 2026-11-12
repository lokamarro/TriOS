#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import datetime
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import NoReturn

from contract_checks import ContractChecks, parse_config

_ABI = str(Path(__file__).resolve().parents[1] / "abi")
if _ABI not in sys.path:
    sys.path.insert(0, _ABI)
from bridge_imports import (  # noqa: E402
    SHT_NOBITS,
    bounded_slice,
    section_headers,
    undefined_symbols,
    verify_imports,
)


SCHEMA_VERSION = 3
# Every kernel runs 4 KiB guest pages; the manifest records the size so a
# consumer checks the profile it was built for.
GUEST_PAGE_SIZE = 4096
RELEASE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
ABI_FINGERPRINT_RE = re.compile(r"^0x[0-9a-f]{16}$")
TOP_LEVEL_KEYS = {
    "schema_version",
    "release_id",
    "directory_name",
    "family",
    "slice",
    "published_at_utc",
    "release",
    "page_size",
    "elf_role",
    "source",
    "abi",
    "inventory",
}
SOURCE_KEYS = {
    "repository",
    "branch",
    "base_commit",
    "base_tree",
    "final_tree",
}
ABI_KEYS = {"version", "fingerprint"}
INVENTORY_KEYS = {"path", "role", "format", "size"}

PAYLOAD_SPECS = {
    "kernel/System.map": ("kernel-symbol-map", "utf8-text"),
    "kernel/config": ("kernel-config", "kconfig"),
    "kernel/kernel.release": ("kernel-release", "utf8-text"),
    "kernel/modules.builtin": ("built-in-module-list", "utf8-text"),
    "kernel/modules.builtin.modinfo": (
        "built-in-module-metadata",
        "binary-records",
    ),
    "kernel/vmlinux": ("kernel-elf", "elf64-aarch64-et-dyn"),
}

_CHECKS = ContractChecks("kernel artifact verifier")
error = _CHECKS.error
fail = _CHECKS.fail
require_exact_keys = _CHECKS.require_exact_keys
require_string = _CHECKS.require_string
require_integer = _CHECKS.require_integer
require_hex = _CHECKS.require_hex
safe_relative = _CHECKS.safe_relative
parse_env = _CHECKS.parse_env

ET_DYN = 3
EM_AARCH64 = 183
PT_LOAD = 1
PT_GNU_STACK = 0x6474E551
PF_X = 1
PF_W = 2
SHF_ALLOC = 2
DWARF_SECTION_PREFIXES = (".debug_", ".zdebug_")


def validate_manifest(value: object) -> dict:
    manifest = require_exact_keys(value, TOP_LEVEL_KEYS, "artifact manifest")
    if manifest["schema_version"] != SCHEMA_VERSION:
        fail("artifact manifest schema version is unsupported")
    release_id = require_string(manifest["release_id"], "release ID")
    if RELEASE_ID_RE.fullmatch(release_id) is None:
        fail("release ID has an unsafe format")
    family = require_string(manifest["family"], "kernel family")
    if family not in ("mainline-6.18", "android-6.18"):
        fail("kernel family is unsupported")
    kernel_slice = require_string(manifest["slice"], "kernel slice")
    if kernel_slice not in ("arm64", "x86_64"):
        fail("kernel slice is unsupported")
    page_size = require_integer(manifest["page_size"], "kernel page size")
    if page_size != GUEST_PAGE_SIZE:
        fail("kernel page size is unsupported")
    if kernel_slice == "x86_64" and family != "mainline-6.18":
        fail("the x86_64 kernel slice requires the mainline-6.18 family")
    if family == "android-6.18" and kernel_slice != "arm64":
        fail("the android-6.18 family requires the arm64 slice")
    directory_name = require_string(manifest["directory_name"], "directory name")
    expected_directory = (
        f"{family}-{kernel_slice}-{page_size // 1024}k-{release_id}"
    )
    if directory_name != expected_directory:
        fail(
            "artifact directory name does not match family, slice, page size, "
            "and release ID"
        )
    require_string(manifest["published_at_utc"], "publication timestamp")
    require_string(manifest["release"], "kernel release")
    if manifest["elf_role"] != "darwin-uml":
        fail("kernel ELF role is not darwin-uml")

    source = require_exact_keys(manifest["source"], SOURCE_KEYS, "source")
    for key in ("repository", "branch"):
        require_string(source[key], f"source.{key}")
    for key in ("base_commit", "base_tree", "final_tree"):
        require_hex(source[key], 40, f"source.{key}")

    abi = require_exact_keys(manifest["abi"], ABI_KEYS, "abi")
    require_integer(abi["version"], "abi.version", minimum=1)
    fingerprint = require_string(abi["fingerprint"], "abi.fingerprint")
    if ABI_FINGERPRINT_RE.fullmatch(fingerprint) is None:
        fail("ABI fingerprint has an unsafe format")

    inventory = manifest["inventory"]
    if not isinstance(inventory, list):
        fail("artifact inventory must be an array")
    seen: set[str] = set()
    ordered: list[str] = []
    for index, raw_entry in enumerate(inventory):
        entry = require_exact_keys(raw_entry, INVENTORY_KEYS, f"inventory entry {index}")
        relative = safe_relative(entry["path"], f"inventory entry {index} path")
        if relative in seen:
            fail(f"duplicate inventory path: {relative}")
        if relative not in PAYLOAD_SPECS:
            fail(f"undeclared kernel artifact payload: {relative}")
        expected_role, expected_format = PAYLOAD_SPECS[relative]
        if entry["role"] != expected_role or entry["format"] != expected_format:
            fail(f"inventory role or format differs for {relative}")
        require_integer(entry["size"], f"inventory size for {relative}", minimum=1)
        seen.add(relative)
        ordered.append(relative)
    if seen != set(PAYLOAD_SPECS):
        fail("artifact inventory differs from its fixed payload contract")
    if ordered != sorted(ordered):
        fail("artifact inventory is not sorted by path")
    return manifest


def load_manifest(artifact_root: Path) -> dict:
    path = artifact_root / "kernel-artifact.json"
    if not path.is_file() or path.is_symlink():
        fail("kernel-artifact.json is missing or unsafe")
    try:
        return validate_manifest(json.loads(path.read_text(encoding="utf-8")))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot read artifact manifest: {error}")


def verify_inventory(artifact_root: Path, manifest: dict) -> None:
    expected_files = set(PAYLOAD_SPECS) | {"kernel-artifact.json"}
    expected_dirs = {".", "kernel"}
    actual_files: set[str] = set()
    actual_dirs: set[str] = {"."}
    for root, directories, files in os.walk(artifact_root, topdown=True, followlinks=False):
        root_path = Path(root)
        relative_root = root_path.relative_to(artifact_root)
        for name in directories:
            path = root_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
                fail(f"unsafe artifact directory: {path.relative_to(artifact_root)}")
            actual_dirs.add(str((relative_root / name).as_posix()))
        for name in files:
            path = root_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
                fail(f"unsafe artifact payload: {path.relative_to(artifact_root)}")
            actual_files.add(str((relative_root / name).as_posix()))
    if actual_files != expected_files or actual_dirs != expected_dirs:
        fail(
            "artifact tree differs from its fixed contract "
            f"(missing files={sorted(expected_files - actual_files)}, "
            f"extra files={sorted(actual_files - expected_files)}, "
            f"extra directories={sorted(actual_dirs - expected_dirs)})"
        )
    entries = {entry["path"]: entry for entry in manifest["inventory"]}
    for relative in PAYLOAD_SPECS:
        if (artifact_root / relative).stat().st_size != entries[relative]["size"]:
            fail(f"artifact payload size differs for {relative}")


def verify_config(path: Path, kernel_slice: str) -> None:
    if kernel_slice not in ("arm64", "x86_64"):
        fail("kernel config slice is unsupported")
    config = parse_config(path)
    expected = {
        "CONFIG_64BIT": "y",
        "CONFIG_ARM64": "y",
        "CONFIG_UML": "y",
        "CONFIG_MODULES": "n",
        "CONFIG_PAGE_SHIFT": "12",
        "CONFIG_PAGE_SIZE_4KB": "y",
        "CONFIG_UML_X86_64_USERSPACE_ONLY": (
            "y" if kernel_slice == "x86_64" else "n"
        ),
        "CONFIG_BPF": "y",
        "CONFIG_BPF_SYSCALL": "y",
        "CONFIG_CGROUP_BPF": "y",
        "CONFIG_DEBUG_INFO_BTF": "y",
    }
    for symbol, wanted in expected.items():
        observed = config.get(symbol, "n")
        if observed != wanted:
            fail(f"kernel config {symbol}={observed}, expected {wanted}")
    modular = sorted(symbol for symbol, value in config.items() if value == "m")
    if modular:
        fail(f"kernel artifact contains loadable-module values: {modular[:8]}")


def verify_config_policy(
    config_path: Path, required_path: Path, forbidden_path: Path
) -> None:
    """Check a kernel config against a product's two symbol lists.

    Every symbol of the required list must resolve to y, and every symbol of the
    forbidden list must be disabled or absent. A list holds one symbol per line;
    blank lines and comments are ignored.
    """
    config = parse_config(config_path)
    for list_path, wanted in ((required_path, "y"), (forbidden_path, "n")):
        if not list_path.is_file() or list_path.is_symlink():
            fail(f"config policy list is missing or unsafe: {list_path}")
        for raw in list_path.read_text(encoding="utf-8").splitlines():
            symbol = raw.strip()
            if not symbol or symbol.startswith("#"):
                continue
            observed = config.get(symbol, "n")
            if observed != wanted:
                fail(f"kernel config {symbol}={observed}, expected {wanted}")


def verify_elf_geometry(
    payload: bytes,
    error: Callable[[str], BaseException] = _CHECKS.error,
) -> None:
    """Check a kernel image's AArch64 ET_DYN layout and separated W^X loads."""
    if len(payload) < 64 or payload[:7] != b"\x7fELF\x02\x01\x01":
        raise error("kernel is not a little-endian ELF64 image")
    elf_type, machine = struct.unpack_from("<HH", payload, 16)
    if elf_type != ET_DYN or machine != EM_AARCH64:
        raise error("kernel is not an AArch64 ET_DYN image")
    program_offset = struct.unpack_from("<Q", payload, 32)[0]
    program_entry_size, program_count = struct.unpack_from("<HH", payload, 54)
    if program_entry_size < 56:
        raise error("ELF program-header entry size is malformed")
    bounded_slice(
        payload,
        program_offset,
        program_entry_size * program_count,
        "program-header table",
        error,
    )
    load_count = 0
    executable = False
    writable = False
    for index in range(program_count):
        offset = program_offset + index * program_entry_size
        kind, flags = struct.unpack_from("<II", payload, offset)
        file_offset, virtual_address = struct.unpack_from("<QQ", payload, offset + 8)
        file_size, memory_size, alignment = struct.unpack_from(
            "<QQQ", payload, offset + 32
        )
        if file_size > memory_size:
            raise error(f"ELF program header {index} has file size above memory size")
        bounded_slice(payload, file_offset, file_size, f"segment {index}", error)
        if kind == PT_GNU_STACK and flags & PF_X:
            raise error("kernel GNU stack is executable")
        if kind != PT_LOAD:
            continue
        load_count += 1
        if flags & PF_W and flags & PF_X:
            raise error(f"ELF load segment {index} is writable and executable")
        if alignment < GUEST_PAGE_SIZE or alignment & (alignment - 1):
            raise error(f"ELF load segment {index} has invalid alignment {alignment:#x}")
        if virtual_address % alignment != file_offset % alignment:
            raise error(f"ELF load segment {index} violates alignment congruence")
        executable |= bool(flags & PF_X)
        writable |= bool(flags & PF_W)
    if load_count < 2 or not executable or not writable:
        raise error("kernel lacks separated executable and writable load segments")


def verify_elf(path: Path, common_root: Path, kernel_slice: str) -> bytes:
    """Check the kernel's ELF geometry and bridge imports; return its bytes."""
    payload = path.read_bytes()
    verify_elf_geometry(payload)
    verify_imports(
        undefined_symbols(payload, error),
        kernel_slice,
        common_root / "abi",
        error,
    )
    return payload


def elf_sections(payload: bytes) -> list[tuple[str, int, int, int]]:
    """Return (name, type, flags, size) for every ELF64 section."""
    names_index = struct.unpack_from("<H", payload, 62)[0]
    headers = section_headers(payload, error)
    if names_index >= len(headers):
        fail("ELF section-header table is missing or malformed")
    names_header = headers[names_index]
    if names_header.kind == SHT_NOBITS:
        fail("ELF section-name table has no contents")
    names = bounded_slice(
        payload, names_header.offset, names_header.size, "section-name table", error
    )
    sections = []
    for header in headers:
        terminator = names.find(b"\0", header.name)
        if header.name >= len(names) or terminator < 0:
            fail("ELF section name is outside its string table")
        sections.append(
            (
                names[header.name : terminator].decode("ascii", "replace"),
                header.kind,
                header.flags,
                header.size,
            )
        )
    return sections


def verify_runtime_image(payload: bytes) -> None:
    """Check the shipped kernel image carries BTF and no DWARF.

    DWARF is only pahole's input for BTF; nothing at runtime reads it.
    .BTF is allocated kernel data behind /sys/kernel/btf/vmlinux, which
    verify_config requires on every line.
    """
    sections = {
        name: (kind, flags, size)
        for name, kind, flags, size in elf_sections(payload)
    }
    dwarf = sorted(
        name for name in sections if name.startswith(DWARF_SECTION_PREFIXES)
    )
    if dwarf:
        fail(f"kernel image carries DWARF sections: {', '.join(dwarf[:8])}")
    btf = sections.get(".BTF")
    if btf is None or btf[0] == SHT_NOBITS or btf[2] == 0:
        fail("kernel config enables BTF but the image has no .BTF contents")
    if not btf[1] & SHF_ALLOC:
        fail("kernel .BTF section is not allocated")


def publish_runtime_image(objcopy: Path, source: Path, destination: Path) -> None:
    """Copy a built vmlinux without its DWARF.

    --strip-debug removes the debugging sections and the .symtab entries that
    only describe them: STT_FILE symbols and locals defined in .debug_*
    sections. Every other .symtab symbol, .dynsym, .BTF, and every loaded
    segment remain, so System.map and symbolization are unaffected.
    """
    result = subprocess.run(
        [str(objcopy), "--strip-debug", str(source), str(destination)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        fail(f"could not strip DWARF from the kernel image: {result.stderr.strip()}")
    shutil.copymode(source, destination)


def tracked_line(kernel_metadata_path: Path) -> dict[str, str]:
    """Return a kernel line's metadata, with an overlay's inherited identity."""
    # Imported here so that compatible verification, which reads no line
    # metadata, does not load the series tooling.
    tooling = str(Path(__file__).resolve().parents[1] / "tooling")
    if tooling not in sys.path:
        sys.path.insert(0, tooling)
    import kernel_series  # noqa: PLC0415

    try:
        return kernel_series.load_line(kernel_metadata_path.absolute()).values
    except kernel_series.SeriesError as error:
        fail(str(error))


def verify_tracked_identity(
    manifest: dict,
    kernel_metadata_path: Path,
    abi_metadata_path: Path,
) -> None:
    kernel = tracked_line(kernel_metadata_path)
    expected_source = {
        "repository": kernel["UML_KERNEL_UPSTREAM_REPOSITORY"],
        "branch": kernel["UML_KERNEL_UPSTREAM_BRANCH"],
        "base_commit": kernel["UML_KERNEL_UPSTREAM_COMMIT"],
        "base_tree": kernel["UML_KERNEL_UPSTREAM_TREE"],
        "final_tree": kernel["UML_KERNEL_FINAL_TREE"],
    }
    for key, value in expected_source.items():
        if manifest["source"][key] != value:
            fail(f"artifact source identity differs from tracked {key}")
    verify_abi_identity(manifest, abi_metadata_path)


def verify_abi_identity(manifest: dict, abi_metadata_path: Path) -> None:
    abi = parse_env(abi_metadata_path)
    expected_abi = {
        "version": int(abi["DARWIN_UML_ABI_VERSION"]),
        "fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
    }
    for key, value in expected_abi.items():
        if manifest["abi"][key] != value:
            fail(f"artifact ABI identity differs from tracked {key}")


def verify_descriptor(manifest: dict, descriptor_path: Path) -> None:
    """Check that an accepted-*.env descriptor still repeats its bundle.

    A descriptor names the bundle an Apple configure imports and keeps the
    bundle's identity as provenance for the commit that accepted it. Configure
    reads only the directory name, so this check is what stops the rest from
    drifting away from the manifest.
    """
    expected = {
        "SCHEMA": manifest["schema_version"],
        "RELEASE_ID": manifest["release_id"],
        "DIRECTORY_NAME": manifest["directory_name"],
        "FAMILY": manifest["family"],
        "SLICE": manifest["slice"],
        "RELEASE": manifest["release"],
        "PAGE_SIZE": manifest["page_size"],
        "FINAL_TREE": manifest["source"]["final_tree"],
        "ABI_VERSION": manifest["abi"]["version"],
        "ABI_FINGERPRINT": manifest["abi"]["fingerprint"],
    }
    descriptor = parse_env(descriptor_path)
    expected = {f"UML_KERNEL_ARTIFACT_{key}": str(value) for key, value in expected.items()}
    if set(descriptor) != set(expected):
        fail(
            f"accepted descriptor {descriptor_path} keys differ "
            f"(missing={sorted(set(expected) - set(descriptor))}, "
            f"extra={sorted(set(descriptor) - set(expected))})"
        )
    for key, value in expected.items():
        if descriptor[key] != value:
            fail(
                f"accepted descriptor {descriptor_path} has {key}="
                f"{descriptor[key]}, but the bundle manifest has {value}"
            )


def verify_compatible_identity(
    manifest: dict,
    abi_metadata_path: Path,
    expected_family: str,
    expected_slice: str,
    expected_release_series: str,
    descriptor_path: Path | None = None,
) -> None:
    verify_abi_identity(manifest, abi_metadata_path)
    if manifest["family"] != expected_family:
        fail(
            f"artifact family {manifest['family']} differs from "
            f"required {expected_family}"
        )
    if manifest["slice"] != expected_slice:
        fail(
            f"artifact slice {manifest['slice']} differs from "
            f"required {expected_slice}"
        )
    release = manifest["release"]
    release_pattern = re.compile(
        rf"^{re.escape(expected_release_series)}(?:$|[.-].*)"
    )
    if release_pattern.fullmatch(release) is None:
        fail(
            f"kernel release {release} is outside required "
            f"{expected_release_series} series"
        )
    if descriptor_path is not None:
        verify_descriptor(manifest, descriptor_path)


def _verify_bundle(
    artifact_root: Path,
    common_root: Path,
    verify_identity: Callable[[dict], None],
) -> dict:
    """Check one bundle, with verify_identity deciding which identity it must carry."""
    if not artifact_root.is_absolute():
        fail("artifact root must be an absolute local path")
    if not artifact_root.is_dir() or artifact_root.is_symlink():
        fail("artifact root must be a regular local directory")
    manifest = load_manifest(artifact_root)
    if artifact_root.name != manifest["directory_name"]:
        fail("artifact directory basename differs from its manifest")
    verify_identity(manifest)
    verify_inventory(artifact_root, manifest)
    release = (artifact_root / "kernel/kernel.release").read_text(
        encoding="utf-8"
    ).strip()
    if release != manifest["release"]:
        fail("kernel release payload differs from its manifest")
    verify_config(artifact_root / "kernel/config", manifest["slice"])
    payload = verify_elf(
        artifact_root / "kernel/vmlinux", common_root, manifest["slice"]
    )
    verify_runtime_image(payload)
    return manifest


def verify_artifact(
    artifact_root: Path,
    common_root: Path,
    kernel_metadata_path: Path,
    abi_metadata_path: Path,
) -> dict:
    return _verify_bundle(
        artifact_root,
        common_root,
        lambda manifest: verify_tracked_identity(
            manifest, kernel_metadata_path, abi_metadata_path
        ),
    )


def verify_compatible_artifact(
    artifact_root: Path,
    common_root: Path,
    abi_metadata_path: Path,
    expected_family: str,
    expected_slice: str,
    expected_release_series: str,
    descriptor_path: Path | None = None,
) -> dict:
    return _verify_bundle(
        artifact_root,
        common_root,
        lambda manifest: verify_compatible_identity(
            manifest,
            abi_metadata_path,
            expected_family,
            expected_slice,
            expected_release_series,
            descriptor_path,
        ),
    )


def publish_artifact(
    fail_publish: Callable[[str], NoReturn],
    *,
    family: str,
    kernel_slice: str,
    source: Path,
    source_label: str,
    build: Path,
    output_root: Path,
    release_id: str,
    objcopy: Path,
    common_root: Path,
    metadata_path: Path,
    check_release: Callable[[str, dict[str, str]], None],
    verify_staged: Callable[[Path], None] | None = None,
) -> Path:
    """Publish one built kernel as a new artifact directory in output_root.

    The source must be a clean checkout of the line's tracked final tree, and
    the checked-in series must still produce that tree. Each line's publisher
    validates its own arguments and supplies its release check and any extra
    check of the staged payloads, which runs before the manifest is written.
    """

    def git(*arguments: str) -> str:
        result = subprocess.run(
            ["git", "-C", str(source), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            fail_publish(f"Git failed in the {source_label}: {result.stderr.strip()}")
        return result.stdout.strip()

    metadata = tracked_line(metadata_path)
    abi_metadata_path = common_root / "abi/identity.env"
    abi = parse_env(abi_metadata_path)
    if git("status", "--porcelain=v1", "--untracked-files=all"):
        fail_publish(f"{source_label} worktree is dirty")
    if git("rev-parse", "HEAD^{tree}") != metadata["UML_KERNEL_FINAL_TREE"]:
        fail_publish(f"{source_label} tree differs from the tracked final tree")
    # The recorded final tree must still be what the checked-in series
    # produce, so a line left stale by a change below it cannot publish.
    series_check = subprocess.run(
        [
            sys.executable,
            "-B",
            str(common_root / "tooling/kernel_series.py"),
            "tree",
            "--metadata",
            str(metadata_path),
            "--source",
            str(source),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
    )
    if series_check.returncode != 0:
        fail_publish("the checked-in series do not produce the tracked final tree")

    source_paths = {
        "kernel/System.map": build / "System.map",
        "kernel/config": build / ".config",
        "kernel/kernel.release": build / "include/config/kernel.release",
        "kernel/modules.builtin": build / "modules.builtin",
        "kernel/modules.builtin.modinfo": build / "modules.builtin.modinfo",
        "kernel/vmlinux": build / "vmlinux",
    }
    for relative, path in source_paths.items():
        if not path.is_file() or path.is_symlink():
            fail_publish(f"build payload is missing or unsafe for {relative}: {path}")
    for root, directories, files in os.walk(build, followlinks=False):
        directories[:] = [
            name for name in directories if not (Path(root) / name).is_symlink()
        ]
        for name in files:
            if name.endswith(".ko") or ".ko." in name:
                fail_publish(
                    f"loadable module output is forbidden: {Path(root) / name}"
                )

    # A config check is cheap, so a wrong configuration fails before the copy.
    # The image is checked once, as published, by verify_artifact below:
    # stripping DWARF changes nothing verify_elf reads, and the unstripped
    # build image carries the full DWARF.
    verify_config(source_paths["kernel/config"], kernel_slice)
    release = source_paths["kernel/kernel.release"].read_text(
        encoding="utf-8"
    ).strip()
    check_release(release, metadata)

    directory_name = f"{family}-{kernel_slice}-4k-{release_id}"
    output_root.mkdir(parents=True, exist_ok=True)
    if output_root.is_symlink():
        fail_publish("output root must not be a symlink")
    destination = output_root / directory_name
    if destination.exists() or destination.is_symlink():
        fail_publish("kernel artifact destination already exists")
    staging_parent = Path(
        tempfile.mkdtemp(prefix=f".{directory_name}.", dir=output_root)
    )
    staging = staging_parent / directory_name
    staging.mkdir()
    try:
        inventory = []
        for relative, (role, file_format) in sorted(PAYLOAD_SPECS.items()):
            destination_path = staging / relative
            destination_path.parent.mkdir(parents=True, exist_ok=True)
            if relative == "kernel/vmlinux":
                # DWARF is only pahole's input. verify_artifact below proves
                # that the image keeps .BTF and carries no DWARF.
                publish_runtime_image(
                    objcopy, source_paths[relative], destination_path
                )
            else:
                shutil.copyfile(source_paths[relative], destination_path)
                shutil.copymode(source_paths[relative], destination_path)
            inventory.append(
                {
                    "path": relative,
                    "role": role,
                    "format": file_format,
                    "size": destination_path.stat().st_size,
                }
            )
        if verify_staged is not None:
            verify_staged(staging)
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "release_id": release_id,
            "directory_name": directory_name,
            "family": family,
            "slice": kernel_slice,
            "published_at_utc": datetime.datetime.now(
                datetime.timezone.utc
            ).isoformat(timespec="seconds").replace("+00:00", "Z"),
            "release": release,
            "page_size": GUEST_PAGE_SIZE,
            "elf_role": "darwin-uml",
            "source": {
                "repository": metadata["UML_KERNEL_UPSTREAM_REPOSITORY"],
                "branch": metadata["UML_KERNEL_UPSTREAM_BRANCH"],
                "base_commit": metadata["UML_KERNEL_UPSTREAM_COMMIT"],
                "base_tree": metadata["UML_KERNEL_UPSTREAM_TREE"],
                "final_tree": metadata["UML_KERNEL_FINAL_TREE"],
            },
            "abi": {
                "version": int(abi["DARWIN_UML_ABI_VERSION"]),
                "fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
            },
            "inventory": inventory,
        }
        (staging / "kernel-artifact.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        verify_artifact(staging, common_root, metadata_path, abi_metadata_path)
        os.replace(staging, destination)
        staging_parent.rmdir()
    except BaseException:
        shutil.rmtree(staging_parent, ignore_errors=True)
        raise
    return destination
