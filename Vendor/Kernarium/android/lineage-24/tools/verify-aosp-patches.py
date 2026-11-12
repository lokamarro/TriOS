#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import xml.etree.ElementTree as ET
from pathlib import Path

import pinned_source
# pinned_source puts scripts/ and the contract checks on sys.path.
from contract_checks import parse_assignments  # noqa: E402


def error(message: str) -> SystemExit:
    return SystemExit(f"AOSP 17 patch verifier: {message}")


def fail(message: str) -> None:
    raise error(message)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--aosp-source", type=Path)
    parser.add_argument("--expect", choices=("base", "final"), default="final")
    args = parser.parse_args()
    android_root = args.android_root.resolve()

    metadata = parse_assignments(android_root / "metadata/lineage.env", fail)
    base_manifest = android_root / "metadata/base-manifest.xml"
    if pinned_source.digest(base_manifest, error) != metadata.get(
        "LINEAGE24_BASE_MANIFEST_SHA256"
    ):
        fail("resolved base-manifest digest differs from AOSP metadata")
    base_projects = pinned_source.manifest_projects(ET.parse(base_manifest).getroot(), error)
    routes = pinned_source.read_route_table(
        android_root / "patches/projects.tsv", android_root, base_projects, error
    )

    if args.aosp_source is not None:
        pinned_source.verify_repo_routes(
            args.aosp_source, base_projects, routes, args.expect, error
        )
        if args.expect == "final":
            pinned_source.verify_device_tree(android_root, args.aosp_source, error)
    print(f"verified {len(routes)} routed AOSP 17 project series")


if __name__ == "__main__":
    main()
