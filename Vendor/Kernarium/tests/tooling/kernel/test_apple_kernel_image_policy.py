#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the signed kernel carrier verifier and accepted-artifact caching."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def synthetic_elf(
    *,
    late_nonexecutable_overlap: bool,
    reverse_overlap_order: bool = False,
    overlap_writable: bool = False,
) -> bytes:
    image = bytearray(0x10000)
    programs = [
        (1, 4, 0, 0xC00000000, 0xC00000000, 0x1000, 0x1000, 0x4000),
        (
            1,
            5,
            0x4000,
            0xC00004000,
            0xC00004000,
            0x1800 if late_nonexecutable_overlap else 0x1000,
            0x1800 if late_nonexecutable_overlap else 0x1000,
            0x4000,
        ),
        (
            1,
            6 if overlap_writable else 4,
            0x6000 if late_nonexecutable_overlap else 0x8000,
            0xC00006000 if late_nonexecutable_overlap else 0xC00008000,
            0xC00006000 if late_nonexecutable_overlap else 0xC00008000,
            0x1000,
            0x1000,
            0x4000,
        ),
    ]
    if reverse_overlap_order:
        programs[1:] = reversed(programs[1:])
    identity = b"\x7fELF\x02\x01\x01" + bytes(9)
    ELF_HEADER.pack_into(
        image,
        0,
        identity,
        3,
        183,
        1,
        0xC00004000,
        ELF_HEADER.size,
        0,
        0,
        ELF_HEADER.size,
        PROGRAM_HEADER.size,
        len(programs),
        0,
        0,
        0,
    )
    for index, program in enumerate(programs):
        PROGRAM_HEADER.pack_into(
            image,
            ELF_HEADER.size + index * PROGRAM_HEADER.size,
            *program,
        )
    return bytes(image)


