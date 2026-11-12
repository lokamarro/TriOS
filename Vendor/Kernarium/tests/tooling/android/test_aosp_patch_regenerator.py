#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from source_fixture import run


def write_fixture(
    root: Path, android_root: Path, topic: Path, verifier: Path, scripts: Path
) -> None:
    run("git", "init", "-q", str(topic))
    (topic / "base.txt").write_text("base\n")
    run("git", "add", "base.txt", cwd=topic)
    run("git", "commit", "-q", "-m", "fixture base", cwd=topic)
    base_commit = run("git", "rev-parse", "HEAD", cwd=topic)
    base_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)
    (topic / "first.txt").write_text("first\n")
    run("git", "add", "first.txt", cwd=topic)
    run("git", "commit", "-q", "-m", "fixture: first", cwd=topic)
    (topic / "second.txt").write_text("second\n")
    run("git", "add", "second.txt", cwd=topic)
    run("git", "commit", "-q", "-m", "fixture: second", cwd=topic)

    (android_root / "metadata").mkdir(parents=True)
    route_root = android_root / "patches/projects/p0001"
    (route_root / "series").mkdir(parents=True)
    (android_root / "tools").mkdir()
    shutil.copy2(verifier, android_root / "tools/verify-aosp-patches.py")
    os.chmod(android_root / "tools/verify-aosp-patches.py", 0o755)
    shutil.copy2(
        verifier.with_name("pinned_source.py"), android_root / "tools/pinned_source.py"
    )
    artifacts = root / "kernel/common/artifacts"
    artifacts.mkdir(parents=True)
    shutil.copy2(
        verifier.parents[3] / "kernel/common/artifacts/contract_checks.py",
        artifacts / "contract_checks.py",
    )
    (root / "scripts").mkdir()
    shutil.copy2(scripts / "patch_series.py", root / "scripts/patch_series.py")
    manifest = (
        '<manifest><default remote="fixture"/>'
        '<project name="fixture/project" path="fixture/project" '
        f'revision="{base_commit}" upstream="refs/tags/fixture"/></manifest>\n'
    )
    (android_root / "metadata/base-manifest.xml").write_text(manifest)
    manifest_digest = hashlib.sha256(manifest.encode()).hexdigest()
    (android_root / "metadata/lineage.env").write_text(
        "AOSP_SOURCE_SCHEMA=1\n"
        f"LINEAGE24_BASE_MANIFEST_SHA256='{manifest_digest}'\n"
    )
    empty_tree = "0" * 40
    (android_root / "patches/projects.tsv").write_text(
        "# SPDX-License-Identifier: Apache-2.0\n"
        "order\troute_id\tmanifest_remote\tmanifest_project\tcheckout_path\t"
        "manifest_revision\tbase_commit\tbase_tree\tfinal_tree\tseries_dir\n"
        f"1\tp0001\tfixture\tfixture/project\tfixture/project\trefs/tags/fixture\t"
        f"{base_commit}\t{base_tree}\t{empty_tree}\t"
        "patches/projects/p0001/series\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    generator = source_root / "android/lineage-24/tools/regenerate-aosp-patches.py"
    verifier = source_root / "android/lineage-24/tools/verify-aosp-patches.py"

    with tempfile.TemporaryDirectory(prefix="aosp-patch-regenerator-") as temporary:
        root = Path(temporary)
        android_root = root / "android/lineage-24"
        topic = root / "topic"
        write_fixture(root, android_root, topic, verifier, source_root / "scripts")
        command = (
            sys.executable,
            "-B",
            str(generator),
            "--android-root",
            str(android_root),
            "--route-id",
            "p0001",
            "--topic-checkout",
            str(topic),
        )
        run(*command)
        run(
            sys.executable,
            "-B",
            str(android_root / "tools/verify-aosp-patches.py"),
            "--android-root",
            str(android_root),
        )
        patches = sorted((android_root / "patches/projects/p0001/series").glob("*.patch"))
        if len(patches) != 2:
            raise SystemExit("AOSP patch-regenerator fixture did not emit two patches")
        listed = (android_root / "patches/projects/p0001/series/series.tsv").read_text()
        for name in ("fixture-first", "fixture-second"):
            if name not in listed:
                raise SystemExit(f"AOSP patch-regenerator fixture lost patch: {name}")
        final_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)
        if f"\t{final_tree}\t" not in (android_root / "patches/projects.tsv").read_text():
            raise SystemExit("AOSP patch regenerator did not record the topic tree")

        projects_before = (android_root / "patches/projects.tsv").read_bytes()
        series_before = (
            android_root / "patches/projects/p0001/series/series.tsv"
        ).read_bytes()
        (topic / "dirty.txt").write_text("dirty\n")
        rejected = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if rejected.returncode == 0:
            raise SystemExit("AOSP patch regenerator accepted a dirty topic checkout")
        if (android_root / "patches/projects.tsv").read_bytes() != projects_before:
            raise SystemExit("dirty-topic rejection changed projects.tsv")
        if (
            android_root / "patches/projects/p0001/series/series.tsv"
        ).read_bytes() != series_before:
            raise SystemExit("dirty-topic rejection changed the route series")

        verify_strict_contract(android_root)

    print("AOSP 17 patch-regenerator fixture passed")


