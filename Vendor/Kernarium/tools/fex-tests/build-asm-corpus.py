#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a pinned FEX handwritten assembly test corpus."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from pinned_source import (  # noqa: E402
    FEX_CORPUS_INPUTS,
    manifest_identity,
    sha256_file,
    verify_checkout,
)


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"build-asm-corpus.py: {message}")


def verify_export(source: pathlib.Path, expected_commit: str) -> None:
    identity_path = source / "fex-source.json"
    try:
        identity = json.loads(identity_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        fail(f"invalid exported FEX source identity: {error}")
    if identity.get("schema") != 1 or identity.get("commit") != expected_commit:
        fail("exported FEX source identity does not match the pinned commit")
    records = identity.get("files")
    if not isinstance(records, list) or len(records) < 2000:
        fail("exported FEX source inventory is unexpectedly small")

    expected_paths: set[pathlib.Path] = set()
    for record in records:
        if not isinstance(record, dict):
            fail("exported FEX source has a malformed file record")
        relative = pathlib.Path(str(record.get("path", "")))
        if relative.is_absolute() or ".." in relative.parts or relative in expected_paths:
            fail(f"exported FEX source has an unsafe path: {relative}")
        expected_paths.add(relative)
        path = source / relative
        if not path.is_file() or sha256_file(path) != record.get("sha256"):
            fail(f"exported FEX source hash mismatch: {relative}")

    actual_paths = {
        path.relative_to(source)
        for path in source.rglob("*")
        if path.is_file() and path != identity_path
    }
    if actual_paths != expected_paths:
        fail("exported FEX source inventory differs from its identity")


def verify_source(source: pathlib.Path, expected_commit: str) -> None:
    if (source / ".git").exists():
        verify_checkout(source, expected_commit, "FEX", FEX_CORPUS_INPUTS)
        return
    verify_export(source, expected_commit)


def listed_tests(path: pathlib.Path) -> set[str]:
    tests: set[str] = set()
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            tests.add(line)
    return tests


def embedded_config(source: pathlib.Path) -> tuple[str, dict[str, object]]:
    text = source.read_text()
    marker = "%ifdef CONFIG"
    if marker not in text:
        fail(f"missing CONFIG record: {source}")
    config_text = text.split(marker, 1)[1].split("%endif", 1)[0].strip()
    parsed = json.loads(config_text)
    return config_text, {key.upper(): value for key, value in parsed.items()}


def assemble(
    source: pathlib.Path,
    destination: pathlib.Path,
    include_directory: pathlib.Path,
    nasm: str,
    mode: str,
) -> None:
    temporary = destination.with_suffix(".tmp.asm")
    prelude = "BITS 64\n"
    if mode == "i386":
        prelude = (
            "BITS 32\n"
            "org 10000h\n"
            "mov eax, 0x17\n"
            "mov ds, ax\n"
            "mov es, ax\n"
        )
    temporary.write_text(prelude + source.read_text() + "\nret\n")
    try:
        subprocess.run(
            [
                nasm,
                "-i",
                str(include_directory) + os.sep,
                str(temporary),
                "-o",
                str(destination),
            ],
            check=True,
        )
    finally:
        temporary.unlink(missing_ok=True)


def build(source: pathlib.Path, output: pathlib.Path, mode: str) -> None:
    try:
        version, expected_commit = manifest_identity("fex")
        verify_source(source, expected_commit)
    except ValueError as error:
        fail(str(error))
    if output.exists() or output.is_symlink():
        fail(f"output already exists: {output}")
    nasm = os.environ.get("UML_FEX_NASM", "nasm")
    nasm_path = shutil.which(nasm)
    if nasm_path is None:
        fail("NASM is required")

    # Upstream's Known_Failures_host names the programs whose expectations
    # fail on native x86; the 32-bit suite has no such list.
    if mode == "x86-64":
        asm_root = source / "unittests" / "ASM"
        test_prefix = "Test_"
        known_failure_lists = ("Known_Failures_host",)
    else:
        asm_root = source / "unittests" / "32Bit_ASM"
        test_prefix = "Test_32Bit_"
        known_failure_lists = ()
    include_directory = source / "unittests" / "ASM" / "Includes"
    parser_directory = source / "Scripts"
    sys.path.insert(0, str(parser_directory))
    from json_config_parse import parse_json  # type: ignore[import-not-found]

    disabled = listed_tests(asm_root / "Disabled_Tests")
    known_failures: set[str] = set()
    for name in known_failure_lists:
        known_failures |= listed_tests(asm_root / name)
    sources = sorted(asm_root.rglob("*.asm"))
    if not sources:
        fail(f"the pinned FEX checkout has no {mode} ASM tests")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_root = pathlib.Path(
        tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent)
    )
    try:
        tests_directory = temporary_root / "tests"
        records: list[dict[str, object]] = []
        for asm_source in sources:
            relative = asm_source.relative_to(asm_root)
            test_name = f"{test_prefix}{relative.as_posix()}"
            destination = tests_directory / relative
            binary = destination.with_suffix(destination.suffix + ".bin")
            config = destination.with_suffix(destination.suffix + ".config.bin")
            binary.parent.mkdir(parents=True, exist_ok=True)

            config_text, parsed = embedded_config(asm_source)
            assemble(asm_source, binary, include_directory, nasm_path, mode)
            parse_json(config_text, str(config))
            records.append(
                {
                    "test": test_name,
                    "source": relative.as_posix(),
                    "binary": binary.relative_to(temporary_root).as_posix(),
                    "config": config.relative_to(temporary_root).as_posix(),
                    "binary_sha256": sha256_file(binary),
                    "config_sha256": sha256_file(config),
                    "host_features": parsed.get("HOSTFEATURES", []),
                    "disabled_upstream": test_name in disabled,
                    "known_failure_host": test_name in known_failures,
                }
            )
        unknown = known_failures - {str(record["test"]) for record in records}
        if unknown:
            fail("Known_Failures_host names no test: " + ", ".join(sorted(unknown)))

        metadata = {
            "schema": 1,
            "source": "https://github.com/FEX-Emu/FEX.git",
            "version": version,
            "commit": expected_commit,
            "mode": mode,
            "test_count": len(records),
            "disabled_count": sum(
                bool(record["disabled_upstream"]) for record in records
            ),
            "known_failure_host_count": sum(
                bool(record["known_failure_host"]) for record in records
            ),
            "tests": records,
        }
        (temporary_root / "corpus.json").write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n"
        )
        temporary_root.rename(output)
    except BaseException:
        shutil.rmtree(temporary_root, ignore_errors=True)
        raise

    print(
        f"built {len(sources)} FEX {version} {mode} ASM tests at {output}",
        flush=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fex_source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument(
        "--mode", choices=("x86-64", "i386"), default="x86-64"
    )
    arguments = parser.parse_args()
    build(
        arguments.fex_source.resolve(),
        arguments.output.resolve(),
        arguments.mode,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
