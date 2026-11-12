#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Precompile, at build time, every application that first boot would compile.

ART Service compiles every package that lacks a usable artifact on the first
boot of a fresh /data (reason first-boot, filter verify). Soong dexpreopts the
applications it installs itself, but it never preopts an application inside an
APEX, and a presigned prebuilt can opt out. The build subcommand runs the host
dex2oat for exactly those packages, with the flags and class loader context ART
Service uses on the device, and writes the artifacts where ART looks first:

* an APK inside an APEX: /system/framework/oat/<isa>/apex@<apex>@<path>@classes.{odex,vdex}
  (OatFileAssistant::DexLocationToOdexFilename names this file for a dex
  location under /apex);
* any other APK: <apk directory>/oat/<isa>/<name>.{odex,vdex}.

The output directory holds the files, a PRODUCT_COPY_FILES fragment that
installs them (first-boot-oat.mk) and a manifest. The verify subcommand checks
a packaged target-files tree: every APK with dex code must carry artifacts, and
the ones this tool produced must match the manifest byte for byte.

A multilib image runs applications in two instruction sets. ART Service
compiles a package for every ISA of its ABI set, so each package gets one
artifact pair per ISA it can run in. The set follows PackageAbiHelperImpl: a
package without native code runs in the device's preferred ISA (arm64); a
multiArch package with libraries for both ABIs runs in both; any other package
runs in the ISA of the first supported ABI it has libraries for; and a package
with preinstalled libraries for both ISAs next to its APK counts as both. The
set is the same whichever ABI is primary, so use32bitAbi does not change it.

Soong dexpreopts an application for the targets of its own compile_multilib,
the first target by default, so a product records an application invocation for
the primary ISA only; libraries are preopted for every ISA. dex2oat's command
for an application differs between ISAs in nothing but --instruction-set,
--instruction-set-variant, --instruction-set-features and the output paths
(dexpreopt.go, dexpreoptCommand: the boot image location is the same for every
ISA, and dex2oat finds <isa>/boot.art beside it). The tool therefore takes the
recorded application invocation of the primary ISA and replaces those three
flags with the ones Soong recorded for libraries of the other ISA. Before it
does, it checks the rule against Soong's own run: for every library recorded
for both ISAs, the primary ISA's invocation with the other ISA's flags must be
that other ISA's invocation, token for token.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import concurrent.futures
import hashlib
import json
import mmap
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))

import android_image  # noqa: E402

PRIMARY_ISA = "arm64"
ISAS = ("arm64", "arm")
ABI_ISA = {"arm64-v8a": "arm64", "armeabi-v7a": "arm", "armeabi": "arm"}   # in SUPPORTED_ABIS order
FILTER = "verify"
REASON = "prebuilt"
PARTITIONS = ("system", "system_ext", "product", "vendor", "odm")
COPY_OUT = {
    "system": "TARGET_COPY_OUT_SYSTEM",
    "system_ext": "TARGET_COPY_OUT_SYSTEM_EXT",
    "product": "TARGET_COPY_OUT_PRODUCT",
    "vendor": "TARGET_COPY_OUT_VENDOR",
    "odm": "TARGET_COPY_OUT_ODM",
}
# Flags of Soong's recorded application dex2oat run that this tool replaces, or
# that only serve Soong's own artifact handling.
TEMPLATE_DROP_PREFIXES = (
    "--write-invocation-to=", "--dex-file=", "--dex-location=", "--oat-file=",
    "--oat-symbols=", "--class-loader-context=", "--stored-class-loader-context=",
    "--compiler-filter=", "--compilation-reason=", "--copy-dex-files=",
)
TEMPLATE_DROP_FLAGS = ("--strip", "--generate-debug-info", "--abort-on-hard-verifier-error")
# The flags that name the ISA the artifact is compiled for.
ISA_FLAG_PREFIXES = ("--instruction-set=", "--instruction-set-variant=", "--instruction-set-features=")
OAT_ISA_DIRECTORY = re.compile(r"/oat/(?:arm64|arm)/")
SDK_P = 28
SDK_Q = 29
APACHE_HTTP_LEGACY = "org.apache.http.legacy"
HIDL_LIBRARIES = ("android.hidl.base-V1.0-java", "android.hidl.manager-V1.0-java")
TEST_BASE = "android.test.base"


