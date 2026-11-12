#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import atexit
import datetime
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pinned_source  # noqa: E402
import sign_release  # noqa: E402
from release_common import (  # noqa: E402
    DIRECTORY_PREFIX,
    PAYLOAD_SPECS,
    PRODUCT_FAMILY,
    RELEASE_ID_RE,
    SCHEMA_VERSION,
    error,
    fail,
    parse_env,
    read_routes,
    require_release_build_variant,
    tracked_identity,
    verify_disk_boot_payloads,
    verify_kernel_config,
    verify_release,
)


def absolute_path(value: Path, label: str) -> Path:
    if not value.is_absolute():
        fail(f"{label} must be an absolute local path")
    if any(part in (".", "..") for part in value.parts):
        fail(f"{label} must be normalized")
    return value


def regular_file(value: Path, label: str, *, executable: bool = False) -> Path:
    absolute_path(value, label)
    if not value.is_file() or value.is_symlink():
        fail(f"{label} must be a regular non-symlink file")
    if executable and not os.access(value, os.X_OK):
        fail(f"{label} must be executable")
    return value


def executable_tool(value: Path, label: str) -> Path:
    absolute_path(value, label)
    target = value.resolve()
    if not target.is_file() or not os.access(target, os.X_OK):
        fail(f"{label} must resolve to a regular executable")
    return value


def regular_directory(value: Path, label: str) -> Path:
    absolute_path(value, label)
    if not value.is_dir() or value.is_symlink():
        fail(f"{label} must be a regular non-symlink directory")
    return value


def run(
    arguments: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    capture: bool = False,
) -> str:
    return pinned_source.run(
        *arguments, cwd=cwd, env=env, capture=capture, error=error
    )


def first_version(arguments: list[str], *, cwd: Path | None = None) -> str:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        fail(f"cannot record tool version: {arguments[0]}")
    line = result.stdout.splitlines()[0].strip().replace("\t", " ")
    return re.sub(r"(^|[ (])/[^ )]+", r"\1<tool-build-source>", line)


def read_properties(archive: zipfile.ZipFile, member: str) -> dict[str, str]:
    try:
        payload = archive.read(member).decode("utf-8", errors="strict")
    except (KeyError, UnicodeError) as error:
        fail(f"cannot read packaged Android properties {member}: {error}")
    values: dict[str, str] = {}
    for raw in payload.splitlines():
        key, separator, value = raw.partition("=")
        if separator and key not in values:
            values[key] = value
    return values


def property_value(values: dict[str, str], keys: tuple[str, ...], label: str) -> str:
    for key in keys:
        value = values.get(key)
        if value:
            return value
    fail(f"packaged Android properties omit {label}")


