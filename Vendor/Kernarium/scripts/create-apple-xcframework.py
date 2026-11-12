#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage the project's three Apple static-library slices as an XCFramework."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
from pathlib import Path
import plistlib
import shutil
import stat
import sys
import tempfile


PACKAGE_NAME = "UMLEmulator.xcframework"
LIBRARY_NAME = "libuml_emulator.a"
PUBLIC_HEADERS = ("uml/abi_identity.h", "uml/emulator.h")

SLICE_POLICIES = {
    "ios": {
        "identifier": "ios-arm64",
        "platform": "ios",
    },
    "macos": {
        "identifier": "macos-arm64",
        "platform": "macos",
    },
    "ios-simulator": {
        "identifier": "ios-arm64-simulator",
        "platform": "ios",
        "variant": "simulator",
    },
}


class StagingError(RuntimeError):
    pass


def _regular_file(path: Path, description: str) -> None:
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError as error:
        raise StagingError(f"missing {description}: {path}") from error
    if not stat.S_ISREG(mode):
        raise StagingError(f"{description} is not a regular file: {path}")


def _directory(path: Path, description: str) -> None:
    try:
        mode = path.lstat().st_mode
    except FileNotFoundError as error:
        raise StagingError(f"missing {description}: {path}") from error
    if not stat.S_ISDIR(mode):
        raise StagingError(f"{description} is not a directory: {path}")


def _validate_header_tree(header_root: Path) -> None:
    _directory(header_root, "public header root")
    actual: list[str] = []
    for path in sorted(header_root.rglob("*")):
        relative = path.relative_to(header_root).as_posix()
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            raise StagingError(f"public header tree contains a symlink: {path}")
        if stat.S_ISREG(mode):
            actual.append(relative)
        elif not stat.S_ISDIR(mode):
            raise StagingError(
                f"public header tree contains a non-regular entry: {path}"
            )
    if tuple(actual) != PUBLIC_HEADERS:
        raise StagingError(
            "public header tree must contain exactly "
            f"{list(PUBLIC_HEADERS)!r}, found {actual!r}"
        )


def _parse_slices(values: list[str]) -> dict[str, Path]:
    slices: dict[str, Path] = {}
    for value in values:
        if value.count("=") != 1:
            raise StagingError(
                f"malformed --slice {value!r}; expected PLATFORM=INSTALL_ROOT"
            )
        platform, root_text = value.split("=", 1)
        if platform not in SLICE_POLICIES:
            raise StagingError(f"unsupported Apple slice platform: {platform!r}")
        if not root_text:
            raise StagingError(f"slice {platform!r} has an empty install root")
        if platform in slices:
            raise StagingError(f"duplicate Apple slice platform: {platform}")
        root = Path(root_text)
        if not root.is_absolute():
            raise StagingError(f"slice install root must be absolute: {root}")
        slices[platform] = root

    missing = sorted(set(SLICE_POLICIES) - set(slices))
    if missing:
        raise StagingError(f"missing required Apple slices: {', '.join(missing)}")
    return slices


def _library_entry(platform: str) -> dict[str, object]:
    policy = SLICE_POLICIES[platform]
    entry: dict[str, object] = {
        "BinaryPath": LIBRARY_NAME,
        "HeadersPath": "Headers",
        "LibraryIdentifier": policy["identifier"],
        "LibraryPath": LIBRARY_NAME,
        "SupportedArchitectures": ["arm64"],
        "SupportedPlatform": policy["platform"],
    }
    if "variant" in policy:
        entry["SupportedPlatformVariant"] = policy["variant"]
    return entry


def stage_xcframework(slices: dict[str, Path], output: Path) -> None:
    if not output.is_absolute():
        raise StagingError(f"XCFramework output must be absolute: {output}")
    if output.name != PACKAGE_NAME:
        raise StagingError(f"XCFramework output must be named {PACKAGE_NAME}: {output}")
    if output.exists() or output.is_symlink():
        raise StagingError(f"refusing to replace existing XCFramework output: {output}")

    inputs: dict[str, tuple[Path, Path]] = {}
    for platform, root in slices.items():
        _directory(root, f"{platform} install root")
        library = root / "lib" / LIBRARY_NAME
        headers = root / "include"
        _regular_file(library, f"{platform} archive")
        _validate_header_tree(headers)
        inputs[platform] = (library, headers)

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_root = Path(
        tempfile.mkdtemp(prefix=f".{PACKAGE_NAME}.", dir=output.parent)
    )
    staged = temporary_root / PACKAGE_NAME
    try:
        staged.mkdir()
        for platform in ("ios", "macos", "ios-simulator"):
            policy = SLICE_POLICIES[platform]
            library, headers = inputs[platform]
            slice_root = staged / str(policy["identifier"])
            slice_root.mkdir()
            shutil.copyfile(library, slice_root / LIBRARY_NAME)
            shutil.copytree(headers, slice_root / "Headers", symlinks=False)

        plist = {
            "AvailableLibraries": [
                _library_entry(platform)
                for platform in ("ios", "macos", "ios-simulator")
            ],
            "CFBundlePackageType": "XFWK",
            "XCFrameworkFormatVersion": "1.0",
        }
        with (staged / "Info.plist").open("wb") as stream:
            plistlib.dump(plist, stream, fmt=plistlib.FMT_XML, sort_keys=False)
        os.replace(staged, output)
    finally:
        shutil.rmtree(temporary_root, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--slice",
        action="append",
        default=[],
        metavar="PLATFORM=INSTALL_ROOT",
        help="installed macos, ios, or ios-simulator slice",
    )
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    try:
        stage_xcframework(_parse_slices(arguments.slice), arguments.output)
    except (OSError, StagingError) as error:
        print(f"create-apple-xcframework.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
