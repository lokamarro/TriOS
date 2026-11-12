#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write the release manifest for a completed starter-image build.

build-all.sh owns the source pins and passes them here, so the manifest
records the sources the build actually verified."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import datetime
import hashlib
import json
import platform
from pathlib import Path

from gpt import inspect

BOOT_BASES = {
    "boot-base-aarch64": ("uml-boot-base-aarch64.cpio.gz", "aarch64"),
    "boot-base-x86_64": ("uml-boot-base-x86_64.cpio.gz", "x86_64"),
}
DEBIAN_IMAGES = {
    "debian-13-minimal": "debian-13-minimal.img",
    "debian-13-plasma-wayland": "debian-13-plasma-wayland.img",
    "debian-13-minimal-armhf": "debian-13-minimal-armhf.img",
}
PRODUCTS = ("boot-base-aarch64", "boot-base-x86_64", "alpine",
            *DEBIAN_IMAGES)
# What each row of the armhf image's package inventory records.
ARMHF_INVENTORY_COLUMNS = ["package", "version", "architecture", "size",
                           "sha256"]


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def sidecar(output: Path, name: str, what: str) -> dict[str, object]:
    path = output / name
    if not path.is_file() or path.is_symlink():
        raise SystemExit(f"starter manifest: missing {what}: {path}")
    return {"path": path.name, "sha256": digest(path),
            "size": path.stat().st_size}


def elf_audit(output: Path, image: str) -> dict[str, object]:
    """Summarise the ELF audit report, which the producer's audit passed."""
    entry = sidecar(output, f"{image}.elf.tsv", "ELF audit report")
    files = 0
    for line in (output / str(entry["path"])).read_text().splitlines():
        fields = line.split("\t")
        files += 1
        if fields[9] != "ok":
            raise SystemExit(
                f"starter manifest: {fields[0]} failed the ELF audit")
    entry["files"] = files
    return entry


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--product", action="append", required=True,
                        choices=PRODUCTS, dest="products")
    for source in ("buildroot", "busybox", "alpine"):
        parser.add_argument(f"--{source}-version")
        parser.add_argument(f"--{source}-sha256")
    parser.add_argument("--debian-suite")
    parser.add_argument("--debian-version")
    parser.add_argument("--debian-mirror")
    parser.add_argument("--debian-armhf-pins", type=Path)
    parser.add_argument("--armhf-include")
    parser.add_argument("--debootstrap-version")
    parser.add_argument("--emulator-version")
    arguments = parser.parse_args()
    output = arguments.output.resolve()
    products = list(dict.fromkeys(arguments.products))

    def require(*names: str) -> None:
        for name in names:
            if getattr(arguments, name) is None:
                parser.error(f"--{name.replace('_', '-')} is required "
                             f"by --product {', '.join(products)}")

    boot_bases = {BOOT_BASES[p][0]: BOOT_BASES[p][1]
                  for p in products if p in BOOT_BASES}
    artifacts: list[str] = [*boot_bases]
    sources: dict[str, object] = {}
    if boot_bases:
        require("buildroot_version", "buildroot_sha256", "busybox_version",
                "busybox_sha256")
        sources["buildroot"] = {"version": arguments.buildroot_version,
                                "sha256": arguments.buildroot_sha256}
        sources["busybox"] = {"version": arguments.busybox_version,
                              "sha256": arguments.busybox_sha256}
    if "alpine" in products:
        require("alpine_version", "alpine_sha256")
        artifacts.append(f"alpine-{arguments.alpine_version}-minimal.img")
        sources["alpine"] = {"version": arguments.alpine_version,
                             "sha256": arguments.alpine_sha256}
    debian = [DEBIAN_IMAGES[p] for p in products if p in DEBIAN_IMAGES]
    artifacts += debian
    if debian:
        require("debian_suite", "debian_version")
        sources["debian"] = {"suite": arguments.debian_suite,
                             "version": arguments.debian_version}
    pins: dict[str, object] = {}
    if "debian-13-minimal-armhf" in products:
        require("debian_armhf_pins", "armhf_include", "debian_mirror",
                "debootstrap_version", "emulator_version")
        pins = json.loads(arguments.debian_armhf_pins.read_text())
        sources["debian_armhf"] = {
            "mirror": arguments.debian_mirror,
            "suite": pins["suite"],
            "component": pins["component"],
            "architecture": pins["architecture"],
            "variant": "minbase",
            "include": arguments.armhf_include.split(","),
            "debootstrap": arguments.debootstrap_version,
            "emulator": arguments.emulator_version,
            "release": pins["release"],
            "downloads": pins["downloads"],
        }

    products_manifest: dict[str, object] = {}
    for name in sorted(artifacts):
        path = output / name
        if not path.is_file() or path.is_symlink():
            raise SystemExit(f"starter manifest: missing artifact: {path}")
        stat = path.stat()
        entry: dict[str, object] = {
            "sha256": digest(path),
            "size": stat.st_size,
            "allocated_size": stat.st_blocks * 512,
        }
        if name in boot_bases:
            entry["architecture"] = boot_bases[name]
        if path.suffix == ".img":
            disk = inspect(path)
            disk.pop("image", None)
            entry["disk"] = disk
            inventory = sidecar(output, f"{name}.packages.tsv",
                                "package inventory")
            if name == DEBIAN_IMAGES["debian-13-minimal-armhf"]:
                inventory["columns"] = ARMHF_INVENTORY_COLUMNS
                inventory["packages"] = pins["downloads"]["packages"]
                entry["architecture"] = "armhf"
                entry["elf_audit"] = elf_audit(output, name)
            entry["package_inventory"] = inventory
        products_manifest[name] = entry
    manifest = {
        "schema_version": 1,
        "architecture": "aarch64",
        "built_at_utc": datetime.datetime.now(datetime.timezone.utc)
        .isoformat(timespec="seconds")
        .replace("+00:00", "Z"),
        "builder": {
            "kernel": platform.release(),
            "machine": platform.machine(),
            "system": platform.system(),
        },
        "sources": sources,
        "artifacts": products_manifest,
    }
    path = output / "release-manifest.json"
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"wrote starter release manifest: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
