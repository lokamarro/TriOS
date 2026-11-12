#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Configure scratch trees through the Apple toolchain at each deployment target.

Every Apple product builds at the one floor the toolchain names. A tree whose
CMake cache or command line holds another target must fail to configure rather
than build at it.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

PLATFORMS = ("macos", "ios", "ios-simulator")
CACHE_ENTRY = re.compile(r"^CMAKE_OSX_DEPLOYMENT_TARGET:[A-Z]+=(.*)$", re.MULTILINE)


def require(condition: bool, message: str) -> None:
    if not condition:
        sys.exit(f"test_apple_deployment_floor.py: {message}")


def write_source(root: Path, languages: str) -> Path:
    source = root / "source"
    source.mkdir()
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.27)\n"
        f"project(uml_deployment_floor_fixture LANGUAGES {languages})\n",
        encoding="utf-8",
    )
    return source


def configure(
    cmake: Path, toolchain: Path, source: Path, build: Path, platform: str, *extra: str
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(cmake),
            "-S",
            str(source),
            "-B",
            str(build),
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
            f"-DUML_APPLE_PLATFORM={platform}",
            *extra,
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def cached_target(build: Path) -> str:
    match = CACHE_ENTRY.search((build / "CMakeCache.txt").read_text(encoding="utf-8"))
    require(match is not None, f"{build} caches no CMAKE_OSX_DEPLOYMENT_TARGET")
    return match.group(1)


def require_refusal(
    result: subprocess.CompletedProcess[str], label: str, stale: str, floor: str
) -> None:
    text = re.sub(r"\s+", " ", result.stdout)
    require(result.returncode != 0, f"{label} configured at {stale}:\n{result.stdout}")
    for needle in (
        f"CMAKE_OSX_DEPLOYMENT_TARGET={stale}",
        f"floor {floor}",
        "--fresh",
        "binary directory",
    ):
        require(needle in text, f"{label} refusal lacks {needle!r}:\n{result.stdout}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    options = parser.parse_args()
    toolchain = options.toolchain.resolve()

    with tempfile.TemporaryDirectory(prefix="uml-deployment-floor.") as scratch:
        root = Path(scratch)
        bare = write_source(root, "NONE")

        # The toolchain's own default is the floor, one for every platform.
        # Targets below and above it are the others a tree must refuse.
        floors = set()
        for platform in PLATFORMS:
            build = root / f"{platform}-probe"
            result = configure(options.cmake, toolchain, bare, build, platform)
            require(result.returncode == 0, f"{platform}: {result.stdout}")
            floors.add(cached_target(build))
        require(len(floors) == 1, f"the platforms disagree on the floor: {floors}")
        floor = floors.pop()
        version = re.fullmatch(r"([0-9]+)\.0", floor)
        require(version is not None, f"the floor {floor!r} is not a major version")
        stale = f"{int(version.group(1)) - 1}.0"
        above = f"{int(version.group(1)) + 1}.0"

        # The floor configures on every platform and a second configure of the
        # same tree stays at it. An empty value is no target and takes the floor.
        for platform in PLATFORMS:
            for label, extra in (
                ("default", ()),
                ("explicit", (f"-DCMAKE_OSX_DEPLOYMENT_TARGET={floor}",)),
                ("empty", ("-DCMAKE_OSX_DEPLOYMENT_TARGET=",)),
            ):
                build = root / f"{platform}-{label}"
                for attempt, arguments in (("first", extra), ("second", ())):
                    result = configure(
                        options.cmake, toolchain, bare, build, platform, *arguments
                    )
                    require(
                        result.returncode == 0,
                        f"{platform} {label} {attempt} configure failed:\n"
                        f"{result.stdout}",
                    )
                    require(
                        cached_target(build) == floor,
                        f"{platform} {label} caches {cached_target(build)}",
                    )

        # A tree configured before the floor moved caches the old target. The
        # toolchain refuses it, without rewriting the cache, and both a new
        # binary directory and --fresh recover.
        for platform in PLATFORMS:
            build = root / f"{platform}-stale"
            result = configure(options.cmake, toolchain, bare, build, platform)
            require(result.returncode == 0, f"{platform}: {result.stdout}")
            cache = build / "CMakeCache.txt"
            cache.write_text(
                CACHE_ENTRY.sub(
                    f"CMAKE_OSX_DEPLOYMENT_TARGET:STRING={stale}",
                    cache.read_text(encoding="utf-8"),
                ),
                encoding="utf-8",
            )
            refused = configure(options.cmake, toolchain, bare, build, platform)
            require_refusal(refused, f"{platform} stale cache", stale, floor)
            require(
                cached_target(build) == stale,
                f"{platform}: the refused configure rewrote the cache",
            )
            recovered = configure(
                options.cmake, toolchain, bare, build, platform, "--fresh"
            )
            require(
                recovered.returncode == 0,
                f"{platform}: --fresh did not recover:\n{recovered.stdout}",
            )
            require(cached_target(build) == floor, f"{platform}: --fresh kept {stale}")

        # Any other target passed on the command line is refused, below the
        # floor or above it.
        for passed in (stale, above):
            result = configure(
                options.cmake,
                toolchain,
                bare,
                root / f"passed-{passed}",
                "macos",
                f"-DCMAKE_OSX_DEPLOYMENT_TARGET={passed}",
            )
            require_refusal(result, f"passed {passed}", passed, floor)

        # A project with a compiler runs the toolchain again inside CMake's
        # compiler probes, which must see the floor too.
        compiled_root = root / "compiled"
        compiled_root.mkdir()
        result = configure(
            options.cmake,
            toolchain,
            write_source(compiled_root, "C"),
            compiled_root / "build",
            "macos",
        )
        require(result.returncode == 0, f"compiled configure failed:\n{result.stdout}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