def fail(message: str) -> None:
    raise SystemExit(f"precompile-first-boot-apps: {message}")


@dataclass
class Package:
    """One APK that carries dex code."""

    host_apk: Path
    device_location: str          # the dex location the runtime uses
    partition: str                # partition that holds the artifacts
    oat_parent: str = ""          # <oat_parent>/<isa>/<odex_name> inside the partition
    odex_name: str = ""
    apex: str = ""
    target_sdk: int = 0
    uses_libraries: list[str] = field(default_factory=list)
    uses_optional_libraries: list[str] = field(default_factory=list)
    multi_arch: bool = False
    isas: tuple[str, ...] = ()

    def odex_relative(self, isa: str) -> str:
        """The odex path relative to the partition's root, for one ISA."""
        return f"{self.oat_parent}/{isa}/{self.odex_name}"


# --------------------------------------------------------------------------
# Manifest and shared-library rules (mirrors PackageBackwardCompatibility).
# --------------------------------------------------------------------------

def parse_xmltree(text: str) -> tuple[int, list[str], list[str]]:
    """Return (targetSdkVersion, required libraries, optional libraries)."""
    target = 0
    required: list[str] = []
    optional: list[str] = []
    current = ""
    name: str | None = None
    is_required = True
    pending = False

    def flush() -> None:
        nonlocal name, is_required, pending
        if pending and name:
            (required if is_required else optional).append(name)
        name, is_required, pending = None, True, False

    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("E: "):
            flush()
            current = stripped[3:].split()[0]
            pending = current == "uses-library"
            continue
        match = re.match(r"A: (?:\S+:)?(\w+)\(0x[0-9a-f]+\)=(.*)$", stripped)
        if not match:
            continue
        attribute, value = match.groups()
        if current == "uses-sdk" and attribute == "targetSdkVersion":
            number = re.match(r"(?:\(type 0x\d+\))?(0x[0-9a-f]+|\d+)", value)
            if number:
                target = int(number.group(1), 0)
        elif current == "uses-library":
            if attribute == "name":
                quoted = re.match(r'"([^"]*)"', value)
                name = quoted.group(1) if quoted else value
            elif attribute == "required":
                is_required = is_true(value)
    flush()
    return target, required, optional


def is_true(value: str) -> bool:
    return value.strip() not in ("false", "(type 0x12)0x0", "(type 0x12)0x00000000", "0")


def parse_multi_arch(text: str) -> bool:
    """Whether the application element declares android:multiArch."""
    in_application = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("E: "):
            in_application = stripped[3:].split()[0] == "application"
            continue
        match = re.match(r"A: (?:\S+:)?(\w+)\(0x[0-9a-f]+\)=(.*)$", stripped)
        if match and in_application and match.group(1) == "multiArch":
            return is_true(match.group(2))
    return False


def apply_backward_compatibility(
    target_sdk: int, system_app: bool, required: list[str], optional: list[str]
) -> tuple[list[str], list[str]]:
    """The implicit libraries PackageBackwardCompatibility adds to a system app.

    Each updater prefixes a required library, so the last updater's library is
    first: org.apache.http.legacy (targets below P), then the two HIDL libraries
    (targets P or below, system apps), then android.test.base (targets Q or
    below).
    """
    required = list(required)
    optional = list(optional)

    def present(name: str) -> bool:
        return name in required or name in optional

    if target_sdk < SDK_P and not present(APACHE_HTTP_LEGACY):
        required.insert(0, APACHE_HTTP_LEGACY)
    if target_sdk <= SDK_P and system_app:
        for name in HIDL_LIBRARIES:
            if not present(name):
                required.insert(0, name)
    else:
        required = [lib for lib in required if lib not in HIDL_LIBRARIES]
        optional = [lib for lib in optional if lib not in HIDL_LIBRARIES]
    if target_sdk <= SDK_Q and not present(TEST_BASE):
        required.insert(0, TEST_BASE)
    return required, optional