def copy_regular(source: Path, destination: Path) -> None:
    regular_file(source, f"release input {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination, follow_symlinks=False)
    shutil.copymode(source, destination, follow_symlinks=False)


def extract_small(
    archive: zipfile.ZipFile, member: str, destination: Path, *, limit: int = 1 << 20
) -> None:
    try:
        info = archive.getinfo(member)
    except KeyError:
        fail(f"target files omit release support payload: {member}")
    if info.is_dir() or info.file_size <= 0 or info.file_size > limit:
        fail(f"target-files release support payload has an unsafe size: {member}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(archive.read(info))


def verify_clean_git(repository: Path, label: str, expected_tree: str | None = None) -> str:
    revision = run(["git", "rev-parse", "HEAD"], cwd=repository, capture=True)
    dirty = run(
        ["git", "status", "--porcelain", "--untracked-files=all"],
        cwd=repository,
        capture=True,
    )
    if dirty:
        fail(f"{label} has uncommitted or untracked changes")
    if expected_tree is not None:
        tree = run(["git", "rev-parse", "HEAD^{tree}"], cwd=repository, capture=True)
        if tree != expected_tree:
            fail(f"{label} tree differs from the tracked final identity")
    return revision


def sdk_identity(darwin_sdk: Path) -> str:
    settings = darwin_sdk / "SDKSettings.json"
    if settings.is_file():
        try:
            value = json.loads(settings.read_text(encoding="utf-8"))
            version = value.get("Version") or value.get("DefaultDeploymentTarget")
            if isinstance(version, str) and version:
                return f"MacOSX.sdk {version}"
        except (OSError, UnicodeError, json.JSONDecodeError):
            pass
    return "MacOSX.sdk (version unavailable)"


def write_receipt(path: Path, rows: list[tuple[str, object]]) -> None:
    lines = ["key\tvalue"]
    for key, raw_value in rows:
        value = str(raw_value).replace("\t", " ").replace("\n", " ").strip()
        if not value or "/home/" in value:
            fail(f"build receipt value is empty or stores a local path: {key}")
        lines.append(f"{key}\t{value}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--aosp-source", type=Path, required=True)
    parser.add_argument(
        "--kernel-artifact",
        type=Path,
        required=True,
        help="published Android kernel artifact of the tracked kernel source",
    )
    parser.add_argument("--target-files", type=Path, required=True)
    parser.add_argument(
        "--disk-dir",
        type=Path,
        required=True,
        help=(
            "the directory assemble-image.sh filled with system.img and "
            "partition-layout.json; the new directory the signed system disk is "
            "assembled into"
        ),
    )
    parser.add_argument("--darwin-sdk", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--linker", type=Path, required=True)
    parser.add_argument("--vintf-checker", type=Path, required=True)
    parser.add_argument("--release-root", type=Path, required=True)
    parser.add_argument("--release-id", required=True)
    parser.add_argument(
        "--signing-keys",
        type=Path,
        help=(
            "private key directory (mode 0700) to sign the target files with; "
            "--target-files is the unsigned archive and the signed disk "
            "is assembled into --disk-dir. The directory is linked from a "
            "private temporary directory that is deleted, and checked deleted, "
            "afterwards"
        ),
        required=True,
    )
    parser.add_argument(
        "--first-boot-artifacts",
        type=Path,
        required=True,
        help=(
            "first-boot artifact directory of precompile-first-boot-apps.py; "
            "the signed tree is verified against it, so first boot compiles "
            "nothing"
        ),
    )
    parser.add_argument("--mkbootfs", type=Path, required=True)
    parser.add_argument("--mkbootimg", type=Path, required=True)
    args = parser.parse_args()

    if RELEASE_ID_RE.fullmatch(args.release_id) is None:
        fail("release ID has an unsafe format")
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        fail("release publisher requires an x86_64 Linux builder")

    android_root = Path(__file__).resolve().parents[1]
    outer_root = android_root.parents[1]
    aosp_source = regular_directory(args.aosp_source, "AOSP source")
    kernel_artifact = regular_directory(args.kernel_artifact, "Android kernel artifact")
    darwin_sdk = regular_directory(args.darwin_sdk, "Darwin SDK")
    release_root = regular_directory(args.release_root, "release root")
    target_files = regular_file(args.target_files, "target-files archive")
    # Every release is a user build, signed with the release keys: there is no unsigned
    # publication.
    disk_dir = absolute_path(args.disk_dir, "signed system disk directory")
    if disk_dir.exists() or disk_dir.is_symlink():
        fail("the signed system disk directory already exists")
    signing_keys = absolute_path(args.signing_keys, "signing key directory")
    for private_area, label in ((outer_root, "outer project"), (release_root, "release root"), (aosp_source, "AOSP source")):
        if signing_keys == private_area or private_area in signing_keys.parents:
            fail(f"the signing key directory must not live inside the {label}")
    mkbootfs = regular_file(args.mkbootfs, "mkbootfs", executable=True)
    mkbootimg = regular_file(args.mkbootimg, "mkbootimg", executable=True)
    first_boot_artifacts = regular_directory(
        args.first_boot_artifacts, "first-boot artifact directory"
    )
    clang = executable_tool(args.clang, "pinned Clang")
    linker = executable_tool(args.linker, "pinned linker")
    vintf_checker = regular_file(args.vintf_checker, "VINTF checker", executable=True)

    directory_name = DIRECTORY_PREFIX + args.release_id
    destination = release_root / directory_name
    if destination.exists() or destination.is_symlink():
        fail("release destination already exists")

    aosp_metadata = parse_env(android_root / "metadata/lineage.env")
    require_release_build_variant(aosp_metadata["LINEAGE24_BUILD_VARIANT"])
    tracked = tracked_identity(android_root)
    kernel_root = outer_root / "kernel/mainline-6.18"
    outer_revision = verify_clean_git(outer_root, "outer project")
    # The release ships the kernel as its published artifact: the tracked
    # source's runtime image, without the DWARF the build's vmlinux keeps.
    run([
        sys.executable,
        "-B",
        str(outer_root / "kernel/common/artifacts/verify-kernel-artifact.py"),
        "--artifact",
        str(kernel_artifact),
        "--common-root",
        str(outer_root / "kernel/common"),
        "--kernel-metadata",
        str(kernel_root / "patches/metadata.env"),
        "--abi-metadata",
        str(outer_root / "kernel/common/abi/identity.env"),
    ])

    run([
        str(android_root / "tools/verify-aosp-patches.py"),
        "--android-root",
        str(android_root),
        "--aosp-source",
        str(aosp_source),
        "--expect",
        "final",
    ])

    # verify-kernel-artifact.py has checked the payload files and the ELF.
    kernel_payload = kernel_artifact / "kernel"
    kernel_elf = kernel_payload / "vmlinux"
    kernel_config = kernel_payload / "config"
    kernel_release_file = kernel_payload / "kernel.release"
    verify_kernel_config(kernel_artifact, android_root, enforce_tracked_policy=True)
    # The unsigned build is the input. Signing rebuilds its images, and the
    # disk is assembled from the signed archive, so everything below
    # verifies and publishes the signed artifacts.
    signing_work = Path(tempfile.mkdtemp(prefix=".signing-work.", dir=release_root))
    atexit.register(shutil.rmtree, signing_work, ignore_errors=True)
    signed_archive = signing_work / "signed-target-files.zip"
    signing_result = sign_release.sign_target_files(
        target_files=target_files,
        signed_output=signed_archive,
        keys=signing_keys,
        build_variant=aosp_metadata["LINEAGE24_BUILD_VARIANT"],
        host_bin=vintf_checker.parent,
        aosp_source=aosp_source,
        jdk_bin=aosp_source / "prebuilts/jdk/jdk21/linux-x86/bin",
        temporary_parent=signing_work,
    )
    run(["unzip", "-q", str(signed_archive), "-d", str(signing_work / "signed-target-files")])
    # Re-signing must keep the precompiled first-boot artifacts valid.
    run([
        sys.executable, "-B", str(android_root / "tools/precompile-first-boot-apps.py"),
        "verify",
        "--host-bin", str(vintf_checker.parent),
        "--target-files-dir", str(signing_work / "signed-target-files"),
        "--artifacts", str(first_boot_artifacts),
    ])
    run([
        str(android_root / "build/assemble-image.sh"),
        "--target-files", str(signed_archive),
        "--signed",
        "--kernel-artifact", str(kernel_artifact),
        "--mkbootfs", str(mkbootfs),
        "--mkbootimg", str(mkbootimg),
        "--output-dir", str(disk_dir),
    ])
    target_files = signed_archive
    run([
        str(android_root / "tools/verify-target-files.py"),
        "--signed",
        "--target-files",
        str(target_files),
        "--kernel-elf",
        str(kernel_elf),
        "--kernel-config",
        str(kernel_config),
        "--kernel-release-file",
        str(kernel_release_file),
    ])
    vintf_env = os.environ.copy()
    vintf_env["PATH"] = f"{vintf_checker.parent}:/usr/bin:/bin"
    run([str(vintf_checker), str(target_files)], env=vintf_env)
    disk = regular_file(disk_dir / "system.img", "system disk")
    disk_layout = regular_file(
        disk_dir / "partition-layout.json", "system disk partition layout"
    )
    run([
        str(android_root / "tools/ubd-disk.py"),
        "verify",
        "--layout",
        str(disk_layout),
        "--disk",
        str(disk),
        "--hash",
        "--policy",
        str(android_root / "support/image/disk-policy.json"),
    ])
    verify_disk_boot_payloads(disk, disk_layout, android_root, structural=False)

    java_candidates = (
        aosp_source / "prebuilts/jdk/jdk21/linux-x86/bin/java",
        aosp_source / "prebuilts/jdk/jdk17/linux-x86/bin/java",
    )
    java = next((path for path in java_candidates if path.is_file()), None)
    if java is None:
        fail("pinned AOSP JDK is missing")

    try:
        archive = zipfile.ZipFile(target_files)
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"cannot open target-files archive: {error}")
    with archive:
        system_properties = read_properties(archive, "SYSTEM/build.prop")
        vendor_properties = read_properties(archive, "VENDOR/build.prop")
        build_id = property_value(
            system_properties, ("ro.build.id", "ro.system.build.id"), "build ID"
        )
        sdk = int(property_value(
            system_properties,
            ("ro.build.version.sdk", "ro.system.build.version.sdk"),
            "SDK level",
        ))
        fingerprint = property_value(
            system_properties,
            ("ro.build.fingerprint", "ro.system.build.fingerprint"),
            "build fingerprint",
        )
        vendor_api_level = int(property_value(
            vendor_properties,
            ("ro.board.api_level", "ro.vendor.api_level"),
            "vendor API level",
        ))
        # The build number is the release's monotonic position: the host
        # compares it to refuse a downgrade of a writable disk, so a release
        # needs a numeric one (build-aosp.sh --build-number), not the eng
        # fallback the build system uses when none is given.
        build_number_text = property_value(
            system_properties,
            ("ro.build.version.incremental", "ro.system.build.version.incremental"),
            "build number",
        )
        if re.fullmatch(r"[1-9][0-9]{0,9}", build_number_text) is None:
            fail(
                "the build's ro.build.version.incremental is not a release "
                f"build number: {build_number_text}"
            )
        build_number = int(build_number_text)
        security_patch = property_value(
            system_properties,
            ("ro.build.version.security_patch", "ro.system.build.version.security_patch"),
            "security patch level",
        )
        # The ABIs the image enables, in its own preference order.
        abi_list = [
            abi.strip()
            for abi in property_value(
                system_properties,
                ("ro.product.cpu.abilist", "ro.system.product.cpu.abilist"),
                "ABI list",
            ).split(",")
            if abi.strip()
        ]

        temporary_parent = Path(tempfile.mkdtemp(
            prefix=f".{directory_name}.", dir=release_root
        ))
        staging = temporary_parent / directory_name
        staging.mkdir()
        try:
            for name, source in (
                ("vmlinux", kernel_elf),
                ("config", kernel_config),
                ("System.map", kernel_payload / "System.map"),
                ("modules.builtin", kernel_payload / "modules.builtin"),
                ("modules.builtin.modinfo", kernel_payload / "modules.builtin.modinfo"),
            ):
                copy_regular(source, staging / "kernel" / name)
            copy_regular(
                android_root / "support/boot/kernel-command-line.txt",
                staging / "boot/kernel-command-line.txt",
            )
            copy_regular(disk_layout, staging / "guest/partition-layout.json")
            extract_small(archive, "VENDOR/etc/fstab.uml", staging / "support/fstab.uml")
            extract_small(
                archive,
                "VENDOR/etc/vintf/manifest.xml",
                staging / "support/vintf-manifest.xml",
            )
            extract_small(
                archive,
                "VENDOR/etc/vintf/compatibility_matrix.xml",
                staging / "support/compatibility-matrix.xml",
            )

            copy_regular(disk, staging / "guest/system.img")

            resolved_manifest = run(
                ["repo", "manifest", "-r"], cwd=aosp_source, capture=True
            )
            provenance = staging / "provenance/source-repo-manifest.xml"
            provenance.parent.mkdir(parents=True, exist_ok=True)
            provenance.write_text(resolved_manifest + "\n", encoding="utf-8")

            kernel_release = kernel_release_file.read_text(
                encoding="utf-8", errors="strict"
            ).strip()
            source, kernel = tracked["source"], tracked["kernel"]
            source_routes = read_routes(
                android_root / "patches/projects.tsv", android_root, "source"
            )
            receipt_rows = [
                ("schema", 3),
                ("release_id", args.release_id),
                ("product_family", PRODUCT_FAMILY),
                ("producer", "android/lineage-24/build/publish-release.sh"),
                ("outer_revision", outer_revision),
                ("outer_dirty", "false"),
                ("source_manifest_commit", source["manifest_commit"]),
                ("source_projects_manifest_sha256", source["projects_manifest_sha256"]),
                ("kernel_final_tree", kernel["final_tree"]),
                ("kernel_patch_manifest_sha256", kernel["patch_manifest_sha256"]),
                ("kernel_release", kernel_release),
                ("page_size", 4096),
                ("abi_version", kernel["abi_version"]),
                ("abi_fingerprint", kernel["abi_fingerprint"]),
                ("android_product", aosp_metadata["LINEAGE24_PRODUCT"]),
                ("android_build_id", build_id),
                ("android_build_number", build_number),
                ("source_role_android", "pinned Repo workspace"),
                ("source_role_kernel", "replayed mainline 6.18 worktree"),
                ("input_role_kernel", "verified standalone Android UML build"),
                ("input_role_disk", "verified read-only system disk"),
            ]
            signed_count, left_presigned, certificate_digests = signing_result
            receipt_rows += [
                ("signing", "private release key set"),
                ("signed_files", signed_count),
                ("presigned_files", ", ".join(sorted(left_presigned)) or "none"),
                *(
                    (f"signing_certificate_sha256_{name}", digest)
                    for name, digest in sorted(certificate_digests.items())
                ),
            ]
            write_receipt(staging / "provenance/build-receipt.tsv", receipt_rows)

            manifest = {
                "schema_version": SCHEMA_VERSION,
                "release_id": args.release_id,
                "directory_name": directory_name,
                "product_family": PRODUCT_FAMILY,
                "acceptance": "accepted",
                "published_at_utc": datetime.datetime.now(
                    datetime.timezone.utc
                ).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
                "android": {
                    "major": int(aosp_metadata["LINEAGE24_ANDROID_MAJOR"]),
                    "sdk": sdk,
                    "build_id": build_id,
                    "build_number": build_number,
                    "security_patch": security_patch,
                    "build_variant": aosp_metadata["LINEAGE24_BUILD_VARIANT"],
                    "fingerprint": fingerprint,
                    "vendor_api_level": vendor_api_level,
                    "product": aosp_metadata["LINEAGE24_PRODUCT"],
                    "release_config": aosp_metadata["LINEAGE24_RELEASE_CONFIG"],
                    "abi_list": abi_list,
                },
                "source": {**source, "routes": source_routes},
                "kernel": {
                    **kernel,
                    "release": kernel_release,
                    "page_size": 4096,
                    "elf_role": "darwin-uml",
                },
                "outer": {
                    "revision": outer_revision,
                    "dirty": False,
                    **tracked["outer"],
                },
                "producer": {
                    "entry_point": "android/lineage-24/build/publish-release.sh",
                    "functional_options": [
                        "pinned Android source",
                        "clean replayed mainline kernel source",
                        "verified standalone Android kernel build",
                        "verified AOSP target files",
                        "verified partition-backed UML initramfs",
                        "verified kernel-less LineageOS recovery partition",
                        "verified exact-fit read-only system disk",
                        "explicit Darwin SDK",
                        "explicit pinned Clang",
                        "explicit pinned linker",
                        "explicit AOSP VINTF checker",
                        "signed with a private release key set",
                    ],
                    "builder": {
                        "os": platform.platform(),
                        "architecture": platform.machine(),
                        "jdk": first_version([str(java), "-version"]),
                        "clang": first_version([str(clang), "--version"]),
                        "linker": first_version([str(linker), "--version"]),
                        "darwin_sdk": sdk_identity(darwin_sdk),
                        "android_build_tools": (
                            f"Android {aosp_metadata['LINEAGE24_AOSP_TAG']} target-files tools"
                        ),
                    },
                },
                "inventory": [],
            }
            for relative, (role, file_format) in sorted(PAYLOAD_SPECS.items()):
                payload = staging / relative
                regular_file(payload, f"published payload {relative}")
                manifest["inventory"].append({
                    "path": relative,
                    "role": role,
                    "format": file_format,
                    "size": payload.stat().st_size,
                })
            (staging / "release-manifest.json").write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            verify_release(staging, android_root)
            os.replace(staging, destination)
            temporary_parent.rmdir()
        except BaseException:
            shutil.rmtree(temporary_parent, ignore_errors=True)
            raise

    print(f"published Android release: {destination}")


if __name__ == "__main__":
    main()
