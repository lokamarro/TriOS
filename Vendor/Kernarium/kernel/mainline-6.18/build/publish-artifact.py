#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import sys
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"mainline kernel artifact publisher: {message}")


def absolute_directory(path: Path, label: str) -> Path:
    if not path.is_absolute():
        fail(f"{label} must be an absolute local path")
    resolved = path.resolve()
    if not resolved.is_dir():
        fail(f"{label} is not a directory: {resolved}")
    return resolved


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--slice", choices=("arm64", "x86_64"), default="arm64")
    parser.add_argument(
        "--product",
        choices=("linux", "android"),
        default="linux",
        help="the product the build configured; the android product publishes "
        "the android-6.18 artifact family and is checked against its symbol lists",
    )
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--release-id", required=True)
    parser.add_argument(
        "--objcopy",
        type=Path,
        required=True,
        help="llvm-objcopy of the toolchain that built the kernel; it strips "
        "the DWARF that only pahole consumes",
    )
    args = parser.parse_args()

    line_root = Path(__file__).resolve().parents[1]
    outer_root = line_root.parents[1]
    common_root = outer_root / "kernel/common"
    sys.path.insert(0, str(common_root / "artifacts"))
    from kernel_artifact import (  # noqa: PLC0415
        RELEASE_ID_RE,
        parse_env,
        publish_artifact,
        verify_config_policy,
    )

    source = absolute_directory(args.source, "kernel source")
    build = absolute_directory(args.build, "kernel build")
    if not args.output_root.is_absolute():
        fail("output root must be an absolute local path")
    output_root = args.output_root.resolve()
    if RELEASE_ID_RE.fullmatch(args.release_id) is None:
        fail("release ID has an unsafe format")
    objcopy = args.objcopy
    if not objcopy.is_absolute() or not objcopy.is_file() or not os.access(
        objcopy, os.X_OK
    ):
        fail("--objcopy must name an absolute executable")
    if args.product == "android" and args.slice != "arm64":
        fail("the android product supports only the arm64 slice")

    metadata_path = line_root / "patches/metadata.env"
    if args.slice == "x86_64":
        # The overlay's upstream identity is the arm64 line's; the shared
        # publisher reads it through the kernel series loader.
        metadata_path = line_root / "slices/x86_64/patches/metadata.env"
        if parse_env(metadata_path).get("UML_KERNEL_SLICE") != "x86_64":
            fail("x86_64 patch metadata has the wrong slice identity")

    def check_release(release: str, metadata: dict[str, str]) -> None:
        if not release.startswith(metadata["UML_KERNEL_RELEASE_VERSION"]):
            fail("built kernel release does not match tracked release version")

    def verify_android_policy(staging: Path) -> None:
        verify_config_policy(
            staging / "kernel/config",
            line_root / "config/android-required-builtins.txt",
            line_root / "config/android-forbidden-options.txt",
        )

    destination = publish_artifact(
        fail,
        family="android-6.18" if args.product == "android" else "mainline-6.18",
        kernel_slice=args.slice,
        source=source,
        source_label="kernel source",
        build=build,
        output_root=output_root,
        release_id=args.release_id,
        objcopy=objcopy,
        common_root=common_root,
        metadata_path=metadata_path,
        check_release=check_release,
        verify_staged=(
            verify_android_policy if args.product == "android" else None
        ),
    )
    print(f"published {args.slice} {args.product} kernel artifact: {destination}")


if __name__ == "__main__":
    main()
