#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Update the Apple SDK under a configured scratch tree and rebuild it.

An Xcode update installs a newer SDK at another path or rewrites the selected
one in place. A tree configured before the update must notice at its next
build, run configure again, and take its SDK libraries and frameworks from the
SDK that is installed now rather than keep the first one's cached paths.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

FIXTURE = """\
cmake_minimum_required(VERSION 3.27)
project(uml_apple_sdk_fixture LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH "@REPOSITORY@/cmake")
include(UmlAppleSdk)
uml_track_apple_sdk()
uml_find_sdk_library(fixture_math m)
uml_find_sdk_framework(fixture_foundation Foundation)
file(WRITE "${CMAKE_BINARY_DIR}/found.txt"
    "${CMAKE_OSX_SYSROOT}\\n${fixture_math}\\n${fixture_foundation}\\n")
add_custom_target(fixture_all ALL COMMAND ${CMAKE_COMMAND} -E true)
"""

FAKE_XCRUN = """\
#!/bin/sh
# Answers `xcrun --sdk <name> --show-sdk-path` with the path in the file next to it.
exec cat "$(dirname "$0")/sdk-path"
"""


def require(condition: bool, message: str) -> None:
    if not condition:
        sys.exit(f"test_apple_sdk_update.py: {message}")


def make_sdk(root: Path, name: str) -> Path:
    sdk = root / name
    (sdk / "usr" / "lib").mkdir(parents=True)
    (sdk / "usr" / "lib" / "libm.tbd").write_text("--- !tapi-tbd\n", encoding="utf-8")
    (sdk / "System" / "Library" / "Frameworks" / "Foundation.framework").mkdir(
        parents=True
    )
    (sdk / "SDKSettings.json").write_text("{}\n", encoding="utf-8")
    return sdk


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--make-program", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--repository", type=Path, required=True)
    options = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="uml-apple-sdk-update.") as scratch:
        root = Path(scratch).resolve()
        bin_dir = root / "bin"
        bin_dir.mkdir()
        xcrun = bin_dir / "xcrun"
        xcrun.write_text(FAKE_XCRUN, encoding="utf-8")
        xcrun.chmod(0o755)
        environment = dict(os.environ, PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}")
        source = root / "source"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            FIXTURE.replace("@REPOSITORY@", str(options.repository.resolve())),
            encoding="utf-8",
        )
        build = root / "build"

        def select(sdk: Path) -> None:
            (bin_dir / "sdk-path").write_text(f"{sdk}\n", encoding="utf-8")

        def run(*command: str) -> str:
            result = subprocess.run(
                command,
                check=False,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            require(result.returncode == 0, f"{' '.join(command)} failed:\n{result.stdout}")
            return result.stdout

        def found() -> list[str]:
            return (build / "found.txt").read_text(encoding="utf-8").split("\n")[:3]

        def rebuild() -> str:
            return run(str(options.cmake), "--build", str(build))

        first = make_sdk(root, "first.sdk")
        select(first)
        run(
            str(options.cmake),
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={options.make_program}",
            f"-DCMAKE_TOOLCHAIN_FILE={options.toolchain.resolve()}",
            "-S",
            str(source),
            "-B",
            str(build),
        )
        require(
            found()
            == [
                str(first),
                str(first / "usr/lib/libm.tbd"),
                str(first / "System/Library/Frameworks/Foundation.framework"),
            ],
            f"the first configure found {found()}",
        )

        # An untouched SDK is not an update.
        require("Re-running CMake" not in rebuild(), "an unchanged SDK reconfigured the tree")

        # The update moves the SDK to another path and removes the old one.
        second = make_sdk(root, "second.sdk")
        select(second)
        shutil.rmtree(first)
        require("Re-running CMake" in rebuild(), "a moved SDK did not reconfigure the tree")
        require(
            found()
            == [
                str(second),
                str(second / "usr/lib/libm.tbd"),
                str(second / "System/Library/Frameworks/Foundation.framework"),
            ],
            f"after the SDK moved the tree still uses {found()}",
        )
        cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
        require(str(first) not in cache, "the cache still names the removed SDK")
        require("Re-running CMake" not in rebuild(), "the reconfigured tree reconfigured again")

        # The update rewrites the selected SDK in place.
        time.sleep(0.05)
        settings = second / "SDKSettings.json"
        settings.write_text('{"Version": "next"}\n', encoding="utf-8")
        require("Re-running CMake" in rebuild(), "a rewritten SDK did not reconfigure the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
