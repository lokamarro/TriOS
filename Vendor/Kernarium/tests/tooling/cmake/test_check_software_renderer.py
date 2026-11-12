#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the software-renderer check separates the Simulator's slices."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


TIMEOUT_SECONDS = 120
# A Zink build has the generic software loader's entry points but no software
# Gallium screen of its own.
CLEAN = (
    'extern "C" void *sw_screen_create_zink() { return nullptr; }\n'
    'extern "C" void *drisw_init_screen() { return nullptr; }\n'
)
SOFTPIPE = (
    'extern "C" void *softpipe_create_screen() { return nullptr; }\n'
)
LLVMPIPE = (
    'extern "C" void *llvmpipe_create_screen() { return nullptr; }\n'
)
# LLVM's C API and its C++ namespace, which only the Simulator's llvmpipe links.
LLVM_C = (
    'extern "C" void *LLVMLinkInMCJIT() { return nullptr; }\n'
)
LLVM_CXX = (
    'namespace llvm { void *kernarium_fixture() { return nullptr; } }\n'
)
# A reference is not a definition: a build that calls the creator through a
# table it does not define carries no rasterizer.
REFERENCED = (
    'extern "C" void *softpipe_create_screen();\n'
    'void *kernarium_fixture() { return softpipe_create_screen(); }\n'
)
# The host provider names itself with this line; the name outlives a
# stripped symbol table.
MARKED = (
    'extern "C" const char *kernarium_marker() {\n'
    '    return "Virgl software rasterizer provider: software\\n";\n'
    '}\n'
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
        "xcrun", "--sdk", "iphonesimulator", "clang++", "-std=c++20",
        "-target", "arm64-apple-ios27.0-simulator", "-c", str(source_path),
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
    with tempfile.TemporaryDirectory(prefix="uml-software-renderer-") as directory:
        root = Path(directory)
        clean = compile_object(root, "clean", CLEAN)
        softpipe = compile_object(root, "softpipe", SOFTPIPE)
        llvmpipe = compile_object(root, "llvmpipe", LLVMPIPE)
        referenced = compile_object(root, "referenced", REFERENCED)
        marked = compile_object(root, "marked", MARKED)
        llvm_c = compile_object(root, "llvm_c", LLVM_C)
        llvm_cxx = compile_object(root, "llvm_cxx", LLVM_CXX)
        archive = root / "libmixed.a"
        archived = run([
            "xcrun", "libtool", "-static", "-o", str(archive),
            str(clean), str(softpipe),
        ])
        if archived.returncode != 0:
            raise SystemExit(f"could not archive the fixture:\n{archived.stdout}")
        # A framework binary nested in an XCFramework slice.
        slice_directory = (root / "MesaGallium.xcframework"
                           / "ios-arm64-simulator" / "MesaGallium.framework")
        slice_directory.mkdir(parents=True)
        shutil.copyfile(softpipe, slice_directory / "MesaGallium")
        empty = root / "Empty.xcframework"
        empty.mkdir()
        (empty / "Info.plist").write_text("", encoding="utf-8")

        cases = (
            ("clean object absent", "--absent", [clean], 0,
             "no software renderer in 1 binaries"),
            ("referenced creator absent", "--absent", [referenced], 0,
             "no software renderer in 1 binaries"),
            ("softpipe absent", "--absent", [softpipe], 1,
             "defines the software renderer's softpipe_create_screen"),
            ("llvmpipe absent", "--absent", [llvmpipe], 1,
             "defines the software renderer's llvmpipe_create_screen"),
            ("marker absent", "--absent", [marked], 1,
             "holds the software provider's marker"),
            ("archive member absent", "--absent", [archive], 1,
             "defines the software renderer's softpipe_create_screen"),
            ("nested slice absent", "--absent",
             [root / "MesaGallium.xcframework"], 1,
             "defines the software renderer's softpipe_create_screen"),
            ("softpipe present", "--present", [softpipe], 0,
             "the software renderer is in 1 of 1 binaries"),
            ("llvmpipe present", "--present", [llvmpipe], 0,
             "the software renderer is in 1 of 1 binaries"),
            ("nested slice present", "--present",
             [root / "MesaGallium.xcframework"], 0,
             "the software renderer is in 1 of 1 binaries"),
            ("clean object present", "--present", [clean], 1,
             "no binary defines a software Gallium screen"),
            ("referenced creator present", "--present", [referenced], 1,
             "no binary defines a software Gallium screen"),
            ("llvm c api absent", "--absent", [llvm_c], 1,
             "defines the LLVM symbol _LLVMLinkInMCJIT"),
            ("llvm c++ absent", "--absent", [llvm_cxx], 1,
             "defines the LLVM symbol __ZN4llvm"),
            ("llvm does not make a screen", "--present", [llvm_c], 1,
             "no binary defines a software Gallium screen"),
            ("llvmpipe with llvm present", "--present", [llvmpipe, llvm_c], 0,
             "the software renderer is in 1 of 2 binaries"),
            ("bundle without binaries", "--absent", [empty], 1,
             "no Mach-O binary"),
        )
        for label, mode, paths, expected, message in cases:
            checked = run([
                sys.executable, "-B", str(options.checker), mode,
                *map(str, paths),
            ])
            if checked.returncode != expected or message not in checked.stdout:
                failures.append(
                    f"{label}: status {checked.returncode}, expected "
                    f"{expected} with {message!r}:\n{checked.stdout}"
                )

    if failures:
        raise SystemExit("\n".join(failures))
    print("the software-renderer check separated 17 cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