def class_loader_context(paths: list[str]) -> str:
    if not paths:
        return "PCL[]"
    return "PCL[]{" + "#".join(f"PCL[{path}]" for path in paths) + "}"


def apex_odex_name(apex: str, path_in_apex: str) -> str:
    """OatFileAssistant's odex name for a dex location under /apex."""
    return "apex@" + apex + "@" + path_in_apex.strip("/").replace("/", "@") + "@classes.odex"


# --------------------------------------------------------------------------
# Build tree and target-files access.
# --------------------------------------------------------------------------

class Layout:
    """A directory tree of installed partitions: a build output or a target-files staging."""

    def __init__(self, root: Path, kind: str) -> None:
        if kind not in ("product-out", "target-files"):
            fail(f"unknown layout: {kind}")
        if not root.is_dir():
            fail(f"missing tree: {root}")
        self.root = root
        self.kind = kind

    def partition(self, name: str) -> Path:
        return self.root / (name.upper() if self.kind == "target-files" else name)

    def host_path(self, device_path: str) -> Path:
        first, _, rest = device_path.lstrip("/").partition("/")
        return self.partition(first) / rest


class Tools:
    def __init__(self, aosp: Path, out_dir: str, host_bin: Path | None = None) -> None:
        self.aosp = aosp
        self.out = out_dir
        self._recorded: dict[tuple[str, str], list[tuple[Path, list[str]]]] | None = None
        self.bin = host_bin or aosp / out_dir / "host" / "linux-x86" / "bin"
        for tool in ("debugfs", "fsck.erofs", "aapt2"):
            if not (self.bin / tool).is_file():
                fail(f"missing host tool: {self.bin / tool}")

    def _index(self) -> dict[tuple[str, str], list[tuple[Path, list[str]]]]:
        """Every dex2oat invocation Soong recorded, by (ISA directory, file name)."""
        if self._recorded is None:
            self._recorded = {}
            base = self.aosp / self.out / "soong" / ".intermediates"
            for path in sorted(base.glob("**/dexpreopt/*/oat/*/*.invocation")):
                text = [t for t in path.read_text(encoding="utf-8").split("\n") if t]
                self._recorded.setdefault((path.parent.name, path.name), []).append((path, text))
        return self._recorded

    def recorded(self, isa: str, name: str) -> list[list[str]]:
        """The tokens of every invocation Soong recorded as <name> for one ISA."""
        return [text for _, text in self._index().get((isa, name), [])]

    def library_pairs(self, isa: str) -> list[tuple[list[str], list[str]]]:
        """The library invocations of the primary ISA and `isa` for each module that has both."""
        primary = {path.parents[1]: text
                   for path, text in self._index().get((PRIMARY_ISA, "javalib.invocation"), [])}
        return [(primary[path.parents[1]], text)
                for path, text in self._index().get((isa, "javalib.invocation"), [])
                if path.parents[1] in primary]

    def template(self, isa: str) -> list[str]:
        """Soong's own dex2oat flags for an application of one ISA.

        A recorded application invocation for the ISA is used as it is.
        Otherwise it is the primary ISA's, with the ISA flags Soong records for
        libraries of this ISA, once Soong's libraries have shown that these
        flags are all that separates the ISAs."""
        def complete(text: list[str]) -> bool:
            return (any(t.startswith("--boot-image=") for t in text)
                    and any(t.startswith("-Xbootclasspath:") for t in text)
                    and any(t.startswith("--dex-location=/") for t in text))

        for text in self.recorded(isa, "package.invocation"):
            if complete(text):
                return text
        if isa == PRIMARY_ISA:
            fail(f"no recorded {isa} application dex2oat invocation under "
                 f"{self.aosp / self.out / 'soong' / '.intermediates'}; "
                 f"build an application that Soong dexpreopts first")
        primary = next((t for t in self.recorded(PRIMARY_ISA, "package.invocation") if complete(t)), None)
        if primary is None:
            fail(f"no recorded {PRIMARY_ISA} application dex2oat invocation to derive the {isa} one from")
        pairs = self.library_pairs(isa)
        if not pairs:
            fail(f"no recorded {isa} application dex2oat invocation, and no library recorded for "
                 f"both {PRIMARY_ISA} and {isa} to show how they differ; build the libraries first")
        flags = isa_flags(pairs[0][1])
        for primary_library, other_library in pairs:
            if isa_flags(other_library) != flags:
                fail(f"Soong recorded different {isa} ISA flags for different libraries: "
                     f"{isa_flags(other_library)} and {flags}")
            derived = oat_neutral(retarget(primary_library, flags))
            recorded = oat_neutral(other_library)
            if derived != recorded:
                difference = next((a, b) for a, b in zip(derived, recorded) if a != b) \
                    if len(derived) == len(recorded) else f"{len(derived)} against {len(recorded)} arguments"
                fail(f"Soong's {isa} library invocation differs from its {PRIMARY_ISA} one in more than "
                     f"the ISA flags ({difference}); the application template cannot be derived")
        return retarget(primary, flags)


