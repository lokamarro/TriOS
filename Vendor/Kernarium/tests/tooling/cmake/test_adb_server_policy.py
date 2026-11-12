#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the adb server policy checker on fixtures, then on the tree.

The fixture runs name their own allowlist, so their result never depends on
the repository's. The tree run uses the checked-in allowlist and must be clean.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

# A scan lists the files with Git and reads the scripts: a second or two. Only a
# wedged checker may fail on time; the test's own TIMEOUT is the hard bound.
CHECKER_WEDGE_SECONDS = 60


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def write(root: Path, name: str, text: str, mode: int | None = None) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if mode is not None:
        path.chmod(mode)


def run(
    checker: Path, root: Path, allowlist: Path | None = None
) -> tuple[int, set[tuple[str, int, str]], str]:
    command = [sys.executable, "-B", str(checker), "--source-root", str(root)]
    if allowlist is not None:
        command += ["--allowlist", str(allowlist)]
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        text=True,
        timeout=CHECKER_WEDGE_SECONDS,
    )
    parsed: set[tuple[str, int, str]] = set()
    for line in result.stdout.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in (
            "server-variable",
            "port-flag",
            "kill-server",
        ):
            path, _, number = parts[0].rpartition(":")
            parsed.add((path, int(number), parts[1]))
    return result.returncode, parsed, result.stdout


def make_fixture(root: Path) -> None:
    write(
        root,
        "variable.py",
        "\n".join(
            [
                'environment["ANDROID_ADB_SERVER_PORT"] = "5389"',  # 1
                'os.environ.get("ANDROID_ADB_SERVER_ADDRESS")',  # 2
                "export ADB_SERVER_SOCKET=tcp:5555",  # 3
                'subprocess.run(["adb", "-s", serial, "shell", "id"])',  # 4 pass
            ]
        )
        + "\n",
    )
    write(
        root,
        "flag.sh",
        "\n".join(
            [
                "adb -P 5389 devices",  # 1
                'cmd=(adb -s "$serial" shell)',  # 2 pass
                "ssh -P 22 host",  # 3 pass: not adb
                "adb push -p file /sdcard/",  # 4 pass: another flag
            ]
        )
        + "\n",
    )
    write(
        root,
        "flag_list.py",
        'command = ["adb", "-P", port, "-s", serial]\n'
        'run([adb_program, "-P", port, "devices"])\n'
        'run([ADB, "-P", port, "devices"])\n'
        'run([ADB, "-p", "devices"])\n',  # a lowercase -p is another flag
    )
    write(
        root,
        "stop.sh",
        "adb kill-server\n",
    )
    # A shebang makes an extensionless file a script, even in a directory.
    write(root, "bin/tool", "#!/bin/sh\nadb disconnect \"$serial\"\nadb kill-server\n", 0o755)
    write(root, "bin/readme", "adb kill-server is mentioned here\n")
    # Prose and non-scripts are not scanned.
    write(
        root,
        "notes.md",
        "adb kill-server, ANDROID_ADB_SERVER_PORT and adb -P are discussed\n",
    )
    write(root, "layout.json", '{"ANDROID_ADB_SERVER_PORT": "5389"}\n')
    # The shared server's own words pass.
    write(
        root,
        "shared.py",
        "\n".join(
            [
                'run(["adb", "connect", serial])',
                'run(["adb", "-s", serial, "get-state"])',
                'run(["adb", "disconnect", serial])',
                'run(["adb", "kill-servers"])',  # not the command
            ]
        )
        + "\n",
    )
    # Not first-party scripts: a build tree and vendored sources.
    write(root, ".build/generated.py", "adb kill-server\n")
    write(root, "external/upstream.sh", "adb kill-server\n")
    # An allowed exception: rule by rule, so another rule in it still fails.
    write(
        root,
        "stranger/runner.py",
        "\n".join(
            [
                'environment["ANDROID_ADB_SERVER_PORT"] = str(port)',  # 1 allowed
                'run([adb, "kill-server"])',  # 2 allowed
                'run([adb, "-P", str(port), "devices"])',  # 3 port-flag
            ]
        )
        + "\n",
    )
    write(root, "allow.tsv", "# comment\n\n" + "\n".join(
        [
            "stranger/runner.py\tserver-variable\tthe key rule's server",
            "stranger/runner.py\tkill-server\tstops it",
            "gone.py\tkill-server\ta file that no longer exists",
            "shared.py\tport-flag\ta rule the file never breaks",
        ]
    ) + "\n")


EXPECTED = {
    ("variable.py", 1, "server-variable"),
    ("variable.py", 2, "server-variable"),
    ("variable.py", 3, "server-variable"),
    ("flag.sh", 1, "port-flag"),
    ("flag_list.py", 1, "port-flag"),
    ("flag_list.py", 2, "port-flag"),
    ("flag_list.py", 3, "port-flag"),
    ("stop.sh", 1, "kill-server"),
    ("bin/tool", 3, "kill-server"),
    ("stranger/runner.py", 3, "port-flag"),
}
STALE = ("gone.py", "shared.py")


def check_fixture(checker: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="adb-policy-") as name:
        root = Path(name)
        make_fixture(root)
        status, found, output = run(checker, root, root / "allow.tsv")
        require(status == 1, f"a violating tree must fail:\n{output}")
        require(found == EXPECTED, f"findings differ:\n{output}")
        for path in STALE:
            require(
                f"{path}: allowlist:" in output,
                f"an allowlist entry that matches nothing must fail ({path}):\n{output}",
            )

        # The same tree without its violations is clean.
        for path in {finding[0] for finding in EXPECTED}:
            (root / path).unlink()
        (root / "allow.tsv").write_text(
            "stranger/runner.py\tserver-variable\treason\n", encoding="utf-8"
        )
        write(root, "stranger/runner.py", 'environment["ANDROID_ADB_SERVER_PORT"] = "1"\n')
        status, found, output = run(checker, root, root / "allow.tsv")
        require(status == 0 and not found, f"a clean tree must pass:\n{output}")

        # A malformed allowlist is a usage error, not a pass.
        (root / "allow.tsv").write_text("stranger/runner.py\tbogus\treason\n")
        status, _, output = run(checker, root, root / "allow.tsv")
        require(status != 0 and "expected" in output, f"bad allowlist:\n{output}")


def check_tree(checker: Path, source_root: Path) -> None:
    status, _, output = run(checker, source_root)
    require(
        status == 0,
        "first-party scripts use the shared adb server (docs/testing.md); "
        f"the checker found:\n{output}",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--checker", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    options = parser.parse_args()
    check_fixture(options.checker)
    check_tree(options.checker, options.source_root)
    print("adb server policy: fixtures and tree pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
