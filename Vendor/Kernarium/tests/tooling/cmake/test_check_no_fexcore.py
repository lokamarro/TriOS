#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the iOS FEXCore check finds FEXCore however it is packaged."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


TIMEOUT_SECONDS = 120
CLEAN = "int kernarium_fixture(int value) { return value + 1; }\n"
PLANTED = (
    "namespace FEXCore { int Embedded(int value) { return value * 3; } }\n"
    "int kernarium_fixture(int value) { return FEXCore::Embedded(value); }\n"
)
# FEXCore names its code mappings; the name outlives a stripped symbol table.
STRIPPED = (
    'extern "C" const char *kernarium_name() { return "FEXMemJIT"; }\n'
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=TIMEOUT_SECONDS,
    )


def compile_object(root: Path, name: str, source: str) -> Path:
    source_path = root / f"{name}.cpp"
    source_path.write_text(source, encoding="utf-8")
    output = root / f"{name}.o"
    built = run([
        "xcrun", "--sdk", "iphoneos", "clang++", "-std=c++20",
        "-target", "arm64-apple-ios27.0", "-c", str(source_path),
        "-o", str(output),
    ])
    if built.returncode != 0:
        raise SystemExit(f"could not compile {name}:\n{built.stdout}")
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", type=Path, required=True)
    options = parser.parse_args()

    failures = []
    with tempfile.TemporaryDirectory(prefix="uml-no-fexcore-") as directory:
        root = Path(directory)
        clean = compile_object(root, "clean", CLEAN)
        planted = compile_object(root, "planted", PLANTED)
        stripped = compile_object(root, "stripped", STRIPPED)
        stripped_copy = root / "stripped-nosyms.o"
        shutil.copyfile(stripped, stripped_copy)
        strip = run(["xcrun", "strip", "-x", str(stripped_copy)])
        if strip.returncode != 0:
            raise SystemExit(f"could not strip the fixture:\n{strip.stdout}")
        archive = root / "libplanted.a"
        archived = run([
            "xcrun", "libtool", "-static", "-o", str(archive),
            str(clean), str(planted),
        ])
        if archived.returncode != 0:
            raise SystemExit(f"could not archive the fixture:\n{archived.stdout}")
        # A framework binary nested in an application bundle.
        bundle = root / "Fixture.app" / "Frameworks" / "Fixture.framework"
        bundle.mkdir(parents=True)
        shutil.copyfile(planted, bundle / "Fixture")
        (root / "Fixture.app" / "Info.plist").write_text("", encoding="utf-8")
        empty = root / "Empty.app"
        empty.mkdir()
        (empty / "Info.plist").write_text("", encoding="utf-8")

        cases = (
            ("clean object", [clean], 0, "no FEXCore in 1 binaries"),
            ("planted object", [planted], 1, "FEXCore symbol"),
            ("stripped name", [stripped_copy], 1, "FEXMemJIT"),
            ("planted archive member", [archive], 1, "FEXCore symbol"),
            ("nested framework", [root / "Fixture.app"], 1, "FEXCore symbol"),
            ("bundle without binaries", [empty], 1, "no Mach-O binary"),
        )
        for label, paths, expected, message in cases:
            checked = run([
                sys.executable, "-B", str(options.checker),
                *map(str, paths),
            ])
            if checked.returncode != expected or message not in checked.stdout:
                failures.append(
                    f"{label}: status {checked.returncode}, expected "
                    f"{expected} with {message!r}:\n{checked.stdout}"
                )

    if failures:
        raise SystemExit("\n".join(failures))
    print("the iOS FEXCore check accepted 1 and rejected 5 products")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