def isa_flags(tokens: list[str]) -> list[str]:
    """The ISA flags of an invocation, in the order ISA_FLAG_PREFIXES lists them."""
    found = [next((t for t in tokens if t.startswith(prefix)), None) for prefix in ISA_FLAG_PREFIXES]
    if None in found:
        fail("a recorded dex2oat invocation lacks an ISA flag")
    return found


def retarget(tokens: list[str], flags: list[str]) -> list[str]:
    """The invocation with its ISA flags replaced by `flags`."""
    isa_flags(tokens)
    by_prefix = dict(zip(ISA_FLAG_PREFIXES, flags))
    return [by_prefix[next(p for p in ISA_FLAG_PREFIXES if t.startswith(p))]
            if t.startswith(ISA_FLAG_PREFIXES) else t for t in tokens]


def oat_neutral(tokens: list[str]) -> list[str]:
    """The invocation with the ISA directory of its output paths blanked."""
    return [OAT_ISA_DIRECTORY.sub("/oat/ISA/", t) for t in tokens]


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, text=True, **kwargs)


def zip_has_dex(path: Path) -> bool:
    try:
        with zipfile.ZipFile(path) as archive:
            return any(re.fullmatch(r"classes\d*\.dex", info.filename) for info in archive.infolist())
    except zipfile.BadZipFile:
        return False


def dex_crcs(path: Path) -> dict[str, int]:
    with zipfile.ZipFile(path) as archive:
        return {
            info.filename: info.CRC
            for info in archive.infolist()
            if re.fullmatch(r"classes\d*\.dex", info.filename)
        }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def debugfs_files(tools: Tools, image: Path, directory: str) -> list[str]:
    result = run([str(tools.bin / "debugfs"), "-R", f"ls -p {directory}", str(image)])
    found: list[str] = []
    for line in result.stdout.splitlines():
        fields = line.split("/")
        if len(fields) < 7 or fields[5] in (".", ".."):
            continue
        mode = int(fields[2], 8) & 0o170000
        path = f"{directory.rstrip('/')}/{fields[5]}"
        if mode == 0o040000:
            found += debugfs_files(tools, image, path)
        elif mode == 0o100000:
            found.append(path)
    return found


def embedded_isas(apk: Path) -> set[str]:
    """ISAs that have libraries inside the APK."""
    found: set[str] = set()
    with zipfile.ZipFile(apk) as archive:
        for info in archive.infolist():
            match = re.fullmatch(r"lib/([^/]+)/[^/]+\.so", info.filename)
            if match and match.group(1) in ABI_ISA:
                found.add(ABI_ISA[match.group(1)])
    return found


