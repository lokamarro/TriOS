#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check which FEX checkouts the corpus producers accept.

The prepared FEX source carries the macOS prefix's embedding series on the
pinned commit. The producers accept it only while the files they read are
the pinned ones, and the K semantics source stays exactly at its pin.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import pathlib
import subprocess
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"test_fex_pinned_source.py: {message}")


def git(repository: pathlib.Path, *arguments: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout.strip()


def commit_file(repository: pathlib.Path, relative: str, text: str) -> str:
    path = repository / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    git(repository, "add", relative)
    git(repository, "commit", "--quiet", "-m", f"change {relative}")
    return git(repository, "rev-parse", "HEAD")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pinned-source", type=pathlib.Path, required=True)
    arguments = parser.parse_args()
    specification = importlib.util.spec_from_file_location(
        "pinned_source", arguments.pinned_source
    )
    require(specification is not None and specification.loader is not None,
            "cannot load pinned_source.py")
    pinned_source = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(pinned_source)
    inputs = pinned_source.FEX_CORPUS_INPUTS

    def accepted(source: pathlib.Path, commit: str, paths: tuple[str, ...]) -> bool:
        try:
            pinned_source.verify_checkout(source, commit, "FEX", paths)
        except ValueError:
            return False
        return True

    with tempfile.TemporaryDirectory(prefix="uml-fex-pinned-source-") as directory:
        root = pathlib.Path(directory)
        require(not accepted(root, "0" * 40, inputs), "accepted a directory without Git")

        source = root / "fex"
        source.mkdir()
        git(source, "init", "--quiet")
        commit_file(source, "LICENSE", "license\n")
        commit_file(source, "Scripts/json_config_parse.py", "parser\n")
        pin = commit_file(source, "unittests/ASM/Test.asm", "hlt\n")

        require(accepted(source, pin, ()), "rejected a clean checkout of the pin")
        require(accepted(source, pin, inputs), "rejected the pin with corpus inputs")

        (source / "unittests" / "ASM" / "Test.asm").write_text("ud2\n")
        require(not accepted(source, pin, inputs), "accepted a dirty checkout")
        git(source, "checkout", "--quiet", "--", ".")
        (source / "untracked").write_text("stray\n")
        require(not accepted(source, pin, inputs), "accepted an untracked file")
        (source / "untracked").unlink()

        series = commit_file(source, "FEXCore/Source/Core.cpp", "embedded\n")
        require(not accepted(source, pin, ()),
                "a checkout without corpus inputs accepted a later commit")
        require(accepted(source, pin, inputs),
                "rejected a series that leaves every corpus input alone")

        commit_file(source, "unittests/ASM/Test.asm", "ud2\n")
        require(not accepted(source, pin, inputs), "accepted a changed corpus input")
        git(source, "reset", "--quiet", "--hard", series)
        commit_file(source, "Scripts/json_config_parse.py", "changed\n")
        require(not accepted(source, pin, inputs), "accepted a changed parser")
        git(source, "reset", "--quiet", "--hard", series)

        git(source, "checkout", "--quiet", "--orphan", "unrelated")
        git(source, "commit", "--quiet", "-m", "unrelated history")
        require(not accepted(source, pin, inputs),
                "accepted a checkout not based on the pin")
    print("FEX pinned-source checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
