#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export only the pinned FEX files needed to assemble its ASM corpora."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from pinned_source import (  # noqa: E402
    FEX_CORPUS_INPUTS,
    manifest_identity,
    sha256_file,
    verify_checkout,
)


FIXED_FILES = (
    pathlib.Path("LICENSE"),
    pathlib.Path("unittests/Readme.md"),
    pathlib.Path("Scripts/json_asm_config_parse.py"),
    pathlib.Path("Scripts/json_config_parse.py"),
)


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"export-asm-source.py: {message}")


def export(source: pathlib.Path, output: pathlib.Path) -> None:
    try:
        version, expected_commit = manifest_identity("fex")
        verify_checkout(source, expected_commit, "FEX", FEX_CORPUS_INPUTS)
    except ValueError as error:
        fail(str(error))
    if output.exists() or output.is_symlink():
        fail(f"output already exists: {output}")

    asm_files = sorted((source / "unittests" / "ASM").rglob("*"))
    compat_asm_files = sorted(
        (source / "unittests" / "32Bit_ASM").rglob("*")
    )
    relative_files = list(FIXED_FILES)
    relative_files.extend(
        path.relative_to(source) for path in asm_files if path.is_file()
    )
    relative_files.extend(
        path.relative_to(source)
        for path in compat_asm_files
        if path.is_file()
    )
    if len(relative_files) < 2000:
        fail("the pinned FEX ASM source inventory is unexpectedly small")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = pathlib.Path(
        tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent)
    )
    try:
        records: list[dict[str, str]] = []
        for relative in relative_files:
            source_file = source / relative
            if not source_file.is_file():
                fail(f"missing pinned FEX input: {relative}")
            destination = temporary / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_file, destination)
            records.append(
                {"path": relative.as_posix(), "sha256": sha256_file(destination)}
            )
        identity = {
            "schema": 1,
            "source": "https://github.com/FEX-Emu/FEX.git",
            "version": version,
            "commit": expected_commit,
            "files": records,
        }
        (temporary / "fex-source.json").write_text(
            json.dumps(identity, indent=2, sort_keys=True) + "\n"
        )
        temporary.rename(output)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise

    print(f"exported {len(relative_files)} pinned FEX files to {output}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fex_source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    arguments = parser.parse_args()
    export(arguments.fex_source.resolve(), arguments.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
