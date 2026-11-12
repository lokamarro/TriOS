#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Keep first-party scripts on the one shared adb server.

The host runs one adb server (tcp:5037) and every script names its guest with
`-s 127.0.0.1:<port>`. A script that starts a private server leaves it behind
when it is killed, and the leftover costs a core retrying the USB device the
shared server holds. So a tracked or new unignored script (`.py`, `.sh`,
`.bash`, `.zsh`, or an extensionless file with a shebang) may not:

- select a server: ANDROID_ADB_SERVER_PORT, ANDROID_ADB_SERVER_ADDRESS or
  ADB_SERVER_SOCKET (rule `server-variable`);
- pass `-P` to adb (rule `port-flag`); or
- run `kill-server`, which stops the shared server unless something else
  selected another one (rule `kill-server`).

adb-server-policy-allow.tsv lists the exceptions: `path`, `rule` and the
reason, separated by tabs. An entry that matches nothing fails. Findings are
printed as `path:line: rule: excerpt`; the exit status is 1 for any finding.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

RULES = {
    "server-variable": re.compile(
        r"ANDROID_ADB_SERVER_(?:PORT|ADDRESS)|ADB_SERVER_SOCKET"
    ),
    "port-flag": re.compile(r"(?i:\badb).*(?<![\w-])-P(?![\w-])"),
    "kill-server": re.compile(r"(?<![\w-])kill-server(?![\w-])"),
}
SCRIPT_SUFFIXES = frozenset({".py", ".sh", ".bash", ".zsh"})
# The checker and its test carry the patterns and the fixtures that violate them.
SELF_PATHS = frozenset(
    {
        "tests/tooling/cmake/check_adb_server_policy.py",
        "tests/tooling/cmake/test_adb_server_policy.py",
    }
)
EXCLUDED_ROOT_DIRECTORIES = frozenset({".build", ".git", "external", "linux"})
DEFAULT_ALLOWLIST = Path(__file__).resolve().parent / "adb-server-policy-allow.tsv"
GIT_LIST_SECONDS = 60


def candidate_paths(root: Path) -> list[str]:
    """Tracked files plus new unignored ones; every file of a tree without Git."""
    if (root / ".git").exists():
        listing = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-co", "--exclude-standard", "-z"],
            check=True,
            stdout=subprocess.PIPE,
            stdin=subprocess.DEVNULL,
            timeout=GIT_LIST_SECONDS,
        ).stdout
        paths = [name for name in listing.decode().split("\0") if name]
    else:
        paths = []
        for directory, names, files in os.walk(root):
            names[:] = [n for n in names if n not in EXCLUDED_ROOT_DIRECTORIES]
            paths += [
                os.path.relpath(os.path.join(directory, name), root) for name in files
            ]
    return sorted(
        path
        for path in set(paths)
        if path.split("/", 1)[0] not in EXCLUDED_ROOT_DIRECTORIES
    )


def is_script(path: Path, name: str) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    suffix = Path(name).suffix
    if suffix:
        return suffix in SCRIPT_SUFFIXES
    with path.open("rb") as handle:
        return handle.read(2) == b"#!"


def load_allowlist(path: Path) -> dict[tuple[str, str], str]:
    allowed: dict[tuple[str, str], str] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3 or fields[1] not in RULES or not fields[2].strip():
            raise SystemExit(
                f"{path}:{number}: expected `path<TAB>rule<TAB>reason` with one of "
                + ", ".join(RULES)
            )
        allowed[(fields[0], fields[1])] = fields[2]
    return allowed


def scan(root: Path, allowed: dict[tuple[str, str], str]) -> list[str]:
    findings: list[str] = []
    used: set[tuple[str, str]] = set()
    for name in candidate_paths(root):
        path = root / name
        if name in SELF_PATHS or not is_script(path, name):
            continue
        for number, line in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            for rule, pattern in RULES.items():
                if not pattern.search(line):
                    continue
                if (name, rule) in allowed:
                    used.add((name, rule))
                else:
                    findings.append(f"{name}:{number}: {rule}: {line.strip()[:120]}")
    for name, rule in sorted(set(allowed) - used):
        findings.append(f"{name}: allowlist: no `{rule}` match, remove the entry")
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--allowlist", type=Path, default=DEFAULT_ALLOWLIST)
    options = parser.parse_args()
    findings = scan(options.source_root.resolve(), load_allowlist(options.allowlist))
    for finding in findings:
        print(finding)
    if findings:
        print(
            f"{len(findings)} finding(s): use the shared adb server with -s serials "
            "(see docs/testing.md)",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
