#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Seal the audited Vulkan userspace into a fresh rootfs derivative.

This program is intentionally a native arm64 Debian-VM operation.  It consumes
already verified out-of-tree Mesa, Venus protocol, Vulkan-Headers,
Vulkan-Loader, and VK-GL-CTS builds, starts from a fresh extraction of the
immutable EROFS base, and creates a new ext4 image.  It never mounts or modifies
either image in place.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any, NamedTuple, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tests/integration/gpu"))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
import opengl_cts_plan  # noqa: E402
import patch_series  # noqa: E402


BASE_FILENAME = "rootfs.erofs"
BASE_SIZE = 3_802_316_800
BASE_SHA256 = "73e36c063e8374b68bdaca992f789d41dda59e159fd4b426dbbb5515dcf84c91"
BASE_UUID = "b84fcbe2-0673-4126-8f81-bdee45997306"
DERIVATIVE_FILENAME = "rootfs-vulkan-venus.ext4"
DERIVATIVE_SIZE = 8_589_934_592
DERIVATIVE_UUID = "2240061a-2b64-4626-b8bf-dd0f9993a4a9"
DERIVATIVE_LABEL = "uml-vulkan-venus"
ZINK_GL_DERIVATIVE_FILENAME = "rootfs-vulkan-zink-gl.ext4"
ZINK_GL_DERIVATIVE_UUID = "3b59fbc9-4b35-4a2b-890d-286a77b04afd"
ZINK_GL_DERIVATIVE_LABEL = "uml-vulkan-zink"
INSTALL_ROOT = "/opt/vk-gl-cts/vulkan"
OPENGL_INSTALL_ROOT = "/opt/vk-gl-cts/openglcts"
GUEST_RECEIPT = "/usr/share/uml-vulkan/rootfs-build-receipt.json"
GUEST_ORACLE = "/usr/share/uml-vulkan/status-oracle.json"
ICD_MANIFEST = "/usr/share/vulkan/icd.d/virtio_icd.json"
EXPECTED_ICD_MANIFEST = {
    "ICD": {"api_version": "1.4.363", "library_path": "libvulkan_virtio.so"},
    "file_format_version": "1.0.1",
}
MULTIARCH_TRIPLETS = {
    "amd64": "x86_64-linux-gnu",
    "arm64": "aarch64-linux-gnu",
    "i386": "i386-linux-gnu",
}
ELF_MACHINES = {
    "amd64": "Advanced Micro Devices X86-64",
    "arm64": "AArch64",
    "i386": "Intel 80386",
}
DEFAULT_ARCHITECTURE = "arm64"
EXPECTED_MESA_DRI_ALIASES = {
    "apple_dri.so",
    "armada-drm_dri.so",
    "exynos_dri.so",
    "gm12u320_dri.so",
    "hdlcd_dri.so",
    "hx8357d_dri.so",
    "ili9163_dri.so",
    "ili9225_dri.so",
    "ili9341_dri.so",
    "ili9486_dri.so",
    "imx-dcss_dri.so",
    "imx-drm_dri.so",
    "imx-lcdif_dri.so",
    "ingenic-drm_dri.so",
    "kirin_dri.so",
    "kms_swrast_dri.so",
    "komeda_dri.so",
    "mali-dp_dri.so",
    "mcde_dri.so",
    "mediatek_dri.so",
    "meson_dri.so",
    "mi0283qt_dri.so",
    "mxsfb-drm_dri.so",
    "panel-mipi-dbi_dri.so",
    "pl111_dri.so",
    "rcar-du_dri.so",
    "repaper_dri.so",
    "rockchip_dri.so",
    "rzg2l-du_dri.so",
    "ssd130x_dri.so",
    "st7586_dri.so",
    "st7735r_dri.so",
    "sti_dri.so",
    "stm_dri.so",
    "sun4i-drm_dri.so",
    "swrast_dri.so",
    "udl_dri.so",
    "virtio_gpu_dri.so",
    "vkms_dri.so",
    "zink_dri.so",
    "zynqmp-dpsub_dri.so",
}
SELECTED_MESA_DRI_ALIASES = (
    "kms_swrast_dri.so",
    "swrast_dri.so",
    "virtio_gpu_dri.so",
    "zink_dri.so",
)


def architecture_profile(debian_architecture: str) -> dict[str, Any]:
    """Describe one Debian architecture's guest Vulkan runtime layout.

    Only the multiarch library directory depends on the architecture. The
    driver inventory, build options, ICD identity, and audited runtime closure
    are the same contract for every guest, so a second guest product reuses it
    instead of restating it.
    """
    triplet = MULTIARCH_TRIPLETS[debian_architecture]
    libdir = f"lib/{triplet}"
    libraries = f"usr/{libdir}"
    mesa_stage_files = {
        f"{libraries}/dri/libdril_dri.so",
        f"{libraries}/gbm/dri_gbm.so",
        f"{libraries}/libEGL_mesa.so.0.0.0",
        f"{libraries}/libGLX_mesa.so.0.0.0",
        f"{libraries}/libgallium-26.2.6.so",
        f"{libraries}/libgbm.so.1.0.0",
        f"{libraries}/libvulkan_virtio.so",
        "usr/share/drirc.d/00-mesa-defaults.conf",
        "usr/share/drirc.d/00-venus-defaults.conf",
        "usr/share/drirc.d/00-virtio_gpu-defaults.conf",
        "usr/share/drirc.d/00-zink-defaults.conf",
        "usr/share/glvnd/egl_vendor.d/50_mesa.json",
        "usr/share/vulkan/icd.d/virtio_icd.json",
    }
    mesa_library_symlinks = {
        f"{libraries}/libEGL_mesa.so.0": "libEGL_mesa.so.0.0.0",
        f"{libraries}/libGLX_mesa.so.0": "libGLX_mesa.so.0.0.0",
        f"{libraries}/libgbm.so.1": "libgbm.so.1.0.0",
    }
    mesa_elf_stage_files = {
        relative for relative in mesa_stage_files if relative.startswith(libraries)
    }
    return {
        "debian_architecture": debian_architecture,
        "triplet": triplet,
        "elf_machine": ELF_MACHINES[debian_architecture],
        "libdir": libdir,
        "guest_libraries": f"/{libraries}",
        "mesa_stage_files": mesa_stage_files,
        "mesa_stage_symlinks": {
            **{
                f"{libraries}/dri/{alias}": "libdril_dri.so"
                for alias in EXPECTED_MESA_DRI_ALIASES
            },
            **mesa_library_symlinks,
        },
        "mesa_guest_symlinks": {
            **{
                f"{libraries}/dri/{alias}": "libdril_dri.so"
                for alias in SELECTED_MESA_DRI_ALIASES
            },
            **mesa_library_symlinks,
        },
        "mesa_elf_stage_files": mesa_elf_stage_files,
        "mesa_build_options": {
            "buildtype": "release",
            "egl": "enabled",
            "gallium-drivers": ["virgl", "zink", "softpipe"],
            "gallium-va": "disabled",
            "gbm": "enabled",
            "gles1": "enabled",
            "gles2": "enabled",
            "glvnd": "enabled",
            "glx": "dri",
            "libdir": libdir,
            "llvm": "disabled",
            "opengl": True,
            "platforms": ["x11", "wayland"],
            "prefix": "/usr",
            "video-codecs": [],
            "vulkan-beta": True,
            "vulkan-drivers": ["virtio"],
            "vulkan-layers": [],
            "vulkan-manifest-per-architecture": False,
        },
        "mesa_options": (
            "--prefix=/usr",
            f"--libdir={libdir}",
            "--buildtype=release",
            "-Dplatforms=x11,wayland",
            "-Dgallium-drivers=virgl,zink,softpipe",
            "-Dgallium-va=disabled",
            "-Dvulkan-drivers=virtio",
            "-Dvulkan-beta=true",
            "-Dvulkan-layers=",
            "-Dvulkan-manifest-per-architecture=false",
            "-Dglx=dri",
            "-Degl=enabled",
            "-Dgbm=enabled",
            "-Dgles1=enabled",
            "-Dgles2=enabled",
            "-Dopengl=true",
            "-Dglvnd=enabled",
            "-Dllvm=disabled",
            "-Dvideo-codecs=",
            "-Dbuild-tests=false",
            "-Dvalgrind=disabled",
            "-Dlibunwind=disabled",
            "-Dlmsensors=disabled",
            "-Dperfetto=false",
            "-Dteflon=false",
            "-Dxmlconfig=enabled",
            "-Dshader-cache=enabled",
            "-Dzstd=enabled",
        ),
        "loader_stage": {
            f"{libraries}/cmake/VulkanLoader/VulkanLoaderConfig-release.cmake",
            f"{libraries}/cmake/VulkanLoader/VulkanLoaderConfig.cmake",
            f"{libraries}/cmake/VulkanLoader/VulkanLoaderConfigVersion.cmake",
            f"{libraries}/libvulkan.so",
            f"{libraries}/libvulkan.so.1",
            f"{libraries}/libvulkan.so.1.4.363",
            f"{libraries}/pkgconfig/vulkan.pc",
        },
        "loader_runtime": f"/{libraries}/libvulkan.so.1.4.363",
        "loader_symlinks": {
            f"/{libraries}/libvulkan.so": "libvulkan.so.1",
            f"/{libraries}/libvulkan.so.1": "libvulkan.so.1.4.363",
        },
        "loader_options": (
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_INSTALL_PREFIX=/usr",
            f"-DCMAKE_INSTALL_LIBDIR={libdir}",
            "-DUPDATE_DEPS=OFF",
            "-DBUILD_TESTS=OFF",
        ),
        "closure_consumers": (
            f"/{libraries}/libvulkan_virtio.so",
            f"/{libraries}/dri/libdril_dri.so",
            f"/{libraries}/gbm/dri_gbm.so",
            f"/{libraries}/libEGL_mesa.so.0.0.0",
            f"/{libraries}/libGLX_mesa.so.0.0.0",
            f"/{libraries}/libgallium-26.2.6.so",
            f"/{libraries}/libgbm.so.1.0.0",
            f"/{libraries}/libvulkan.so.1.4.363",
        ),
    }


