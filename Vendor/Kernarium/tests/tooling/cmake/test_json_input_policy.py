#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise maintained JSON discovery and duplicate-member diagnostics."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def run(validator: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(validator), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--validator", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    arguments = parser.parse_args()

    maintained = run(
        arguments.validator,
        "--source-root",
        str(arguments.source_root),
    )
    require(
        maintained.returncode == 0,
        f"maintained JSON inputs failed validation:\n{maintained.stdout}",
    )

    with tempfile.TemporaryDirectory(prefix="uml-json-policy-") as name:
        root = Path(name)
        top_level = root / "top-level.json"
        top_level.write_text(
            '{"preset": 1, "preset": 2}\n', encoding="utf-8"
        )
        nested = root / "nested.json"
        nested.write_text(
            '{"outer": [{"option": true, "option": false}]}\n',
            encoding="utf-8",
        )
        jsonc = root / "valid.jsonc"
        jsonc.write_text(
            '{\n'
            '  // Comments and trailing commas are valid JSONC.\n'
            '  "url": "https://example.invalid/a//b",\n'
            '  "nested": {"marker": "/* text */",},\n'
            '}\n',
            encoding="utf-8",
        )

        top_result = run(arguments.validator, "--input", str(top_level))
        require(
            top_result.returncode == 1,
            f"top-level duplicate was accepted:\n{top_result.stdout}",
        )
        require(
            f"{top_level.resolve()}: duplicate JSON member at $.preset"
            in top_result.stdout,
            f"top-level duplicate lacks file and key path:\n{top_result.stdout}",
        )

        nested_result = run(arguments.validator, "--input", str(nested))
        require(
            nested_result.returncode == 1,
            f"nested duplicate was accepted:\n{nested_result.stdout}",
        )
        require(
            f"{nested.resolve()}: duplicate JSON member at "
            "$.outer[0].option" in nested_result.stdout,
            f"nested duplicate lacks file and key path:\n{nested_result.stdout}",
        )

        jsonc_result = run(arguments.validator, "--input", str(jsonc))
        require(
            jsonc_result.returncode == 0,
            f"valid JSONC was rejected:\n{jsonc_result.stdout}",
        )

    print("JSON input duplicate-key policy tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
