#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from release_common import (  # noqa: E402
    PAYLOAD_SPECS,
    fail,
    load_manifest,
    render_acceptance,
    verify_release,
)


def reject_nonlocal_syntax(raw: str) -> None:
    if "://" in raw or (":" in raw and not raw.startswith("/")) or "@" in raw:
        fail("release source must use local filesystem path syntax")


def copy_declared(source: Path, staging: Path) -> None:
    paths = ["release-manifest.json", *sorted(PAYLOAD_SPECS)]
    for relative in paths:
        source_path = source / relative
        destination = staging / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_path, destination, follow_symlinks=False)
        shutil.copymode(source_path, destination, follow_symlinks=False)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--destination-root", type=Path)
    parser.add_argument("--acceptance", type=Path)
    parser.add_argument(
        "--print-acceptance",
        action="store_true",
        help=(
            "print the accepted-release.env text that selects the candidate, "
            "after checking the candidate against it, and install nothing"
        ),
    )
    args = parser.parse_args()

    reject_nonlocal_syntax(args.source)
    source = Path(args.source)
    if not source.is_absolute():
        fail("release source must be an absolute local path")

    android_root = Path(__file__).resolve().parents[1]
    if args.print_acceptance:
        if args.destination_root or args.acceptance:
            fail("--print-acceptance takes only --source")
        text = render_acceptance(load_manifest(source))
        with tempfile.TemporaryDirectory(prefix="print-acceptance.") as scratch:
            descriptor = Path(scratch) / "accepted-release.env"
            descriptor.write_text(text, encoding="utf-8")
            # The verifiers report what they checked on standard output, in
            # this process and in the tools it runs. Standard error carries
            # that report so standard output is the descriptor alone.
            sys.stdout.flush()
            saved_stdout = os.dup(1)
            os.dup2(2, 1)
            try:
                verify_release(source, android_root, acceptance_path=descriptor)
            finally:
                sys.stdout.flush()
                os.dup2(saved_stdout, 1)
                os.close(saved_stdout)
        sys.stdout.write(text)
        return

    outer_root = android_root.parents[1]
    destination_root = args.destination_root or outer_root / ".build/android/releases"
    acceptance = args.acceptance or android_root / "metadata/accepted-release.env"
    if not destination_root.is_absolute():
        fail("destination root must be an absolute local path")
    if not acceptance.is_absolute():
        acceptance = (Path.cwd() / acceptance).resolve()

    manifest = verify_release(source, android_root, acceptance_path=acceptance)
    destination_root.mkdir(parents=True, exist_ok=True)
    if destination_root.is_symlink():
        fail("destination root must not be a symlink")
    destination = destination_root / manifest["directory_name"]
    if destination.exists() or destination.is_symlink():
        fail("accepted release is already installed")

    staging_parent = Path(tempfile.mkdtemp(
        prefix=f".{manifest['directory_name']}.", dir=destination_root
    ))
    staging_path = staging_parent / manifest["directory_name"]
    staging_path.mkdir()
    try:
        copy_declared(source, staging_path)
        verify_release(staging_path, android_root, acceptance_path=acceptance)
        os.replace(staging_path, destination)
        staging_parent.rmdir()
    except BaseException:
        shutil.rmtree(staging_parent, ignore_errors=True)
        raise

    print(f"installed Android release: {destination}")


if __name__ == "__main__":
    main()
