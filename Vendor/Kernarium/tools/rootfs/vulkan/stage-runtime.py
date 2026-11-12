#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage the audited guest Vulkan runtime as a relocatable root and receipt.

This is the CTS-free product of the same pinned sources, generated patch
series, and build options that `seal-in-vm.py` seals into the Vulkan CTS
rootfs derivative.  It exists so a guest appliance image of any supported
architecture can import the maintained Venus/Zink runtime without carrying a
CTS payload or a whole image identity.

It runs natively on the target architecture, consumes already verified
out-of-tree Mesa, Venus protocol, Vulkan-Headers and Vulkan-Loader builds, and
records the runtime's dynamic-dependency closure against a base root of the
distribution the image will use.  It never modifies a build, a source
checkout, or an image.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
from typing import Any


SCRIPT_ROOT = Path(__file__).resolve().parent
ARTIFACT = "uml-vulkan-guest-runtime"
SCHEMA_VERSION = 1
RUNTIME_COMPONENTS = ("venus-protocol", "mesa", "vulkan-headers", "vulkan-loader")


def load_sealer() -> Any:
    """Share the sealer's audited source, build and stage verification."""
    specification = importlib.util.spec_from_file_location(
        "vulkan_rootfs_sealer", SCRIPT_ROOT / "seal-in-vm.py"
    )
    if specification is None or specification.loader is None:
        raise RuntimeError("cannot load seal-in-vm.py")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


sealer = load_sealer()


def dpkg_owners(root: Path) -> dict[str, str]:
    """Map an installed path to its owning package without running dpkg."""
    owners: dict[str, str] = {}
    information = root / "var/lib/dpkg/info"
    if not information.is_dir():
        sealer.fail(f"closure root has no dpkg database: {root}")
    for listing in sorted(information.glob("*.list")):
        package = listing.name[: -len(".list")].split(":", 1)[0]
        for line in listing.read_text(encoding="utf-8", errors="strict").splitlines():
            if line.startswith("/"):
                owners.setdefault(line, package)
    return owners