def preinstalled_isas(package: Package) -> set[str]:
    """ISAs that have a library directory beside the APK (Soong's jni_libs install)."""
    if package.apex:
        return set()
    return {isa for isa in ISAS if (package.host_apk.parent / "lib" / isa).is_dir()}


def resolve_isas(tools: Tools, package: Package) -> None:
    """Set the ISAs the package runs in, as PackageAbiHelperImpl derives its ABIs."""
    found = preinstalled_isas(package)
    embedded = embedded_isas(package.host_apk)
    if len(embedded) > 1:
        read_manifest(tools, package)
        # A multiArch package keeps every ABI; any other takes the first one in
        # SUPPORTED_ABIS order.
        first = next(isa for isa in ISAS if isa in embedded)
        found |= embedded if package.multi_arch else {first}
    else:
        found |= embedded
    # A package without native code runs in the device's preferred ISA.
    package.isas = tuple(isa for isa in ISAS if isa in found) or (PRIMARY_ISA,)


def payload_error(message: str) -> SystemExit:
    return SystemExit(f"precompile-first-boot-apps: {message}")


def ext4_payload_apks(tools: Tools, payload: Path, scratch: Path, name: str) -> list[tuple[str, Path]]:
    """The APKs under /app and /priv-app of an ext4 APEX payload, with their paths in the APEX."""
    found: list[tuple[str, Path]] = []
    for path in (p for d in ("/app", "/priv-app") for p in debugfs_files(tools, payload, d)):
        if not path.endswith(".apk"):
            continue
        target = scratch / (name + path.replace("/", "@"))
        result = run([str(tools.bin / "debugfs"), "-R", f"dump {path} {target}", str(payload)])
        if not target.is_file():
            fail(f"cannot extract {path} from {name}: {result.stderr.strip()}")
        found.append((path, target))
    return found


def erofs_payload_apks(tools: Tools, payload: Path, scratch: Path, name: str) -> list[tuple[str, Path]]:
    """The APKs under /app and /priv-app of an EROFS APEX payload, with their paths in the APEX.

    The payload's directories say whether it has either; only then fsck.erofs extracts the whole
    payload (the AOSP fsck.erofs cannot extract one directory) and the APKs are taken from it.
    """
    with payload.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as image:
        present = [
            directory for directory in ("app", "priv-app")
            if android_image.erofs_file_type(image, directory, name, payload_error)
            == android_image.EROFS_FILE_TYPE_DIRECTORY
        ]
    if not present:
        return []
    extracted = scratch / f"{name}.extracted"
    result = run([str(tools.bin / "fsck.erofs"), f"--extract={extracted}", str(payload)])
    if result.returncode != 0 or not extracted.is_dir():
        fail(f"cannot extract the payload of {name}: {result.stderr.strip() or result.stdout.strip()}")
    found: list[tuple[str, Path]] = []
    for directory in present:
        for apk in sorted((extracted / directory).rglob("*.apk")):
            if apk.is_file() and not apk.is_symlink():
                relative = apk.relative_to(extracted / directory).as_posix()
                target = scratch / (name + f"@{directory}@" + relative.replace("/", "@"))
                shutil.move(apk, target)
                found.append((f"/{directory}/{relative}", target))
    shutil.rmtree(extracted)
    return found