ARCHITECTURE = architecture_profile(DEFAULT_ARCHITECTURE)
EXPECTED_MESA_STAGE_FILES = ARCHITECTURE["mesa_stage_files"]
EXPECTED_MESA_STAGE_SYMLINKS = ARCHITECTURE["mesa_stage_symlinks"]
MESA_GUEST_SYMLINKS = ARCHITECTURE["mesa_guest_symlinks"]
MESA_ELF_STAGE_FILES = ARCHITECTURE["mesa_elf_stage_files"]
EXPECTED_MESA_BUILD_OPTIONS = ARCHITECTURE["mesa_build_options"]
EXPECTED_LOADER_STAGE = ARCHITECTURE["loader_stage"]
LOADER_RUNTIME_GUEST_PATH = ARCHITECTURE["loader_runtime"]
CTS_ALLOWED_FETCHED_RESOURCE = (
    "external/vulkancts/data/vulkan/video/hevc/LTRPSPS_A_Qualcomm_1.bit",
    "8f8e50fa408b7e96e76e6936b380fc34c208271efb8c283b15a42cb1f8c78ae7",
)
SOURCE_DIRECTORIES = {
    "mesa": "mesa",
    "venus-protocol": "venus-protocol",
    "vulkan-headers": "Vulkan-Headers",
    "vulkan-loader": "Vulkan-Loader",
    "vk-gl-cts": "VK-GL-CTS",
}
MESA_OPTIONS = ARCHITECTURE["mesa_options"]
CTS_OPTIONS = (
    "-G",
    "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DDEQP_TARGET=default",
    "-DDE_CPU=DE_CPU_ARM_64",
    "-DSELECTED_BUILD_TARGETS=deqp-vk",
    "-DDEQP_DISABLE_VK_VIDEO_TESTS=OFF",
)
CTS_OPENGL_OPTIONS = (
    "-G",
    "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DDEQP_TARGET=default",
    "-DDE_CPU=DE_CPU_ARM_64",
    "-DSELECTED_BUILD_TARGETS=deqp-vk;glcts",
    "-DDEQP_DISABLE_VK_VIDEO_TESTS=OFF",
)
VULKAN_HEADERS_OPTIONS = (
    "-G",
    "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
)
VULKAN_LOADER_OPTIONS = ARCHITECTURE["loader_options"]


class SealError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise SealError(message)


# Guest inspection commands run with this environment only, so a host-side
# preload (for example fakeroot inside an unprivileged container) never leaks
# into the recorded dynamic closure.
GUEST_INSPECTION_ENV = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin"}