def install_runtime_files(
    profile: dict[str, Any],
    mesa_stage: Path,
    loader_library: Path,
    root: Path,
) -> None:
    """Materialize exactly the runtime the receipt will describe."""
    for relative in sorted(profile["mesa_stage_files"]):
        sealer.copy_file(mesa_stage / relative, root, f"/{relative}")
    sealer.copy_file(loader_library, root, profile["loader_runtime"])
    links = {
        **{f"/{relative}": target
           for relative, target in profile["mesa_guest_symlinks"].items()},
        **profile["loader_symlinks"],
    }
    for path_in_guest, target in sorted(links.items()):
        link = sealer.guest_path(root, path_in_guest)
        link.parent.mkdir(parents=True, exist_ok=True)
        link.unlink(missing_ok=True)
        link.symlink_to(target)
    # A software ICD must never mask a Venus failure in the importing image.
    manifests = sealer.guest_path(root, "/usr/share/vulkan/icd.d")
    for path in sorted(manifests.glob("*.json")):
        if path.name != "virtio_icd.json":
            path.unlink()


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--metadata-root", type=Path, default=SCRIPT_ROOT)
    parser.add_argument("--outer-root", type=Path, default=SCRIPT_ROOT.parents[2])
    parser.add_argument(
        "--architecture",
        default=sealer.DEFAULT_ARCHITECTURE,
        choices=sorted(sealer.MULTIARCH_TRIPLETS),
    )
    parser.add_argument("--mesa-build-root", type=Path)
    parser.add_argument("--mesa-stage-root", type=Path)
    parser.add_argument("--runtime-root", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument(
        "--closure-root",
        type=Path,
        required=True,
        help=(
            "writable base root of the target distribution; the runtime is "
            "installed into it to record its dynamic-dependency closure"
        ),
    )
    return parser.parse_args(argv)


def stage(options: argparse.Namespace) -> dict[str, Any]:
    if os.geteuid() != 0:
        sealer.fail("stage-runtime.py must run as root to inspect a guest root")
    profile = sealer.architecture_profile(options.architecture)
    work_root = options.work_root.resolve(strict=True)
    metadata_root = options.metadata_root.resolve(strict=True)
    outer_root = options.outer_root.resolve(strict=True)
    runtime_root = options.runtime_root.resolve(strict=False)
    receipt_path = options.receipt.resolve(strict=False)
    closure_root = options.closure_root.resolve(strict=True)
    sealer.require_new_path(runtime_root, "runtime root")
    sealer.require_new_path(receipt_path, "output receipt")
    if not (closure_root / "var/lib/dpkg/status").is_file():
        sealer.fail(f"closure root is not a Debian root: {closure_root}")

    sealer.run(
        (metadata_root / "verify-inputs.py", "--metadata-root", metadata_root,
         "--outer-root", outer_root),
        capture=False,
    )
    rows = {row["component"]: row
            for row in sealer.read_tsv(metadata_root / "sources.tsv")}
    sources: dict[str, Any] = {}
    trees: dict[str, Path] = {}
    for component in RUNTIME_COMPONENTS:
        sources[component], trees[component] = sealer.verify_source(
            work_root, metadata_root, rows[component]
        )

    sealer.verify_bundled_vulkan(trees["mesa"], trees["vulkan-headers"])
    protocol = {**sources["venus-protocol"], "patched": True}
    protocol_build = work_root / "build/venus-protocol"
    protocol["generation"] = sealer.verify_protocol_build(
        work_root, trees["venus-protocol"], trees["vulkan-headers"], protocol_build
    )

    mesa_build_root = sealer.require_within(
        work_root, options.mesa_build_root or work_root / "build/mesa",
        "Mesa build root")
    mesa_stage = sealer.require_within(
        work_root, options.mesa_stage_root or work_root / "stage/mesa",
        "Mesa stage root")
    mesa_configure_argv = sealer.verify_mesa_build_stage(
        trees["mesa"], mesa_build_root, mesa_stage, protocol_build, profile
    )
    (
        headers_build_root,
        headers_install,
        loader_build_root,
        loader_stage,
        loader_library,
        loader_build_id,
    ) = sealer.verify_runtime_stages(
        work_root, trees["vulkan-headers"], mesa_stage, profile
    )

    runtime_root.mkdir(parents=True)
    install_runtime_files(profile, mesa_stage, loader_library, runtime_root)
    install_runtime_files(profile, mesa_stage, loader_library, closure_root)
    sealer.run(("chroot", closure_root, "/sbin/ldconfig"),
               env=sealer.GUEST_INSPECTION_ENV, capture=False)
    closure = sealer.dynamic_closure(closure_root, profile["closure_consumers"])

    mesa_files = [sealer.file_record(runtime_root, f"/{relative}")
                  for relative in sorted(profile["mesa_stage_files"])]
    symlinks = {
        **{f"/{relative}": target
           for relative, target in profile["mesa_guest_symlinks"].items()},
        **profile["loader_symlinks"],
    }
    # A dependency the runtime itself provides is not something the importing
    # image has to supply, even where it replaces a distribution path.
    runtime_paths = {record["path"] for record in mesa_files}
    runtime_paths.add(profile["loader_runtime"])
    runtime_paths.update(symlinks)
    owners = dpkg_owners(closure_root)
    external = sorted({record["resolved_path"] for record in closure
                       if record["resolved_path"] not in runtime_paths})
    unowned = [path for path in external if path not in owners]
    if unowned:
        sealer.fail(f"runtime depends on unpackaged base libraries: {unowned}")
    base_packages = sealer.parse_dpkg_status(
        closure_root, {owners[path] for path in external}
    )
    receipt = {
        "schema_version": SCHEMA_VERSION,
        "artifact": ARTIFACT,
        "architecture": {
            "debian": profile["debian_architecture"],
            "triplet": profile["triplet"],
            "library_directory": profile["guest_libraries"],
        },
        "claim_scope": {
            "backend": "venus-kosmickrisp",
            "downstream_guest_mesa": True,
            "formal_conformance": False,
            "cts_payload": False,
            "purpose": (
                "Relocatable guest Venus/Zink runtime for an appliance image; "
                "not a CTS qualification, a Khronos conformance submission, or "
                "a claim about game rendering."
            ),
        },
        "protocol": protocol,
        "mesa": {
            **sources["mesa"],
            "release": rows["mesa"]["release_ref"],
            "configure_argv": mesa_configure_argv,
            "build_targets": ["env", f"DESTDIR={mesa_stage}", "meson", "install",
                              "-C", str(mesa_build_root), "--tags", "runtime"],
            "install_prefix": "/usr",
            "files": mesa_files,
            "reported_api_version": sealer.EXPECTED_ICD_MANIFEST["ICD"]["api_version"],
            "portability_driver": False,
        },
        "vulkan_headers": {
            **sources["vulkan-headers"],
            "release": rows["vulkan-headers"]["release_ref"],
            "patched": False,
            "configure_argv": ["cmake", "-S", str(trees["vulkan-headers"]),
                               "-B", str(headers_build_root),
                               *sealer.VULKAN_HEADERS_OPTIONS,
                               f"-DCMAKE_INSTALL_PREFIX={headers_install}"],
            "build_targets": ["cmake", "--build", str(headers_build_root),
                              "--target", "install"],
            "installed_in_guest": False,
        },
        "vulkan_loader": {
            **sources["vulkan-loader"],
            "release": rows["vulkan-loader"]["release_ref"],
            "patched": False,
            "configure_argv": ["cmake", "-S", str(trees["vulkan-loader"]),
                               "-B", str(loader_build_root),
                               *profile["loader_options"],
                               f"-DVULKAN_HEADERS_INSTALL_DIR={headers_install}"],
            "build_targets": ["cmake", "--build", str(loader_build_root)],
            "install_argv": ["env", f"DESTDIR={loader_stage}", "cmake",
                             "--install", str(loader_build_root)],
            "install_prefix": "/usr",
            "runtime": sealer.file_record(runtime_root, profile["loader_runtime"]),
            "build_id": loader_build_id,
            "soname": "libvulkan.so.1",
            "symlinks": dict(profile["loader_symlinks"]),
            "reported_api_version": "1.4.363",
        },
        "symlinks": symlinks,
        "selected_icd_manifest": sealer.ICD_MANIFEST,
        "toolchain": sealer.toolchain_record(profile["debian_architecture"]),
        "install_closure": closure,
        "closure_base": {
            "distribution": (closure_root / "etc/os-release")
            .read_text(encoding="utf-8").strip().splitlines()[0],
            "required_libraries": external,
            "packages": base_packages,
        },
    }
    sealer.write_json(receipt_path, receipt, mode=0o444)
    return {"runtime_root": str(runtime_root), "receipt": str(receipt_path),
            "files": len(mesa_files) + 1, "symlinks": len(symlinks),
            "closure_entries": len(closure),
            "closure_packages": sorted(base_packages)}


def main(argv: list[str]) -> int:
    try:
        print(json.dumps(stage(parse_arguments(argv)), indent=2))
    except (OSError, UnicodeError, json.JSONDecodeError, sealer.SealError) as error:
        print(f"stage-runtime.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
