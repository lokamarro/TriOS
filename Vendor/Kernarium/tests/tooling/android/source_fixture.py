# SPDX-License-Identifier: GPL-2.0-or-later
"""Git fixture helpers shared by the Android source-contract tooling tests."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import os
import subprocess
from pathlib import Path


def run(
    *command: str,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> str:
    """Run a fixture command and return its stripped output; exit on failure."""
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"source fixture command failed: {' '.join(command)}\n"
            f"{result.stderr.decode(errors='replace')}"
        )
    return result.stdout.decode(errors="strict").strip()


def commit(topic: Path, path: str, payload: str, subject: str, timestamp: int) -> None:
    """Commit one file to a fixture topic at a fixed time."""
    destination = topic / path
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(payload)
    run("git", "add", path, cwd=topic)
    environment = os.environ.copy()
    date = f"{timestamp} +0000"
    environment["GIT_AUTHOR_DATE"] = date
    environment["GIT_COMMITTER_DATE"] = date
    run(
        "git",
        "commit",
        "-q",
        "-m",
        subject,
        cwd=topic,
        env=environment,
    )


def parse_env(path: Path) -> dict[str, str]:
    """Read a KEY='value' metadata file."""
    result: dict[str, str] = {}
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        result[key] = value.strip("'")
    return result