def run(
    arguments: Sequence[str | Path],
    *,
    check: bool = True,
    capture: bool = True,
    input_bytes: bytes | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[Any]:
    command = tuple(str(argument) for argument in arguments)
    result = subprocess.run(
        command,
        input=input_bytes,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        check=False,
        env=env,
    )
    if check and result.returncode != 0:
        stderr = (result.stderr or b"").decode("utf-8", errors="replace").strip()
        fail(f"command failed ({result.returncode}): {' '.join(command)}: {stderr}")
    return result


def output(
    arguments: Sequence[str | Path], *, env: dict[str, str] | None = None
) -> str:
    return run(arguments, env=env).stdout.decode("utf-8", errors="strict").strip()


def detect_virtualization(debian_architecture: str = DEFAULT_ARCHITECTURE) -> str:
    """Name the native Debian environment the seal ran in.

    A minimal root can lack systemd entirely, which is not a reason to refuse
    an otherwise complete audit; it is reported as an unknown environment.
    """
    try:
        result = run(("systemd-detect-virt",), check=False)
    except OSError:
        kind = ""
    else:
        kind = (result.stdout or b"").decode("utf-8", errors="replace").strip()
    return f"native-{debian_architecture}-debian-{kind or 'unknown'}"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(4 * 1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def require_file(path: Path, label: str) -> None:
    if not path.is_file():
        fail(f"{label} is not a regular file: {path}")


def require_new_path(path: Path, label: str) -> None:
    if path.exists() or path.is_symlink():
        fail(f"{label} already exists; refusing replacement: {path}")


def require_within(work_root: Path, path: Path, label: str) -> Path:
    resolved = path.resolve(strict=False)
    if resolved == work_root:
        fail(f"{label} may not be the work root")
    try:
        resolved.relative_to(work_root)
    except ValueError:
        fail(f"{label} is outside the work root: {resolved}")
    return resolved


def read_tsv(path: Path) -> list[dict[str, str]]:
    lines = [
        line
        for line in path.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    reader = csv.DictReader(lines, delimiter="\t")
    rows = list(reader)
    if not rows or any(None in row or None in row.values() for row in rows):
        fail(f"invalid TSV: {path}")
    return rows


def expected_tree(metadata_root: Path, row: dict[str, str]) -> str:
    """Return the tree a source must have: its base, or what its series records."""
    if row["series"] == "-":
        return row["base_tree"]
    try:
        tree = patch_series.read_series((metadata_root / row["series"]).parent).tree
    except patch_series.SeriesError as error:
        fail(str(error))
    if tree is None:
        fail(f"{row['component']} series records no tree")
    return tree


def git(repository: Path, *arguments: str, input_bytes: bytes | None = None) -> str:
    return run(
        ("git", "-C", repository, *arguments), input_bytes=input_bytes
    ).stdout.decode("utf-8", errors="strict").strip()


def verify_source(
    work_root: Path,
    metadata_root: Path,
    row: dict[str, str],
) -> tuple[dict[str, Any], Path]:
    component = row["component"]
    repository = work_root / "sources" / SOURCE_DIRECTORIES[component]
    status = git(repository, "status", "--porcelain", "--untracked-files=all")
    if component == "vk-gl-cts" and status:
        allowed_path, allowed_digest = CTS_ALLOWED_FETCHED_RESOURCE
        if status != f"?? {allowed_path}" or sha256(repository / allowed_path) != allowed_digest:
            fail("vk-gl-cts contains an unverified fetched or modified resource")
    elif status:
        fail(f"{component} source checkout is dirty")
    base_commit = git(repository, "rev-parse", f"{row['base_commit']}^{{commit}}")
    base_tree = git(repository, "rev-parse", f"{base_commit}^{{tree}}")
    if base_commit != row["base_commit"] or base_tree != row["base_tree"]:
        fail(f"{component} base object identity changed")
    final_tree = git(repository, "rev-parse", "HEAD^{tree}")
    if final_tree != expected_tree(metadata_root, row):
        fail(f"{component} final tree changed: {final_tree}")

    record: dict[str, Any] = {
        "repository": row["repository"],
        "base_commit": base_commit,
        "base_tree": base_tree,
    }
    release_ref = row["release_ref"]
    if release_ref != "-":
        tag_object = git(repository, "rev-parse", release_ref)
        tag_commit = git(repository, "rev-parse", f"{release_ref}^{{commit}}")
        if tag_object != row["release_object"] or tag_commit != base_commit:
            fail(f"{component} release tag identity changed")
        record.update(
            {"release_ref": release_ref, "release_object": tag_object}
        )

    series = row["series"]
    if series == "-":
        if git(repository, "rev-parse", "HEAD") != base_commit:
            fail(f"unpatched {component} checkout is not at its base commit")
        return record, repository

    record.update({"patched_tree": final_tree})
    return record, repository


def verify_bundled_vulkan(mesa_tree: Path, headers_tree: Path) -> None:
    """Refuse a guest Mesa that bundles another Vulkan release than the SDK pin.

    The Venus codec is generated from the pinned SDK's registry and compiled
    against the Vulkan headers Mesa bundles, so a codec can name a type the
    bundled headers lack. The registry and every SDK header Mesa also carries
    must therefore be the pinned SDK's own bytes.
    """
    pairs = [("src/vulkan/registry/vk.xml", "registry/vk.xml")]
    for directory in ("include/vulkan", "include/vk_video"):
        pairs.extend(
            (f"{directory}/{path.name}", f"{directory}/{path.name}")
            for path in sorted((headers_tree / directory).glob("*.h"))
        )
    for bundled, pinned in pairs:
        if not (mesa_tree / bundled).is_file() or not (headers_tree / pinned).is_file():
            fail(f"guest Mesa does not bundle the pinned Vulkan file: {bundled}")
        if sha256(mesa_tree / bundled) != sha256(headers_tree / pinned):
            fail(f"guest Mesa bundles a different Vulkan release than the pinned SDK: {bundled}")


def verify_protocol_headers(protocol_build: Path, canonical_build: Path) -> None:
    generated = {path.name: path for path in protocol_build.glob("vn_protocol_driver*.h")}
    canonical = {path.name: path for path in canonical_build.glob("vn_protocol_driver*.h")}
    if "vn_protocol_driver.h" not in generated or generated.keys() != canonical.keys():
        fail("generated protocol header inventory differs from the authenticated generator")
    for name, path in generated.items():
        target = canonical[name]
        if (path.is_symlink() or target.is_symlink() or
                not path.is_file() or not target.is_file() or sha256(path) != sha256(target)):
            fail(f"generated protocol header differs from the authenticated generator: {name}")


def verify_protocol_build(
    work_root: Path, protocol_tree: Path, headers_tree: Path, protocol_build: Path,
) -> dict[str, Any]:
    protocol_build = require_within(work_root, protocol_build, "protocol build root")
    info_root = protocol_build / "meson-info"
    info = json.loads((info_root / "meson-info.json").read_text(encoding="utf-8"))
    if not isinstance(info, dict) or info.get("error") is not False:
        fail("protocol build introspection reports a configuration error")
    directories = info.get("directories")
    if not isinstance(directories, dict):
        fail("protocol build introspection has no directory identity")
    for name, expected in (("source", protocol_tree), ("build", protocol_build),
                           ("info", info_root)):
        value = directories.get(name)
        if (not isinstance(value, str) or not Path(value).is_absolute() or
                Path(value).resolve(strict=True) != expected.resolve(strict=True)):
            fail(f"protocol {name} directory differs from the selected path")
    options = {row["name"]: row["value"] for row in json.loads(
        (info_root / "intro-buildoptions.json").read_text(encoding="utf-8"))}
    sdk = options.get("vulkan-headers-dir")
    if (not isinstance(sdk, str) or not Path(sdk).is_absolute() or
            Path(sdk).resolve(strict=True) != headers_tree.resolve(strict=True)):
        fail("protocol SDK differs from the authenticated Vulkan-Headers source")
    wrap_mode = options.get("wrap_mode")
    if wrap_mode not in {"nodownload", "nofallback"}:
        fail("protocol build must use an offline wrap mode")
    run(("meson", "compile", "-C", protocol_build, "-j", "4"))
    # Recompute the generated bytes independently. Incremental Ninja state is
    # not an oracle for a generated file that was modified outside the build.
    with tempfile.TemporaryDirectory(prefix="protocol-verify-",
                                     dir=protocol_build.parent) as temporary:
        canonical = Path(temporary)
        run(("meson", "setup", canonical, protocol_tree, "--wrap-mode=nofallback",
             f"-Dvulkan-headers-dir={headers_tree}"))
        run(("meson", "compile", "-C", canonical, "-j", "4"))
        verify_protocol_headers(protocol_build, canonical)
    return {
        "configure_argv": ["meson", "setup", str(protocol_build), str(protocol_tree),
                           f"--wrap-mode={wrap_mode}", f"-Dvulkan-headers-dir={headers_tree}"],
        "registry_sha256": sha256(headers_tree / "registry/vk.xml"),
        "driver_headers": {path.name: sha256(path) for path in
                           sorted(protocol_build.glob("vn_protocol_driver*.h"))},
    }


def verify_mesa_build(
    mesa_tree: Path,
    mesa_build_root: Path,
    protocol_build: Path,
    profile: dict[str, Any] = ARCHITECTURE,
) -> str:
    info_root = mesa_build_root / "meson-info"
    info = json.loads((info_root / "meson-info.json").read_text(encoding="utf-8"))
    if not isinstance(info, dict) or info.get("error") is not False:
        fail("Mesa build introspection reports a configuration error")
    directories = info.get("directories")
    if not isinstance(directories, dict):
        fail("Mesa build introspection has no directory identity")
    for name, expected in (
        ("source", mesa_tree),
        ("build", mesa_build_root),
        ("info", info_root),
    ):
        value = directories.get(name)
        if (not isinstance(value, str) or not Path(value).is_absolute() or
                Path(value).resolve(strict=True) != expected.resolve(strict=True)):
            fail(f"Mesa {name} directory differs from the selected path")
    options = {
        row["name"]: row["value"]
        for row in json.loads(
            (info_root / "intro-buildoptions.json").read_text(encoding="utf-8")
        )
    }
    for name, expected in profile["mesa_build_options"].items():
        if options.get(name) != expected:
            fail(f"Mesa build option {name} changed: {options.get(name)!r} != {expected!r}")
    protocol_dir = options.get("venus-protocol-dir")
    if (not isinstance(protocol_dir, str) or not Path(protocol_dir).is_absolute() or
            Path(protocol_dir).resolve(strict=True) != protocol_build.resolve(strict=True)):
        fail("Mesa protocol directory differs from the authenticated generated headers")
    wrap_mode = options.get("wrap_mode")
    if wrap_mode not in {"nodownload", "nofallback"}:
        fail("Mesa build must use an offline wrap mode before synchronization")
    return wrap_mode


def verify_mesa_stage_inventory(
    mesa_stage: Path, profile: dict[str, Any] = ARCHITECTURE
) -> None:
    expected_files = profile["mesa_stage_files"]
    expected_symlinks = profile["mesa_stage_symlinks"]
    inventory = {
        path.relative_to(mesa_stage).as_posix()
        for path in mesa_stage.rglob("*")
        if path.is_file() and not path.is_symlink()
    }
    if inventory != expected_files:
        fail(f"Mesa install file inventory changed: {sorted(inventory ^ expected_files)}")
    symlinks = {
        path.relative_to(mesa_stage).as_posix(): os.readlink(path)
        for path in mesa_stage.rglob("*")
        if path.is_symlink()
    }
    if symlinks != expected_symlinks:
        changed = set(symlinks.items()) ^ set(expected_symlinks.items())
        fail(f"Mesa install symlink inventory changed: {sorted(changed)}")


def verify_mesa_build_stage(
    mesa_tree: Path,
    mesa_build_root: Path,
    mesa_stage: Path,
    protocol_build: Path,
    profile: dict[str, Any] = ARCHITECTURE,
) -> list[str]:
    wrap_mode = verify_mesa_build(mesa_tree, mesa_build_root, protocol_build, profile)
    # A topic can change at the same source path. Let the owning build system
    # synchronize that confined build before authenticating its staged bytes.
    run(("meson", "compile", "-C", mesa_build_root, "-j", "4"))
    if verify_mesa_build(mesa_tree, mesa_build_root, protocol_build, profile) != wrap_mode:
        fail("Mesa offline wrap mode changed during synchronization")
    introspection = json.loads(output((
        "meson", "introspect", "--installed", "--install-plan", mesa_build_root,
    )))
    installed = introspection.get("installed")
    plan = introspection.get("install_plan")
    if not isinstance(installed, dict) or not isinstance(plan, dict):
        fail("Mesa runtime install introspection is malformed")
    runtime: set[str] = set()
    for category, entries in plan.items():
        if not isinstance(entries, dict):
            fail("Mesa runtime install plan is malformed")
        for value, entry in entries.items():
            if not isinstance(entry, dict):
                fail("Mesa runtime install entry is malformed")
            if entry.get("tag") != "runtime":
                continue
            if category not in {"targets", "data", "configure"}:
                fail(f"Mesa runtime install category changed: {category}")
            source = Path(value)
            roots = (mesa_build_root,) if category != "data" else (mesa_tree, mesa_build_root)
            if (not source.is_absolute() or source.is_symlink() or not source.is_file() or
                    not any(source.resolve().is_relative_to(root.resolve()) for root in roots)):
                fail(f"Mesa runtime input is outside its authenticated source/build: {value}")
            destination = installed.get(value)
            if (not isinstance(destination, str) or not destination.startswith("/") or
                    str(Path(destination)) != destination or ".." in Path(destination).parts):
                fail(f"Mesa runtime destination is not normalized: {destination!r}")
            relative = destination[1:]
            if relative in runtime:
                fail(f"Mesa runtime destination is duplicated: {destination}")
            runtime.add(relative)
    if runtime != profile["mesa_stage_files"]:
        fail("Mesa runtime install mapping changed: "
             f"{sorted(runtime ^ profile['mesa_stage_files'])}")
    verify_mesa_stage_inventory(mesa_stage, profile)
    # Meson normalizes ELF RPATHs when installing. Its own fresh runtime install
    # is the byte oracle; raw build hashes or build IDs are not equivalent.
    with tempfile.TemporaryDirectory(
        prefix="mesa-runtime-verify-", dir=mesa_build_root.parent,
    ) as temporary:
        canonical = Path(temporary)
        run(("meson", "install", "-C", mesa_build_root, "--no-rebuild",
             "--tags", "runtime", "--destdir", canonical))
        verify_mesa_stage_inventory(canonical, profile)
        for relative in sorted(runtime):
            if sha256(mesa_stage / relative) != sha256(canonical / relative):
                fail(f"Mesa staged runtime differs from the selected build: {relative}")
    return ["meson", "setup", str(mesa_build_root), str(mesa_tree),
            f"--wrap-mode={wrap_mode}", *profile["mesa_options"],
            f"-Dvenus-protocol-dir={protocol_build}"]


class RuntimeStages(NamedTuple):
    headers_build_root: Path
    headers_install: Path
    loader_build_root: Path
    loader_stage: Path
    loader_library: Path
    loader_build_id: str


def verify_runtime_stages(
    work_root: Path,
    headers_tree: Path,
    mesa_stage: Path,
    profile: dict[str, Any] = ARCHITECTURE,
) -> RuntimeStages:
    """Check the staged ICD, Mesa ELFs, Vulkan-Headers and Vulkan-Loader."""
    manifest = json.loads(
        (mesa_stage / ICD_MANIFEST.lstrip("/")).read_text(encoding="utf-8")
    )
    if manifest != EXPECTED_ICD_MANIFEST:
        fail("staged Virtio ICD manifest changed")
    for relative in sorted(profile["mesa_elf_stage_files"]):
        read_build_id(mesa_stage / relative, profile)

    headers_build_root = work_root / "build/Vulkan-Headers"
    headers_install = work_root / "stage/vulkan-headers"
    headers_cache = (headers_build_root / "CMakeCache.txt").read_text(
        encoding="utf-8"
    )
    for exact in (
        "CMAKE_BUILD_TYPE:STRING=Release",
        f"CMAKE_INSTALL_PREFIX:PATH={headers_install}",
    ):
        if exact not in headers_cache:
            fail(f"Vulkan-Headers configuration cache lacks {exact}")
    installed_registry = headers_install / "share/vulkan/registry/vk.xml"
    source_registry = headers_tree / "registry/vk.xml"
    if (
        not installed_registry.is_file()
        or not source_registry.is_file()
        or sha256(installed_registry) != sha256(source_registry)
    ):
        fail("installed Vulkan-Headers registry differs from its pinned source")

    loader_build_root = work_root / "build/Vulkan-Loader"
    loader_stage = work_root / "stage/vulkan-loader"
    loader_cache = (loader_build_root / "CMakeCache.txt").read_text(
        encoding="utf-8"
    )
    for exact in (
        "BUILD_TESTS:BOOL=OFF",
        "CMAKE_BUILD_TYPE:STRING=Release",
        f"CMAKE_INSTALL_LIBDIR:PATH={profile['libdir']}",
        "CMAKE_INSTALL_PREFIX:PATH=/usr",
        "UPDATE_DEPS:BOOL=OFF",
        f"VULKAN_HEADERS_INSTALL_DIR:UNINITIALIZED={headers_install}",
    ):
        if exact not in loader_cache:
            fail(f"Vulkan-Loader configuration cache lacks {exact}")
    loader_inventory = {
        path.relative_to(loader_stage).as_posix()
        for path in loader_stage.rglob("*")
        if path.is_file() or path.is_symlink()
    }
    if loader_inventory != profile["loader_stage"]:
        fail(
            "Vulkan-Loader install inventory changed: "
            f"{sorted(loader_inventory ^ profile['loader_stage'])}"
        )
    loader_library = loader_stage / profile["loader_runtime"].lstrip("/")
    loader_build_id = read_build_id(loader_library, profile)
    if "Library soname: [libvulkan.so.1]" not in output(
        ("readelf", "-d", loader_library)
    ):
        fail("staged Vulkan-Loader SONAME changed")
    for path_in_guest, target in profile["loader_symlinks"].items():
        link = loader_stage / path_in_guest.lstrip("/")
        if not link.is_symlink() or os.readlink(link) != target:
            fail(f"staged Vulkan-Loader symlink changed: {link}")
    return RuntimeStages(
        headers_build_root,
        headers_install,
        loader_build_root,
        loader_stage,
        loader_library,
        loader_build_id,
    )


def guest_path(root: Path, value: str) -> Path:
    if not value.startswith("/") or "\x00" in value:
        fail(f"not an absolute guest path: {value!r}")
    relative = Path(value[1:])
    if any(part in {"", ".", ".."} for part in relative.parts):
        fail(f"not a normalized guest path: {value!r}")
    return root / relative


def host_file_record(path: Path, path_in_guest: str) -> dict[str, Any]:
    require_file(path, path_in_guest)
    size = path.stat().st_size
    if size < 1:
        fail(f"receipt file is empty: {path_in_guest}")
    return {"path": path_in_guest, "size": size, "sha256": sha256(path)}


def file_record(root: Path, path_in_guest: str) -> dict[str, Any]:
    return host_file_record(guest_path(root, path_in_guest), path_in_guest)


def directory_record(root: Path, path_in_guest: str, verified: str) -> dict[str, Any]:
    directory = guest_path(root, path_in_guest)
    files = sorted(path for path in directory.rglob("*") if path.is_file())
    if not files:
        fail(f"installed resource directory is empty: {path_in_guest}")
    return {
        "path": path_in_guest,
        "file_count": len(files),
        "total_bytes": sum(path.stat().st_size for path in files),
        "verified_against": verified,
    }


def copy_file(source: Path, root: Path, destination: str) -> None:
    require_file(source, "copy source")
    target = guest_path(root, destination)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    if sha256(source) != sha256(target):
        fail(f"copy verification failed: {destination}")


def copy_directory(source: Path, root: Path, destination: str) -> None:
    if not source.is_dir():
        fail(f"copy source is not a directory: {source}")
    target = guest_path(root, destination)
    require_new_path(target, "resource destination")
    shutil.copytree(source, target, symlinks=True)
    run(("diff", "-qr", source, target))


def copy_generated_runtime_directory(
    generated: Path,
    source_directories: Sequence[Path],
    root: Path,
    destination: str,
) -> None:
    """Copy only the authenticated inputs materialized by a CTS data target."""
    if not generated.is_dir():
        fail(f"generated runtime directory is missing: {generated}")
    expected: dict[Path, Path] = {}
    for source_directory in source_directories:
        if not source_directory.is_dir():
            fail(f"runtime source directory is missing: {source_directory}")
        for source in sorted(source_directory.rglob("*")):
            if source.is_symlink():
                fail(f"runtime source contains a symlink: {source}")
            if source.is_file():
                expected[source.relative_to(source_directory)] = source
    if not expected:
        fail(f"runtime source closure is empty: {destination}")

    target = guest_path(root, destination)
    require_new_path(target, "resource destination")
    target.mkdir(parents=True)
    for relative, source in sorted(expected.items()):
        materialized = generated / relative
        require_file(materialized, "generated runtime file")
        if sha256(materialized) != sha256(source):
            fail(f"CTS data target changed {relative} in {destination}")
        installed = target / relative
        installed.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(materialized, installed)

    installed_files = {
        path.relative_to(target) for path in target.rglob("*") if path.is_file()
    }
    if installed_files != set(expected):
        fail(f"installed runtime closure changed: {destination}")


def read_build_id(path: Path, profile: dict[str, Any] = ARCHITECTURE) -> str:
    machine = profile["elf_machine"]
    header = output(("readelf", "-h", path))
    if f"Machine:                           {machine}" not in header:
        fail(f"ELF is not {machine}: {path}")
    notes = output(("readelf", "-n", path))
    match = re.search(r"Build ID: ([0-9a-f]+)", notes)
    if match is None:
        fail(f"ELF has no GNU build ID: {path}")
    return match.group(1)


def read_cts_release(path: Path) -> str:
    releases = {
        line for line in output(("strings", "--all", "--", path)).splitlines()
        if line.startswith("vulkan-cts-")
    }
    if len(releases) != 1:
        fail(f"CTS binary has no unique embedded release: {path}")
    release = releases.pop()
    if re.fullmatch(r"vulkan-cts-[0-9.]+-[0-9]+-g[0-9a-f]{40}", release) is None:
        fail(f"CTS binary has an invalid embedded release: {path}")
    return release


def verify_cts_build(
    cts_tree: Path, cts_build_root: Path, *, include_opengl_cts: bool,
) -> str:
    cache = (cts_build_root / "CMakeCache.txt").read_text(encoding="utf-8").splitlines()

    def value(key: str) -> str:
        values = [line[len(key) + 1:] for line in cache if line.startswith(key + "=")]
        if len(values) != 1:
            fail(f"CTS configuration cache lacks an unambiguous {key}")
        return values[0]

    for key, expected in (
        ("CMAKE_HOME_DIRECTORY:INTERNAL", cts_tree),
        ("CMAKE_CACHEFILE_DIR:INTERNAL", cts_build_root),
    ):
        selected = Path(value(key))
        if (not selected.is_absolute() or
                selected.resolve(strict=True) != expected.resolve(strict=True)):
            fail(f"CTS {key} differs from the authenticated source/build")
    for key, expected in (
        ("DE_CPU:UNINITIALIZED", "DE_CPU_ARM_64"),
        ("DEQP_TARGET:STRING", "default"),
        ("CMAKE_BUILD_TYPE:STRING", "Release"),
        ("SELECTED_BUILD_TARGETS:STRING", "deqp-vk;glcts" if include_opengl_cts else "deqp-vk"),
    ):
        if value(key) != expected:
            fail(f"CTS configuration cache changed: {key}")
    expected_release = git(
        cts_tree, "describe", "--tags", "--long", "--abbrev=40",
        "--match", "vulkan-cts-*", "HEAD",
    )
    binaries = [cts_build_root / "external/vulkancts/modules/vulkan/deqp-vk"]
    if include_opengl_cts:
        binaries.append(cts_build_root / "external/openglcts/modules/glcts")
    # This is readback, not a rebuild claim. A source update at the same path
    # must not label an older executable with the new source's release.
    releases = [read_cts_release(binary) for binary in binaries]
    if any(release != expected_release for release in releases):
        fail("CTS embedded release differs from its authenticated source")
    return releases[0]


def parse_dpkg_status(root: Path, package_names: set[str]) -> dict[str, str]:
    status = guest_path(root, "/var/lib/dpkg/status").read_text(
        encoding="utf-8", errors="strict"
    )
    found: dict[str, str] = {}
    for paragraph in status.split("\n\n"):
        fields: dict[str, str] = {}
        for line in paragraph.splitlines():
            if ": " in line:
                key, value = line.split(": ", 1)
                fields[key] = value
        name = fields.get("Package")
        if name in package_names and fields.get("Status") == "install ok installed":
            architecture = fields.get("Architecture", "unknown")
            found[f"{name}:{architecture}"] = fields.get("Version", "unknown")
    if {key.split(":", 1)[0] for key in found} != package_names:
        fail("base package inventory is incomplete")
    return dict(sorted(found.items()))


def tree_manifest(root: Path) -> tuple[list[dict[str, Any]], int, int]:
    top_guest = f"{INSTALL_ROOT}/vk-default.txt"
    top = guest_path(root, top_guest)
    leaf_names = [
        line.strip()
        for line in top.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if len(leaf_names) != len(set(leaf_names)):
        fail("top-level CTS manifest repeats a leaf")
    leaves: list[dict[str, Any]] = []
    case_count = 0
    unique_cases: set[str] = set()
    for relative in leaf_names:
        path = Path(relative)
        if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
            fail(f"unsafe CTS leaf path: {relative!r}")
        path_in_guest = f"{INSTALL_ROOT}/{relative}"
        leaf = guest_path(root, path_in_guest)
        leaves.append(host_file_record(leaf, path_in_guest))
        with leaf.open("r", encoding="utf-8") as source:
            for raw in source:
                case = raw.strip()
                if not case or case.startswith("#"):
                    continue
                if not case.startswith("dEQP-VK.") or "\x00" in case:
                    fail(f"invalid CTS case in {relative}: {case!r}")
                case_count += 1
                unique_cases.add(case)
    return leaves, case_count, len(unique_cases)


def opengl_manifest(root: Path, cts_tree: Path) -> dict[str, Any]:
    runner = opengl_cts_plan.RUNNER_SOURCE
    source = cts_tree / runner["path"]
    if source.stat().st_size != runner["size"] or sha256(source) != runner["sha256"]:
        fail("OpenGL runner scope source differs from the authenticated policy")
    try:
        return opengl_cts_plan.build_manifest(
            lambda path: guest_path(root, path).read_bytes()
        )
    except opengl_cts_plan.PlanError as error:
        fail(f"invalid OpenGL runner scope: {error}")


def resolve_in_chroot(root: Path, path_in_guest: str) -> str:
    resolved = output(
        ("chroot", root, "/usr/bin/readlink", "-f", path_in_guest),
        env=GUEST_INSPECTION_ENV,
    )
    if not resolved.startswith("/"):
        fail(f"dynamic dependency did not resolve in the guest: {path_in_guest}")
    require_file(guest_path(root, resolved), "dynamic dependency")
    return resolved


def dynamic_closure(root: Path, consumers: Sequence[str]) -> list[dict[str, Any]]:
    closure: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    for consumer in consumers:
        result = run(("chroot", root, "/usr/bin/ldd", consumer), env=GUEST_INSPECTION_ENV)
        text = result.stdout.decode("utf-8", errors="strict")
        if "not found" in text:
            fail(f"unresolved dynamic dependency for {consumer}: {text}")
        for raw in text.splitlines():
            line = raw.strip()
            if not line or line.startswith("linux-vdso"):
                continue
            if " => " in line:
                needed, remainder = line.split(" => ", 1)
                path_in_guest = remainder.split(" (", 1)[0]
            elif line.startswith("/"):
                path_in_guest = line.split(" (", 1)[0]
                needed = Path(path_in_guest).name
            else:
                continue
            key = (consumer, needed)
            if key in seen:
                fail(f"duplicate dynamic dependency for {consumer}: {needed}")
            seen.add(key)
            resolved = resolve_in_chroot(root, path_in_guest)
            closure.append(
                {
                    "consumer": consumer,
                    "needed": needed,
                    "resolved_path": resolved,
                    "file": file_record(root, resolved),
                }
            )
    return closure


def filesystem_uuid(tool: str, image: Path) -> str:
    text = output((tool, "-l", image)) if tool == "tune2fs" else output((tool, "-s", image))
    match = re.search(
        r"(?:Filesystem UUID|UUID):\s*([0-9a-fA-F-]{36})", text
    )
    if match is None:
        fail(f"could not read filesystem UUID from {image}")
    return match.group(1).lower()


def toolchain_record(debian_architecture: str = DEFAULT_ARCHITECTURE) -> dict[str, str]:
    os_release = Path("/etc/os-release").read_text(encoding="utf-8")
    match = re.search(r'^PRETTY_NAME="?([^"\n]+)"?$', os_release, re.MULTILINE)
    return {
        "container_or_vm": detect_virtualization(debian_architecture),
        "os": match.group(1) if match else f"Debian {debian_architecture}",
        "kernel": output(("uname", "-srvmo")),
        "compiler": output(("c++", "--version")).splitlines()[0],
        "meson": output(("meson", "--version")),
        "cmake": output(("cmake", "--version")).splitlines()[0],
        "ninja": output(("ninja", "--version")),
    }


def write_json(path: Path, value: dict[str, Any], mode: int = 0o644) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    path.chmod(mode)


def verify_sealed_key_files(
    image: Path, root: Path, paths_in_guest: Sequence[str]
) -> None:
    with tempfile.TemporaryDirectory(prefix="uml-vulkan-seal-") as temporary:
        temporary_root = Path(temporary)
        for index, path_in_guest in enumerate(paths_in_guest):
            extracted = temporary_root / str(index)
            run(
                (
                    "debugfs",
                    "-R",
                    f"dump -p {path_in_guest} {extracted}",
                    image,
                )
            )
            if sha256(extracted) != sha256(guest_path(root, path_in_guest)):
                fail(f"sealed image byte verification failed: {path_in_guest}")


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    script_root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--metadata-root", type=Path, default=script_root)
    parser.add_argument("--outer-root", type=Path, default=script_root.parents[2])
    parser.add_argument("--base-rootfs", type=Path, required=True)
    parser.add_argument(
        "--mesa-build-root",
        type=Path,
        help="Mesa build directory (default: WORK/build/mesa)",
    )
    parser.add_argument(
        "--mesa-stage-root",
        type=Path,
        help="Mesa DESTDIR containing runtime-tagged files (default: WORK/stage/mesa)",
    )
    parser.add_argument("--stage-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument(
        "--include-opengl-cts",
        action="store_true",
        help=(
            "seal the matching OpenGL CTS runtime and GL 4.6 must-pass "
            "identity alongside Vulkan CTS"
        ),
    )
    return parser.parse_args(argv)


def seal(options: argparse.Namespace) -> None:
    if os.geteuid() != 0:
        fail("seal-in-vm.py must run as root inside the Debian VM")
    work_root = options.work_root.resolve(strict=True)
    metadata_root = options.metadata_root.resolve(strict=True)
    outer_root = options.outer_root.resolve(strict=True)
    base = require_within(work_root, options.base_rootfs, "base rootfs")
    stage = require_within(work_root, options.stage_root, "stage root")
    image = require_within(work_root, options.output, "output image")
    receipt_path = require_within(work_root, options.receipt, "output receipt")
    require_file(base, "base rootfs")
    require_new_path(stage, "stage root")
    require_new_path(image, "output image")
    require_new_path(receipt_path, "output receipt")
    derivative_filename = (
        ZINK_GL_DERIVATIVE_FILENAME
        if options.include_opengl_cts
        else DERIVATIVE_FILENAME
    )
    derivative_uuid = (
        ZINK_GL_DERIVATIVE_UUID
        if options.include_opengl_cts
        else DERIVATIVE_UUID
    )
    derivative_label = (
        ZINK_GL_DERIVATIVE_LABEL
        if options.include_opengl_cts
        else DERIVATIVE_LABEL
    )
    if image.name != derivative_filename:
        fail(f"output image must be named {derivative_filename}")
    if base.stat().st_size != BASE_SIZE or sha256(base) != BASE_SHA256:
        fail("base rootfs size or digest changed")
    if filesystem_uuid("dump.erofs", base) != BASE_UUID:
        fail("base rootfs UUID changed")

    run(
        (
            metadata_root / "verify-inputs.py",
            "--metadata-root",
            metadata_root,
            "--outer-root",
            outer_root,
        ),
        capture=False,
    )
    source_rows = {row["component"]: row for row in read_tsv(metadata_root / "sources.tsv")}
    protocol_source, protocol_tree = verify_source(
        work_root, metadata_root, source_rows["venus-protocol"]
    )
    protocol = {**protocol_source, "patched": True}
    mesa_source, mesa_tree = verify_source(
        work_root, metadata_root, source_rows["mesa"]
    )
    headers_source, headers_tree = verify_source(
        work_root, metadata_root, source_rows["vulkan-headers"]
    )
    loader_source, loader_tree = verify_source(
        work_root, metadata_root, source_rows["vulkan-loader"]
    )
    cts_source, cts_tree = verify_source(
        work_root, metadata_root, source_rows["vk-gl-cts"]
    )

    verify_bundled_vulkan(mesa_tree, headers_tree)
    protocol_build = work_root / "build/venus-protocol"
    protocol["generation"] = verify_protocol_build(work_root, protocol_tree, headers_tree, protocol_build)

    mesa_build_root = require_within(
        work_root,
        options.mesa_build_root or work_root / "build/mesa",
        "Mesa build root",
    )
    mesa_stage = require_within(
        work_root,
        options.mesa_stage_root or work_root / "stage/mesa",
        "Mesa stage root",
    )
    mesa_configure_argv = verify_mesa_build_stage(
        mesa_tree, mesa_build_root, mesa_stage, protocol_build
    )
    (
        headers_build_root,
        headers_install,
        loader_build_root,
        loader_stage,
        loader_library,
        loader_build_id,
    ) = verify_runtime_stages(work_root, headers_tree, mesa_stage)

    cts_build_root = work_root / (
        "build/VK-GL-CTS-1.4.6.2-vulkan-gl"
        if options.include_opengl_cts
        else "build/VK-GL-CTS-1.4.6.2"
    )
    cts_release = verify_cts_build(
        cts_tree, cts_build_root, include_opengl_cts=options.include_opengl_cts
    )
    cts_build = cts_build_root / "external/vulkancts/modules/vulkan"
    cts_binary = cts_build / "deqp-vk"
    cts_build_id = read_build_id(cts_binary)
    opengl_build = cts_build_root / "external/openglcts/modules"
    opengl_binary = opengl_build / "glcts"
    opengl_build_id = (
        read_build_id(opengl_binary) if options.include_opengl_cts else None
    )
    stage.mkdir(parents=True)
    run(("fsck.erofs", f"--extract={stage}", "--preserve", base), capture=False)
    base_packages = parse_dpkg_status(
        stage,
        {
            "libdrm2",
            "libegl-mesa0",
            "libgbm1",
            "libgl1-mesa-dri",
            "libglx-mesa0",
            "libvulkan1",
            "mesa-vulkan-drivers",
        },
    )
    exclusions = [
        line.strip()
        for line in (metadata_root / "exclusions.txt").read_text(
            encoding="utf-8"
        ).splitlines()
        if line.strip() and not line.startswith("#")
    ]
    if len(exclusions) != len(set(exclusions)):
        fail("exclusion list contains duplicates")
    for excluded in exclusions:
        target = guest_path(stage, excluded)
        if not target.exists() and not target.is_symlink():
            fail(f"declared exclusion does not exist in the base rootfs: {excluded}")
        if target.is_dir() and not target.is_symlink():
            shutil.rmtree(target)
        else:
            target.unlink()

    for relative, target in MESA_GUEST_SYMLINKS.items():
        link = stage / relative
        if not link.is_symlink() or os.readlink(link) != target:
            fail(f"base Mesa symlink changed: {link}")

    for relative in sorted(EXPECTED_MESA_STAGE_FILES):
        copy_file(mesa_stage / relative, stage, f"/{relative}")

    guest_loader_directory = guest_path(stage, ARCHITECTURE["guest_libraries"])
    old_loader = guest_loader_directory / "libvulkan.so.1.4.309"
    old_loader_link = guest_loader_directory / "libvulkan.so.1"
    development_link = guest_loader_directory / "libvulkan.so"
    if not old_loader.is_file() or old_loader.is_symlink():
        fail("base Vulkan loader runtime changed")
    if development_link.exists() or development_link.is_symlink():
        fail("base Vulkan loader unexpectedly contains a development symlink")
    if (
        not old_loader_link.is_symlink()
        or os.readlink(old_loader_link) != "libvulkan.so.1.4.309"
    ):
        fail(f"base Vulkan loader symlink changed: {old_loader_link}")
    old_loader_link.unlink()
    old_loader.unlink()
    copy_file(loader_library, stage, LOADER_RUNTIME_GUEST_PATH)
    os.symlink("libvulkan.so.1.4.363", old_loader_link)
    os.symlink("libvulkan.so.1", development_link)

    install = guest_path(stage, INSTALL_ROOT)
    require_new_path(install, "CTS install root")
    install.mkdir(parents=True)
    copy_file(cts_binary, stage, f"{INSTALL_ROOT}/deqp-vk")
    copy_directory(cts_build / "amber/amber", stage, f"{INSTALL_ROOT}/amber")
    copy_directory(cts_build / "vulkan", stage, f"{INSTALL_ROOT}/vulkan")
    mustpass_source = cts_tree / "external/vulkancts/mustpass/main"
    copy_directory(mustpass_source / "vk-default", stage, f"{INSTALL_ROOT}/vk-default")
    for filename in (
        "vk-default.txt",
        "vk-fraction-mandatory-tests.txt",
        "waivers.xml",
    ):
        copy_file(mustpass_source / filename, stage, f"{INSTALL_ROOT}/{filename}")
    if options.include_opengl_cts:
        opengl_install = guest_path(stage, OPENGL_INSTALL_ROOT)
        require_new_path(opengl_install, "OpenGL CTS install root")
        opengl_install.mkdir(parents=True)
        copy_file(opengl_binary, stage, f"{OPENGL_INSTALL_ROOT}/glcts")
        runtime_sources = {
            "gl_cts": (cts_tree / "external/openglcts/data/gl_cts",),
            "gles2": (cts_tree / "data/gles2",),
            "gles3": (
                cts_tree / "data/gles3",
                cts_tree / "external/graphicsfuzz/data/gles3",
            ),
            "gles31": (cts_tree / "data/gles31",),
        }
        for directory, sources in runtime_sources.items():
            copy_generated_runtime_directory(
                opengl_build / directory,
                sources,
                stage,
                f"{OPENGL_INSTALL_ROOT}/{directory}",
            )
    copy_file(metadata_root / "status-oracle.json", stage, GUEST_ORACLE)

    mesa_files = [
        file_record(stage, f"/{relative}")
        for relative in sorted(EXPECTED_MESA_STAGE_FILES)
    ]
    mesa = {
        **mesa_source,
        "release": source_rows["mesa"]["release_ref"],
        "configure_argv": mesa_configure_argv,
        "build_targets": [
            "env",
            f"DESTDIR={mesa_stage}",
            "meson",
            "install",
            "-C",
            str(mesa_build_root),
            "--tags",
            "runtime",
        ],
        "install_prefix": "/usr",
        "files": mesa_files,
        "reported_api_version": "1.4.363",
        "portability_driver": False,
    }
    vulkan_headers = {
        **headers_source,
        "release": "vulkan-sdk-1.4.363.0",
        "patched": False,
        "configure_argv": [
            "cmake",
            "-S",
            str(headers_tree),
            "-B",
            str(headers_build_root),
            *VULKAN_HEADERS_OPTIONS,
            f"-DCMAKE_INSTALL_PREFIX={headers_install}",
        ],
        "build_targets": [
            "cmake",
            "--build",
            str(headers_build_root),
            "--target",
            "install",
        ],
        "installed_in_guest": False,
    }
    vulkan_loader = {
        **loader_source,
        "release": "vulkan-sdk-1.4.363.0",
        "patched": False,
        "configure_argv": [
            "cmake",
            "-S",
            str(loader_tree),
            "-B",
            str(loader_build_root),
            *VULKAN_LOADER_OPTIONS,
            f"-DVULKAN_HEADERS_INSTALL_DIR={headers_install}",
        ],
        "build_targets": ["cmake", "--build", str(loader_build_root)],
        "install_argv": [
            "env",
            f"DESTDIR={loader_stage}",
            "cmake",
            "--install",
            str(loader_build_root),
        ],
        "install_prefix": "/usr",
        "runtime": file_record(stage, LOADER_RUNTIME_GUEST_PATH),
        "build_id": loader_build_id,
        "soname": "libvulkan.so.1",
        "symlinks": dict(ARCHITECTURE["loader_symlinks"]),
        "reported_api_version": "1.4.363",
    }
    resources = [
        directory_record(stage, f"{INSTALL_ROOT}/amber", "CTS build amber runtime archive"),
        directory_record(stage, f"{INSTALL_ROOT}/vulkan", "CTS build Vulkan runtime archive including authenticated video samples"),
        directory_record(stage, f"{INSTALL_ROOT}/vk-default", "pinned release-generated vk-default leaf lists"),
    ]
    cts = {
        **cts_source,
        "release": "vulkan-cts-1.4.6.2",
        "patched": True,
        "configure_argv": [
            "cmake",
            "--fresh",
            "-S",
            str(cts_tree),
            "-B",
            str(cts_build_root),
            *(CTS_OPENGL_OPTIONS if options.include_opengl_cts else CTS_OPTIONS),
        ],
        "build_targets": [
            "cmake",
            "--build",
            str(cts_build_root),
            "--target",
            *(("deqp-vk", "glcts") if options.include_opengl_cts else ("deqp-vk",)),
        ],
        "install_prefix": INSTALL_ROOT,
        "executable": file_record(stage, f"{INSTALL_ROOT}/deqp-vk"),
        "resources": resources,
        "build_id": cts_build_id,
        "reported_cpu": "DE_CPU_ARM_64",
        "reported_release": cts_release,
        "reported_target": "Default",
    }
    opengl_cts = None
    if options.include_opengl_cts:
        opengl_cts = {
            "release": "vulkan-cts-1.4.6.2",
            "configure_argv": [
                "cmake",
                "--fresh",
                "-S",
                str(cts_tree),
                "-B",
                str(cts_build_root),
                *CTS_OPENGL_OPTIONS,
            ],
            "build_targets": [
                "cmake",
                "--build",
                str(cts_build_root),
                "--target",
                "glcts",
            ],
            "install_prefix": OPENGL_INSTALL_ROOT,
            "executable": file_record(stage, f"{OPENGL_INSTALL_ROOT}/glcts"),
            "resources": [
                directory_record(
                    stage,
                    f"{OPENGL_INSTALL_ROOT}/{directory}",
                    "pinned CTS glcts-data source/output closure",
                )
                for directory in ("gl_cts", "gles2", "gles3", "gles31")
            ],
            "build_id": opengl_build_id,
            "reported_cpu": "DE_CPU_ARM_64",
            "reported_target": "Default",
            "mustpass": opengl_manifest(stage, cts_tree),
        }
    leaves, case_count, unique_case_count = tree_manifest(stage)
    mandatory = file_record(stage, f"{INSTALL_ROOT}/vk-fraction-mandatory-tests.txt")
    mandatory_cases = [
        line
        for line in guest_path(stage, mandatory["path"]).read_text(
            encoding="utf-8"
        ).splitlines()
        if line and not line.startswith("#")
    ]
    if not mandatory_cases or len(mandatory_cases) != len(set(mandatory_cases)):
        fail("mandatory CTS list is empty or contains duplicates")
    mustpass = {
        "top_level": file_record(stage, f"{INSTALL_ROOT}/vk-default.txt"),
        "mandatory": mandatory,
        "leaves": leaves,
        "leaf_count": len(leaves),
        "case_count": case_count,
        "unique_case_count": unique_case_count,
        "duplicate_occurrence_count": case_count - unique_case_count,
        "coverage_semantics": "Execute every ordered top-level leaf exactly once; CTS-native N/M fractions each include the complete mandatory list, whose duplicates are de-duplicated by exact case name during coverage verification.",
    }
    oracle_value = json.loads((metadata_root / "status-oracle.json").read_text(encoding="utf-8"))
    runner_oracle = {
        "file": file_record(stage, GUEST_ORACLE),
        "successful": oracle_value["successful"],
        "always_failure": oracle_value["always_failure"],
        "qualification_authority": False,
    }
    closure = dynamic_closure(
        stage,
        (
            *ARCHITECTURE["closure_consumers"],
            f"{INSTALL_ROOT}/deqp-vk",
            *((f"{OPENGL_INSTALL_ROOT}/glcts",) if options.include_opengl_cts else ()),
        ),
    )
    claim_scope = {
        "backend": "venus-kosmickrisp",
        "downstream_guest_mesa": True,
        "formal_conformance": False,
        "purpose": (
            "Receipt-gated Vulkan and OpenGL CTS qualification of Zink over "
            "the Venus/KosmicKrisp path; not a Khronos conformance "
            "submission or claim."
            if options.include_opengl_cts
            else "Receipt-gated CTS qualification of the Venus/KosmicKrisp "
            "Vulkan path; not a Khronos conformance submission or claim."
        ),
    }
    base_record = {
        "filename": BASE_FILENAME,
        "filesystem": "erofs",
        "size": BASE_SIZE,
        "sha256": BASE_SHA256,
        "filesystem_uuid": BASE_UUID,
        "read_only": True,
    }
    guest_receipt_value = {
        "schema_version": 3 if options.include_opengl_cts else 1,
        "artifact": (
            "uml-vulkan-zink-gl-rootfs-build"
            if options.include_opengl_cts
            else "uml-vulkan-rootfs-build"
        ),
        "claim_scope": claim_scope,
        "base_rootfs": base_record,
        "protocol": protocol,
        "mesa": mesa,
        "vulkan_headers": vulkan_headers,
        "vulkan_loader": vulkan_loader,
        "cts": cts,
        "toolchain": toolchain_record(),
        "install_closure": closure,
        "mustpass": mustpass,
        "runner_oracle": runner_oracle,
        "exclusions": exclusions,
        "base_packages": base_packages,
        **(
            {"opengl_cts": opengl_cts}
            if options.include_opengl_cts
            else {}
        ),
    }
    guest_receipt_path = guest_path(stage, GUEST_RECEIPT)
    write_json(guest_receipt_path, guest_receipt_value)

    image.parent.mkdir(parents=True, exist_ok=True)
    with image.open("xb") as output_image:
        output_image.truncate(DERIVATIVE_SIZE)
    run(
        (
            "mkfs.ext4",
            "-F",
            "-q",
            "-m",
            "0",
            "-U",
            derivative_uuid,
            "-L",
            derivative_label,
            "-d",
            stage,
            image,
        ),
        capture=False,
    )
    repair = run(("e2fsck", "-f", "-y", image), check=False, capture=False)
    if repair.returncode not in {0, 1}:
        fail(f"ext4 repair check returned {repair.returncode}")
    run(("e2fsck", "-f", "-n", image), capture=False)
    if image.stat().st_size != DERIVATIVE_SIZE:
        fail("derivative image size changed")
    if filesystem_uuid("tune2fs", image) != derivative_uuid:
        fail("derivative filesystem UUID changed")
    image.chmod(0o444)
    verify_sealed_key_files(
        image,
        stage,
        (
            *(f"/{relative}" for relative in sorted(EXPECTED_MESA_STAGE_FILES)),
            LOADER_RUNTIME_GUEST_PATH,
            f"{INSTALL_ROOT}/deqp-vk",
            f"{INSTALL_ROOT}/vk-default.txt",
            f"{INSTALL_ROOT}/vk-fraction-mandatory-tests.txt",
            *(
                (
                    f"{OPENGL_INSTALL_ROOT}/glcts",
                    *(row["path"] for row in opengl_cts["mustpass"]["documents"]),
                    *(row["path"] for row in opengl_cts["mustpass"]["leaves"]),
                )
                if options.include_opengl_cts
                else ()
            ),
            GUEST_ORACLE,
            GUEST_RECEIPT,
        ),
    )
    derivative = {
        "filename": derivative_filename,
        "filesystem": "ext4",
        "size": DERIVATIVE_SIZE,
        "sha256": sha256(image),
        "filesystem_uuid": derivative_uuid,
        "read_only": True,
    }
    external_receipt = {
        "schema_version": 4 if options.include_opengl_cts else 2,
        "artifact": (
            "uml-vulkan-zink-gl-rootfs"
            if options.include_opengl_cts
            else "uml-vulkan-rootfs"
        ),
        "claim_scope": claim_scope,
        "base_rootfs": base_record,
        "derivative": derivative,
        "protocol": protocol,
        "mesa": mesa,
        "vulkan_headers": vulkan_headers,
        "vulkan_loader": vulkan_loader,
        "cts": cts,
        "toolchain": guest_receipt_value["toolchain"],
        "install_closure": closure,
        "mustpass": mustpass,
        "runner_oracle": runner_oracle,
        "exclusions": exclusions,
        "base_packages": base_packages,
        "guest_receipt": host_file_record(guest_receipt_path, GUEST_RECEIPT),
        **(
            {"opengl_cts": opengl_cts}
            if options.include_opengl_cts
            else {}
        ),
    }
    write_json(receipt_path, external_receipt, mode=0o444)
    print(json.dumps({"image": derivative, "receipt": str(receipt_path)}, indent=2))


def main(argv: list[str]) -> int:
    try:
        seal(parse_arguments(argv))
    except (OSError, UnicodeError, json.JSONDecodeError, SealError) as error:
        print(f"seal-in-vm.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