def apex_packages(layout: Layout, tools: Tools, scratch: Path) -> list[Package]:
    packages: list[Package] = []
    for partition in PARTITIONS:
        apex_dir = layout.partition(partition) / "apex"
        if not apex_dir.is_dir():
            continue
        for apex in sorted(apex_dir.glob("*.apex")):
            name = apex.stem
            with zipfile.ZipFile(apex) as archive:
                if "apex_payload.img" not in archive.namelist():
                    fail(f"{apex} has no apex_payload.img")
                payload = scratch / f"{name}.payload.img"
                with archive.open("apex_payload.img") as source, payload.open("wb") as sink:
                    shutil.copyfileobj(source, sink, 1 << 20)
            with payload.open("rb") as stream:
                erofs = stream.read(android_image.EROFS_SUPERBLOCK_OFFSET + 4)[
                    android_image.EROFS_SUPERBLOCK_OFFSET:] == android_image.EROFS_MAGIC
            extract = erofs_payload_apks if erofs else ext4_payload_apks
            for path, target in extract(tools, payload, scratch, name):
                if not zip_has_dex(target):
                    target.unlink()
                    continue
                packages.append(Package(
                    host_apk=target, device_location=f"/apex/{name}{path}", partition="system",
                    oat_parent="framework/oat", odex_name=apex_odex_name(name, path), apex=name,
                ))
            payload.unlink()
    return packages


def installed_packages(layout: Layout) -> list[Package]:
    """Application APKs with dex code, wherever they are installed."""
    packages: list[Package] = []
    for partition in PARTITIONS:
        root = layout.partition(partition)
        for directory in ("app", "priv-app"):
            base = root / directory
            if not base.is_dir():
                continue
            for apk in sorted(base.rglob("*.apk")):
                if not zip_has_dex(apk):
                    continue
                relative = apk.relative_to(root)
                packages.append(Package(
                    host_apk=apk,
                    device_location="/" + (PurePosixPath(partition) / relative.as_posix()).as_posix(),
                    partition=partition,
                    oat_parent=(relative.parent / "oat").as_posix(),
                    odex_name=apk.stem + ".odex",
                ))
    return packages


def artifact_paths(layout: Layout, package: Package, isa: str) -> tuple[Path, Path]:
    odex = layout.partition(package.partition) / package.odex_relative(isa)
    return odex, odex.with_suffix(".vdex")


def read_manifest(tools: Tools, package: Package) -> None:
    result = run([str(tools.bin / "aapt2"), "dump", "xmltree", "--file", "AndroidManifest.xml",
                  str(package.host_apk)])
    if result.returncode != 0:
        fail(f"aapt2 cannot read {package.host_apk}: {result.stderr.strip()}")
    target, required, optional = parse_xmltree(result.stdout)
    package.multi_arch = parse_multi_arch(result.stdout)
    package.target_sdk = target or 37
    package.uses_libraries, package.uses_optional_libraries = apply_backward_compatibility(
        package.target_sdk, True, required, optional
    )


def shared_libraries(layout: Layout) -> dict[str, str]:
    libraries: dict[str, str] = {}
    for partition in PARTITIONS:
        permissions = layout.partition(partition) / "etc" / "permissions"
        if not permissions.is_dir():
            continue
        for xml_file in sorted(permissions.glob("*.xml")):
            try:
                root = ET.parse(xml_file).getroot()
            except ET.ParseError:
                continue
            for element in root.iter("library"):
                name, file = element.get("name"), element.get("file")
                if name and file and name not in libraries:
                    libraries[name] = file
    return libraries


def contexts(layout: Layout, libraries: dict[str, str], aosp: Path, package: Package) -> tuple[str, str]:
    """The class loader context for the host run and the one stored in the odex."""
    host: list[str] = []
    device: list[str] = []
    for name in package.uses_libraries + package.uses_optional_libraries:
        location = libraries.get(name)
        jar = layout.host_path(location) if location else None
        if jar is None or not jar.is_file():
            if name in package.uses_libraries:
                fail(f"{package.device_location} requires a library that is not installed: {name}")
            continue
        host.append(str(jar.relative_to(aosp)) if jar.is_relative_to(aosp) else str(jar))
        device.append(location)
    return class_loader_context(host), class_loader_context(device)