def verify_carrier_tool(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="kernarium-carrier-policy-") as directory:
        valid = Path(directory) / "valid-vmlinux"
        invalid = Path(directory) / "invalid-vmlinux"
        reverse_invalid = Path(directory) / "reverse-invalid-vmlinux"
        valid.write_bytes(synthetic_elf(late_nonexecutable_overlap=False))
        invalid.write_bytes(synthetic_elf(late_nonexecutable_overlap=True))
        reverse_invalid.write_bytes(
            synthetic_elf(
                late_nonexecutable_overlap=True,
                reverse_overlap_order=True,
                overlap_writable=True,
            )
        )
        accepted = subprocess.run(
            [sys.executable, "-B", str(verifier), "--image", str(valid)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(accepted.returncode == 0, f"safe carrier rejected:\n{accepted.stderr}")
        unsafe_cases = (
            (
                invalid,
                "non-executable PT_LOAD follows signed executable bytes",
            ),
            (
                reverse_invalid,
                "signed executable PT_LOAD shares a host page with writable",
            ),
        )
        for unsafe, diagnostic in unsafe_cases:
            rejected = subprocess.run(
                [sys.executable, "-B", str(verifier), "--image", str(unsafe)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            require(
                rejected.returncode != 0,
                f"mixed loader page was accepted: {unsafe.name}",
            )
            require(
                diagnostic in rejected.stderr,
                "carrier verifier rejected the hazard for the wrong reason:\n"
                f"{rejected.stderr}",
            )


def verify_accepted_artifact_cache(artifact_cmake: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="accepted-artifact-cache-") as directory:
        temporary = Path(directory)
        managed = temporary / "managed"
        external = temporary / "external" / "copied-artifact"
        script = temporary / "verify.cmake"
        script.write_text(
            f'''cmake_minimum_required(VERSION 3.27)
include([=[{artifact_cmake.resolve()}]=])

# A first configure whose value already names the accepted default follows it.
set(TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v1"}]=] CACHE PATH "test")
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v1"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v1"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "accepted-v1"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "accepted artifact did not follow its first default")
endif()

# A value which still equals the prior generated default follows acceptance.
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v2"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v2"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "accepted-v2"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "accepted artifact did not follow the new default")
endif()

# An explicit whole-bundle override remains stable even when it is another
# publication below the normal managed artifact root.
set(TEST_MANAGED_ARTIFACT [=[{managed / "local-override"}]=]
    CACHE PATH "test" FORCE)
unset(TEST_MANAGED_ARTIFACT)
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v3"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "local-override"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "accepted-v3"}]=] OR
   TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "managed whole-bundle override was replaced")
endif()

# An accepted default may later equal the pinned path without changing its
# override identity. It must remain pinned across the following change too.
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "local-override"}]=]
    "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "local-override"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "local-override"}]=] OR
   TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "matching accepted default reclassified an override")
endif()
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v4"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "local-override"}]=] OR
   TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "managed override followed a later default")
endif()

# Clearing an override opts back into the current accepted default.
set(TEST_MANAGED_ARTIFACT "" CACHE PATH "test" FORCE)
unset(TEST_MANAGED_ARTIFACT)
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v4"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v4"}]=])
    message(FATAL_ERROR "cleared override did not restore acceptance")
endif()
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v5"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v5"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "restored acceptance did not follow a later default")
endif()

set(TEST_EXTERNAL_ARTIFACT [=[{external}]=] CACHE PATH "test")
uml_cache_accepted_artifact_path(
    TEST_EXTERNAL_ARTIFACT [=[{managed / "accepted-v3"}]=] "test")
if(NOT TEST_EXTERNAL_ARTIFACT STREQUAL [=[{external}]=])
    message(FATAL_ERROR "external artifact override was replaced")
endif()
''',
            encoding="utf-8",
        )
        checked = subprocess.run(
            ["cmake", "-P", str(script)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(
            checked.returncode == 0,
            "accepted artifact cache policy failed:\n"
            f"{checked.stdout}{checked.stderr}",
        )


def verify_accepted_import(artifact_cmake: Path) -> None:
    """The accepted import follows its descriptor until a caller pins the bundle.

    The bundle the descriptor names is also checked against it.
    """
    with tempfile.TemporaryDirectory(prefix="accepted-import-") as directory:
        root = Path(directory).resolve()
        common = root / "common"
        source = root / "source"
        build = root / "build"
        descriptor = root / "accepted.env"
        recorded = root / "arguments.txt"
        refusal = root / "refusal.txt"
        (common / "abi").mkdir(parents=True)
        (common / "artifacts").mkdir()
        shutil.copyfile(artifact_cmake, common / "artifacts/KernelArtifact.cmake")
        (common / "abi/identity.env").write_text("", encoding="utf-8")
        # The fixture verifier records its command line and admits any bundle,
        # unless a refusal message is waiting for it.
        (common / "artifacts/verify-kernel-artifact.py").write_text(
            "import os, sys\n"
            f"open({str(recorded)!r}, 'w').write(' '.join(sys.argv[1:]))\n"
            f"if os.path.exists({str(refusal)!r}):\n"
            f"    sys.exit(open({str(refusal)!r}).read())\n",
            encoding="utf-8",
        )
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            f'''cmake_minimum_required(VERSION 3.27)
project(accepted_import LANGUAGES NONE)
include([=[{common}/artifacts/KernelArtifact.cmake]=])
uml_import_accepted_kernel_artifact(
    TARGET fixture_kernel_artifact
    DESCRIPTOR [=[{descriptor}]=]
    CACHE_VARIABLE FIXTURE_KERNEL_ARTIFACT_DIR
    DESCRIPTION "fixture"
    FAMILY android-6.18
    SLICE arm64)
get_property(_watched DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
if(NOT [=[{descriptor}]=] IN_LIST _watched)
    message(FATAL_ERROR "configure does not watch the descriptor")
endif()
''',
            encoding="utf-8",
        )
        managed = source / ".build/kernel/artifacts"
        pinned = root / "pinned"
        same_name = root / "elsewhere/bundle-v6"
        # Each step: the descriptor's bundle, extra configure arguments, the
        # bundle the import must verify, and whether it still follows.
        steps = (
            ("bundle-v1", [], managed / "bundle-v1", True),
            ("bundle-v2", [], managed / "bundle-v2", True),
            ("bundle-v3", [f"-DFIXTURE_KERNEL_ARTIFACT_DIR={pinned}"], pinned, False),
            ("bundle-v4", [], pinned, False),
            ("bundle-v5", ["-DFIXTURE_KERNEL_ARTIFACT_DIR="], managed / "bundle-v5", True),
            ("bundle-v6", [f"-DFIXTURE_KERNEL_ARTIFACT_DIR={same_name}"], same_name, False),
        )
        for name, extra, bundle, follows in steps:
            descriptor.write_text(
                f"UML_KERNEL_ARTIFACT_DIRECTORY_NAME='{name}'\n", encoding="utf-8"
            )
            configured = subprocess.run(
                ["cmake", "-G", "Ninja", "-S", str(source), "-B", str(build), *extra],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            require(
                configured.returncode == 0,
                f"accepted import failed at {name}:\n"
                f"{configured.stdout}{configured.stderr}",
            )
            cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
            variable = "FIXTURE_KERNEL_ARTIFACT_DIR"
            for entry in (
                f"{variable}:PATH={bundle}",
                f"{variable}_ACCEPTED_DEFAULT:INTERNAL={managed / name}",
                f"{variable}_FOLLOW_ACCEPTED:INTERNAL={'ON' if follows else 'OFF'}",
            ):
                require(entry in cache.splitlines(), f"at {name}, the cache lacks {entry}")
            expected = (
                f"--artifact {bundle} --common-root {common} "
                f"--abi-metadata {common / 'abi/identity.env'} --compatible "
                "--expected-family android-6.18 --expected-slice arm64 "
                "--expected-release-series 6.18"
            )
            # Only a bundle that carries the descriptor's name is compared with
            # it; a bundle pinned under another name is not the descriptor's.
            if bundle.name == name:
                expected += f" --descriptor {descriptor}"
            require(
                recorded.read_text(encoding="utf-8") == expected,
                f"at {name}, the verifier ran as: {recorded.read_text(encoding='utf-8')}",
            )

        # A verifier that refuses the bundle, as it does for a descriptor that
        # drifted from its manifest, fails the configure with its message.
        message = "descriptor FINAL_TREE=aaa, but the bundle manifest has bbb"
        refusal.write_text(message, encoding="utf-8")
        refused = subprocess.run(
            ["cmake", "-G", "Ninja", "-S", str(source), "-B", str(build)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(
            refused.returncode != 0
            and "Cannot import the compatible kernel artifact" in refused.stderr
            and message in refused.stderr,
            f"a refused bundle did not fail the configure:\n{refused.stdout}{refused.stderr}",
        )


def verify_artifact_configure_inputs(artifact_cmake: Path) -> None:
    """An imported bundle is re-verified when anything its verifier reads changes."""
    with tempfile.TemporaryDirectory(prefix="artifact-configure-inputs-") as directory:
        root = Path(directory)
        common = root / "common"
        artifact = root / "artifact"
        watched = [
            common / "abi/identity.env",
            common / "abi/import-policy.tsv",
            common / "abi/darwin_uml_first.h",
            common / "abi/darwin_uml_second.h",
            common / "abi/bridge_imports.py",
            common / "artifacts/contract_checks.py",
            common / "artifacts/kernel_artifact.py",
            common / "artifacts/verify-kernel-artifact.py",
            artifact / "kernel-artifact.json",
            artifact / "kernel/vmlinux",
        ]
        for path in watched:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("", encoding="utf-8")
        # The fixture verifier admits any bundle.
        watched[7].write_text("raise SystemExit(0)\n", encoding="utf-8")
        source = root / "source"
        source.mkdir()
        expected = "\n    ".join(f"[=[{path}]=]" for path in watched)
        (source / "CMakeLists.txt").write_text(
            f'''cmake_minimum_required(VERSION 3.27)
project(artifact_configure_inputs LANGUAGES NONE)
include([=[{artifact_cmake.resolve()}]=])
uml_import_kernel_artifact(
    TARGET fixture_kernel_artifact
    ARTIFACT_ROOT [=[{artifact}]=]
    COMMON_ROOT [=[{common}]=]
    EXPECTED_FAMILY mainline-6.18
    EXPECTED_SLICE arm64
    EXPECTED_RELEASE_SERIES 6.18)
get_property(_watched DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
foreach(_input IN ITEMS
    {expected})
    if(NOT _input IN_LIST _watched)
        message(FATAL_ERROR "configure does not watch ${{_input}}")
    endif()
endforeach()
''',
            encoding="utf-8",
        )
        configured = subprocess.run(
            ["cmake", "-G", "Ninja", "-S", str(source), "-B", str(root / "build")],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(
            configured.returncode == 0,
            "artifact import does not watch its verifier's inputs:\n"
            f"{configured.stdout}{configured.stderr}",
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-cmake", type=Path, required=True)
    parser.add_argument("--carrier-verifier", type=Path, required=True)
    arguments = parser.parse_args()

    verify_accepted_artifact_cache(arguments.artifact_cmake)
    verify_accepted_import(arguments.artifact_cmake)
    verify_artifact_configure_inputs(arguments.artifact_cmake)
    verify_carrier_tool(arguments.carrier_verifier)

    print("Apple kernel carrier and artifact cache policy verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