def verify_strict_contract(android_root: Path) -> None:
    """The route table, manifest and digest checks refuse what they once let by."""
    verifier = android_root / "tools/verify-aosp-patches.py"
    projects = android_root / "patches/projects.tsv"
    manifest = android_root / "metadata/base-manifest.xml"
    metadata = android_root / "metadata/lineage.env"
    saved = {path: path.read_bytes() for path in (projects, manifest, metadata)}

    def restore() -> None:
        for path, payload in saved.items():
            if path.is_symlink():
                path.unlink()
            path.write_bytes(payload)

    def refused(case: str, expected: str) -> None:
        result = subprocess.run(
            (sys.executable, "-B", str(verifier), "--android-root", str(android_root)),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if result.returncode == 0 or expected not in result.stdout:
            raise SystemExit(
                f"AOSP patch verifier did not refuse {case} with '{expected}': "
                f"{result.stdout.strip()}"
            )
        restore()

    def write_manifest(text: str) -> None:
        manifest.write_text(text)
        manifest_digest = hashlib.sha256(text.encode()).hexdigest()
        metadata.write_text(
            "AOSP_SOURCE_SCHEMA=1\n"
            f"LINEAGE24_BASE_MANIFEST_SHA256='{manifest_digest}'\n"
        )

    # A line of only white space is a malformed row, not a blank line.
    projects.write_bytes(saved[projects] + b"   \n")
    lines = len(saved[projects].decode().splitlines()) + 1
    refused("a white-space line", f"projects.tsv:{lines}: malformed row")

    # A checkout path must be spelled in its normalized form everywhere.
    for spelling in ("fixture/project/", "fixture/./project", "fixture//project"):
        write_manifest(saved[manifest].decode().replace(
            'path="fixture/project"', f'path="{spelling}"'
        ))
        projects.write_text(saved[projects].decode().replace(
            "\tfixture/project\trefs/", f"\t{spelling}\trefs/"
        ))
        refused(
            f"the checkout path {spelling}",
            f"unsafe manifest checkout path: {spelling}",
        )

    # The pinned manifest is read only as a regular file.
    real = manifest.with_name("base-manifest.real.xml")
    real.write_bytes(saved[manifest])
    manifest.unlink()
    manifest.symlink_to(real.name)
    refused("a symlinked base manifest", "required regular file is missing")
    real.unlink()

    # A manifest with no projects pins nothing, even when no route names one.
    write_manifest("<manifest/>\n")
    header = next(
        line for line in saved[projects].decode().splitlines()
        if not line.startswith("#")
    )
    projects.write_text(header + "\n")
    refused("an empty base manifest", "resolved manifest has no projects")


if __name__ == "__main__":
    main()