def compile_one(layout: Layout, tools: Tools, template: list[str], libraries: dict[str, str],
                package: Package, isa: str, output: Path) -> dict:
    odex = output / package.partition / package.odex_relative(isa)
    odex.parent.mkdir(parents=True, exist_ok=True)
    host_clc, stored_clc = contexts(layout, libraries, tools.aosp, package)
    command = [
        token for token in template
        if not token.startswith(TEMPLATE_DROP_PREFIXES) and token not in TEMPLATE_DROP_FLAGS
    ]
    command[0] = str((tools.bin / "dex2oatd64").relative_to(tools.aosp))
    command += [
        f"--dex-file={package.host_apk}",
        f"--dex-location={package.device_location}",
        f"--oat-file={odex}",
        f"--class-loader-context={host_clc}",
        f"--stored-class-loader-context={stored_clc}",
        f"--compiler-filter={FILTER}",
        f"--compilation-reason={REASON}",
        "--runtime-arg", f"-Xtarget-sdk-version:{package.target_sdk}",
    ]
    result = run(command, cwd=tools.aosp)
    vdex = odex.with_suffix(".vdex")
    if result.returncode != 0 or not odex.is_file() or not vdex.is_file():
        fail(f"dex2oat failed for {package.device_location} ({isa}):\n{result.stderr[-2000:]}")
    relative = package.odex_relative(isa)
    return {
        "location": package.device_location,
        "isa": isa,
        "partition": package.partition,
        "odex": f"{package.partition}/{relative}",
        "vdex": f"{package.partition}/{Path(relative).with_suffix('.vdex')}",
        "odex_sha256": sha256(odex),
        "vdex_sha256": sha256(vdex),
        "filter": FILTER,
        "target_sdk": package.target_sdk,
        "class_loader_context": stored_clc,
        "dex_crc32": dex_crcs(package.host_apk),
    }


def write_makefile(output: Path, entries: list[dict]) -> None:
    lines = [
        "# Generated by tools/precompile-first-boot-apps.py. Do not edit.",
        "LINEAGE24_FIRST_BOOT_OAT_DIR := $(dir $(lastword $(MAKEFILE_LIST)))",
    ]
    for entry in entries:
        for key in ("odex", "vdex"):
            partition, _, relative = entry[key].partition("/")
            lines.append(
                f"PRODUCT_COPY_FILES += $(LINEAGE24_FIRST_BOOT_OAT_DIR){entry[key]}"
                f":$({COPY_OUT[partition]})/{relative}"
            )
            if partition == "system":
                # generic_system.mk reserves system/ for itself; name each file.
                lines.append(
                    f"PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += system/{relative}"
                )
    (output / "first-boot-oat.mk").write_text("\n".join(lines) + "\n", encoding="utf-8")


def previous_artifacts(previous: Path | None) -> dict[str, tuple[str, str]]:
    """sha256 of the odex and vdex this tool produced earlier, by output-relative path."""
    if previous is None or not (previous / "manifest.json").is_file():
        return {}
    data = json.loads((previous / "manifest.json").read_text(encoding="utf-8"))
    return {p["odex"]: (p["odex_sha256"], p["vdex_sha256"]) for p in data["packages"]}


def cmd_build(args: argparse.Namespace) -> int:
    aosp = args.aosp_source.resolve()
    layout = Layout(aosp / args.out_dir / "target" / "product" / args.product, "product-out")
    tools = Tools(aosp, args.out_dir)
    if not (tools.bin / "dex2oatd64").is_file():
        fail(f"missing host tool: {tools.bin / 'dex2oatd64'}")
    if args.output.exists():
        fail(f"output exists: {args.output}")
    libraries = shared_libraries(layout)
    ours = previous_artifacts(args.previous)
    args.output.mkdir(parents=True)
    with tempfile.TemporaryDirectory(prefix="first-boot-apps.") as scratch_name:
        work: list[tuple[Package, str]] = []
        for package in apex_packages(layout, tools, Path(scratch_name)) + installed_packages(layout):
            resolve_isas(tools, package)
            for isa in package.isas:
                odex, vdex = artifact_paths(layout, package, isa)
                key = f"{package.partition}/{package.odex_relative(isa)}"
                if odex.exists() or vdex.exists():
                    # Soong's own artifact, unless it is a copy of one this tool made earlier.
                    if not (key in ours and odex.is_file() and vdex.is_file()
                            and ours[key] == (sha256(odex), sha256(vdex))):
                        continue
                work.append((package, isa))
        if not work:
            fail("nothing to precompile")
        templates = {isa: tools.template(isa) for isa in ISAS if any(w[1] == isa for w in work)}
        for package in {id(p): p for p, _ in work}.values():
            read_manifest(tools, package)
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            entries = list(pool.map(
                lambda item: compile_one(layout, tools, templates[item[1]], libraries,
                                         item[0], item[1], args.output), work))
    entries.sort(key=lambda entry: (entry["location"], ISAS.index(entry["isa"])))
    write_makefile(args.output, entries)
    (args.output / "manifest.json").write_text(
        json.dumps({"filter": FILTER, "packages": entries}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"precompiled {len(entries)} artifact sets into {args.output}")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    layout = Layout(args.target_files_dir.resolve(), "target-files")
    tools = Tools(Path(), "", args.host_bin.resolve())
    manifest = json.loads((args.artifacts / "manifest.json").read_text(encoding="utf-8"))
    if any("isa" not in p for p in manifest["packages"]):
        fail("the manifest predates per-ISA artifacts; rebuild the first-boot output")
    produced = {(p["location"], p["isa"]): p for p in manifest["packages"]}
    problems: list[str] = []
    seen: set[tuple[str, str]] = set()
    packages = 0
    with tempfile.TemporaryDirectory(prefix="first-boot-verify.") as scratch_name:
        for package in apex_packages(layout, tools, Path(scratch_name)) + installed_packages(layout):
            packages += 1
            resolve_isas(tools, package)
            for isa in package.isas:
                seen.add((package.device_location, isa))
                odex, vdex = artifact_paths(layout, package, isa)
                if not odex.is_file() or not vdex.is_file():
                    problems.append(f"first boot would compile {package.device_location} "
                                    f"for {isa}: no artifacts at {odex}")
                    continue
                entry = produced.get((package.device_location, isa))
                if entry is None:
                    continue  # a Soong artifact
                if (sha256(odex), sha256(vdex)) != (entry["odex_sha256"], entry["vdex_sha256"]):
                    problems.append(f"{package.device_location} ({isa}): "
                                    "packaged artifacts differ from the manifest")
                if dex_crcs(package.host_apk) != entry["dex_crc32"]:
                    problems.append(f"{package.device_location} ({isa}): "
                                    "the packaged APK no longer matches its artifacts")
    for location, isa in sorted(set(produced) - seen):
        problems.append(f"artifacts exist for a package or ISA that is not in the image: "
                        f"{location} ({isa})")
    if problems:
        fail("\n".join(problems))
    print(f"verified first-boot artifacts for {packages} packages ({len(seen)} ISA sets)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("build", help="compile the packages that lack artifacts")
    verify = sub.add_parser("verify", help="check a packaged target-files tree")
    build.add_argument("--aosp-source", type=Path, required=True,
                       help="AOSP workspace root (also the working directory of dex2oat)")
    build.add_argument("--out-dir", default="out", help="build output directory name inside the workspace")
    build.add_argument("--product", default="andarium", help="device directory under out/target/product")
    build.add_argument("--output", type=Path, required=True, help="new directory for the artifacts")
    build.add_argument("--previous", type=Path, help="earlier output; its copies in the build tree count as ours")
    build.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    build.set_defaults(func=cmd_build)
    verify.add_argument("--host-bin", type=Path, required=True,
                        help="host tool directory holding debugfs, fsck.erofs and aapt2 (out/host/linux-x86/bin)")
    verify.add_argument("--target-files-dir", type=Path, required=True, help="extracted target-files staging")
    verify.add_argument("--artifacts", type=Path, required=True, help="the build output directory")
    verify.set_defaults(func=cmd_verify)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
