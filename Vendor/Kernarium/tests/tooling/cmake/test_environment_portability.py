#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the environment-portability checker on fixtures, then on the tree.

The fixture runs name their own denylist and allowlist, so their result never
depends on the developer's configuration. The tree runs use normal denylist
discovery (a site's own denylist, when it keeps one) and again the generic
rules alone; both must be clean.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# Only a wedged checker may fail on time: a scan takes about ten seconds
# alone and several times that at gate concurrency. The hard bounds are this
# subprocess timeout and the test's own TIMEOUT.
CHECKER_WEDGE_SECONDS = 120


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


class Checker:
    def __init__(self, checker: Path, home: Path | None) -> None:
        """A checker with a private HOME, or with the invoking environment."""
        self.checker = checker
        self.home = home

    def run(
        self, *arguments: str, environ: dict[str, str] | None = None
    ) -> subprocess.CompletedProcess[str]:
        environment = dict(os.environ)
        if self.home is not None:
            for key in ("UML_PORTABILITY_DENYLIST", "XDG_CONFIG_HOME"):
                environment.pop(key, None)
            environment["HOME"] = str(self.home)
        environment.update(environ or {})
        return subprocess.run(
            [sys.executable, "-B", str(self.checker), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=environment,
            timeout=CHECKER_WEDGE_SECONDS,
        )


def write(root: Path, name: str, text: str) -> None:
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def findings(output: str) -> set[tuple[str, int, str]]:
    """Parse `path:line: RULE: excerpt` lines into (path, line, rule)."""
    parsed: set[tuple[str, int, str]] = set()
    for line in output.splitlines():
        parts = line.split(": ", 2)
        if len(parts) == 3 and parts[1] in ("R1", "R2", "R3", "R4", "R5", "R6"):
            path, _, number = parts[0].rpartition(":")
            parsed.add((path, int(number), parts[1]))
    return parsed


def make_fixture(root: Path) -> None:
    write(
        root,
        "paths.txt",
        "\n".join(
            [
                "/Users/someone/work",  # 1 R1
                "/Volumes/Data/linux",  # 2 R1
                "/home/bob/src",  # 3 R1
                "/private/var/folders/ab/cd/T",  # 4 R1
                "/Users/NAME/x /Users/<name>/x /Users/$USER/x",  # 5 pass
                "/Users/Shared/x /Volumes/<volume>/linux",  # 6 pass
                "/home/uml/x /home/debian/x /home/../x",  # 7 pass
                "see (/Volumes/Data)",  # 8 R1
                "usr/home/bob /opt/Users/bob",  # 9 pass
            ]
        )
        + "\n",
    )
    write(
        root,
        "addresses.txt",
        "\n".join(
            [
                "192.168.1.5",  # 1 R2
                "ssh 10.1.2.3",  # 2 R2
                "172.16.0.1 172.31.255.1",  # 3 R2
                "100.64.1.1 169.254.1.1",  # 4 R2
                "fd12:3456:789a::1",  # 5 R2
                "host Mac.local",  # 6 R2
                "10.0.2.15 8.8.8.8 172.32.0.1 100.128.0.1",  # 7 pass
                "192.168.1.5.7 v10.1.2.3 300.168.1.1",  # 8 pass
                "options.local = 1; self.local.x; vendor.local(3)",  # 9 pass
            ]
        )
        + "\n",
    )
    write(
        root,
        "App.xcodeproj/project.pbxproj",
        "\n".join(
            [
                "DEVELOPMENT_TEAM = ABCDE12345;",  # 1 R3
                'DEVELOPMENT_TEAM = "";',  # 2 pass
                "DEVELOPMENT_TEAM = $(TEAM);",  # 3 pass
                "PRODUCT_BUNDLE_IDENTIFIER = com.acme.App;",  # 4 R3
                "PRODUCT_BUNDLE_IDENTIFIER = org.example.App;",  # 5 pass
                'PRODUCT_BUNDLE_IDENTIFIER = "$(PREFIX).App";',  # 6 pass
                "PRODUCT_BUNDLE_IDENTIFIER = org.linux-um-darwin.App;",  # 7 pass
                "DEVELOPMENT_TEAM = ${TEAM};",  # 8 pass
                "DEVELOPMENT_TEAM = TEAM$(X);",  # 9 R3
            ]
        )
        + "\n",
    )
    # A base xcconfig with an empty team passes, whatever follows it; a
    # literal team fails.
    write(
        root,
        "Config/Signing.xcconfig",
        "\n".join(
            [
                "// base",  # 1
                "DEVELOPMENT_TEAM =",  # 2 pass
                "",  # 3
                '#include? "Signing.local.xcconfig"',  # 4
                "DEVELOPMENT_TEAM = ABCDE12345",  # 5 R3
            ]
        )
        + "\n",
    )
    # The canonical form of `xcprojformatter`: pretty printed, quoted keys
    # with their conditions inside the quotes, string-array values.
    write(
        root,
        "App.xcodeproj/project.xcproj",
        "\n".join(
            [
                "{",  # 1
                '  "targets": [',  # 2
                "    {",  # 3
                '      "build-settings": {',  # 4
                '        "DEVELOPMENT_TEAM": "ABCDE12345",',  # 5 R3
                '        "DEVELOPMENT_TEAM[sdk=iphoneos*]": "ABCDE12345",',  # 6 R3
                '        "DEVELOPMENT_TEAM[sdk=iphoneos*]": [',  # 7
                '          "ABCDE12345",',  # 8 R3
                "        ],",  # 9
                '        "DEVELOPMENT_TEAM": "",',  # 10 pass
                '        "DEVELOPMENT_TEAM[config=Debug]": "$(TEAM)",',  # 11 pass
                '        "DEVELOPMENT_TEAM[config=Release]": "${TEAM}",',  # 12 pass
                '        "DEVELOPMENT_TEAM[sdk=iphoneos*]": [],',  # 13 pass
                '        "PRODUCT_BUNDLE_IDENTIFIER": "com.acme.App",',  # 14 R3
                '        "PRODUCT_BUNDLE_IDENTIFIER[config=Debug]": "com.acme.Debug",',  # 15 R3
                '        "PRODUCT_BUNDLE_IDENTIFIER[sdk=iphoneos*]": [',  # 16
                '          "$(PREFIX).App",',  # 17 pass
                '          "com.acme.App",',  # 18 R3
                "        ],",  # 19
                '        "PRODUCT_BUNDLE_IDENTIFIER": "org.example.App",',  # 20 pass
                '        "PRODUCT_BUNDLE_IDENTIFIER": "$(PREFIX).App",',  # 21 pass
                '        "PRODUCT_BUNDLE_IDENTIFIER": "${PREFIX}.App",',  # 22 pass
                '        "PRODUCT_BUNDLE_IDENTIFIER": "org.linux-um-darwin.App",',  # 23 pass
                '        "PRODUCT_BUNDLE_IDENTIFIER": "",',  # 24 pass
                '        "INFOPLIST_KEY_PRODUCT_BUNDLE_IDENTIFIER": "com.acme.X",',  # 25 pass
                '        "MY_DEVELOPMENT_TEAM": "ABCDE12345",',  # 26 pass
                '        "DEVELOPMENT_TEAM_NAME": "ABCDE12345",',  # 27 pass
                '        "PRODUCT_NAME": "com.acme.App",',  # 28 pass
                '        "DEVELOPMENT_TEAM":',  # 29
                '          "ABCDE12345",',  # 30 R3
                "      },",
                "    },",
                "  ],",
                "}",
            ]
        )
        + "\n",
    )
    # Hand-written JSON5: unquoted and single-quoted keys and values, compact
    # objects, comments, and an element list on one line.
    write(
        root,
        "Compact.xcodeproj/project.xcproj",
        "\n".join(
            [
                "{",  # 1
                "  targets: [",  # 2
                "    { 'build-settings': { DEVELOPMENT_TEAM: 'ABCDE12345' } },",  # 3 R3
                "    { 'build-settings': {"
                " 'DEVELOPMENT_TEAM[sdk=iphoneos*]': ['ABCDE12345'] } },",  # 4 R3
                "    { 'build-settings': { PRODUCT_BUNDLE_IDENTIFIER: 'com.acme.App' } },",  # 5 R3
                "    { 'build-settings': {"
                " \"PRODUCT_BUNDLE_IDENTIFIER[config=Debug]\": 'com.acme.Debug' } },",  # 6 R3
                "    { 'build-settings': { 'PRODUCT_BUNDLE_IDENTIFIER': [ // one",  # 7
                "      'org.example.A', /* literal 'com.acme.B' */ 'com.acme.C' ] } },",  # 8 R3
                "    { 'build-settings': { DEVELOPMENT_TEAM: '$(TEAM)',"
                " PRODUCT_BUNDLE_IDENTIFIER: 'org.example.App' } },",  # 9 pass
                "    { 'build-settings': { DEVELOPMENT_TEAM: [ // 'ABCDE12345'",  # 10
                "      '${TEAM}' ] } },",  # 11 pass
                "    { 'build-settings': { DEVELOPMENT_TEAM: 'it\\'s', B: 1 } },",  # 12 R3, escaped quote
                "  ],",
                "}",
            ]
        )
        + "\n",
    )
    # Any `.xcproj` is a JSON project; a file of another name is not.
    write(
        root,
        "Other/Sub.xcproj",
        '{ "build-settings": { "DEVELOPMENT_TEAM": "ABCDE12345" } }\n',
    )
    write(
        root,
        "Other/settings.json",
        '{ "DEVELOPMENT_TEAM": "ABCDE12345", "PRODUCT_BUNDLE_IDENTIFIER": "com.acme.A" }\n',
    )
    write(
        root,
        "Framework/Info.plist.in",
        "<dict>\n"
        "\t<key>CFBundleIdentifier</key>\n"  # 2
        "\t<string>com.acme.@NAME@</string>\n"  # R3 at line 2
        "\t<key>CFBundleName</key>\n"
        "\t<string>@NAME@</string>\n"
        "</dict>\n",
    )
    write(
        root,
        "Framework/Other.plist",
        "<dict>\n"
        "\t<key>CFBundleIdentifier</key>\n"
        "\t<string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>\n"
        "</dict>\n",
    )
    write(
        root,
        "CMakeLists.txt",
        "set_target_properties(a PROPERTIES\n"
        '    MACOSX_FRAMEWORK_IDENTIFIER "com.acme.${x}")\n'  # R3 at line 2
        "set_target_properties(b PROPERTIES\n"
        '    MACOSX_FRAMEWORK_IDENTIFIER "org.linux-um-darwin.${x}")\n',
    )
    write(
        root,
        "remote.sh",
        "\n".join(
            [
                "scp file alice@buildbox:/tmp/x",  # 1 R4
                "rsync -e ssh alice@buildbox:dir .",  # 2 R4
                "ssh alice@buildbox true",  # 3 R4
                "git clone git@github.com:owner/repo.git",  # 4 pass
                "ssh user@host true",  # 5 pass
                "ssh -o ControlPath=~/.ssh/s %h true",  # 6 R4
                "mail alice@buildbox: the reply",  # 7 pass
                "scp $FILE git@forge.example.com:repo",  # 8 pass
            ]
        )
        + "\n",
    )
    write(
        root,
        "series/topic.patch",
        "\n".join(
            [
                "From 0123456789012345678901234567890123456789 Mon Sep 17 "
                "00:00:00 2001",
                "From: Alice <alice@build.example.com>",
                "Date: Mon, 1 Jan 2026 00:00:00 +0000",
                "Subject: [PATCH] topic",
                "",
                "Built on /Users/someone/src.",  # 6 R1
                "Signed-off-by: Alice <alice@build.example.com>",
                "Claude-Session: https://example.invalid/session/1",  # 8 R6
                "---",
                "diff --git a/f b/f",
                "--- a/f",
                "+++ b/f",
                "@@ -1,3 +1,4 @@",
                " /Users/context/x",  # pass
                "-/Users/removed/x",  # pass
                "+/Users/added/x",  # 16 R1
                "+192.168.0.9",  # 17 R2
                "+ok",
            ]
        )
        + "\n",
    )
    write(
        root,
        "session.txt",
        "\n".join(
            [
                "Claude-Session: https://example.invalid/session/1",  # 1 R6
                "a Claude-Session: mention in running text",  # 2 pass
                " Claude-Session: indented",  # 3 pass
            ]
        )
        + "\n",
    )
    (root / "binary.bin").write_bytes(b"\0/Users/someone/x\n")
    (root / "link.txt").symlink_to("paths.txt")
    write(root, ".gitignore", "ignored.txt\n")
    write(root, "ignored.txt", "/Users/someone/ignored\n")
    write(root, "tokens.txt", "a SecretHost b\nnone here\nmore SECRETHOST\n")
    write(root, "skip/tokens.txt", "SecretHost\n")
    write(root, "rule-lookbehind.txt", "com.keep.nothing keepme nokeepme\n")
    # The checker's own files carry rule patterns, but R5 applies to them.
    write(
        root,
        "tests/tooling/cmake/check_environment_portability.py",
        "/Users/someone/pattern\nSecretHost\n",
    )


EXPECTED = {
    ("paths.txt", 1, "R1"),
    ("paths.txt", 2, "R1"),
    ("paths.txt", 3, "R1"),
    ("paths.txt", 4, "R1"),
    ("paths.txt", 8, "R1"),
    ("addresses.txt", 1, "R2"),
    ("addresses.txt", 2, "R2"),
    ("addresses.txt", 3, "R2"),
    ("addresses.txt", 4, "R2"),
    ("addresses.txt", 5, "R2"),
    ("addresses.txt", 6, "R2"),
    ("App.xcodeproj/project.pbxproj", 1, "R3"),
    ("App.xcodeproj/project.pbxproj", 4, "R3"),
    ("App.xcodeproj/project.pbxproj", 9, "R3"),
    ("Config/Signing.xcconfig", 5, "R3"),
    ("App.xcodeproj/project.xcproj", 5, "R3"),
    ("App.xcodeproj/project.xcproj", 6, "R3"),
    ("App.xcodeproj/project.xcproj", 8, "R3"),
    ("App.xcodeproj/project.xcproj", 14, "R3"),
    ("App.xcodeproj/project.xcproj", 15, "R3"),
    ("App.xcodeproj/project.xcproj", 18, "R3"),
    ("App.xcodeproj/project.xcproj", 30, "R3"),
    ("Compact.xcodeproj/project.xcproj", 3, "R3"),
    ("Compact.xcodeproj/project.xcproj", 4, "R3"),
    ("Compact.xcodeproj/project.xcproj", 5, "R3"),
    ("Compact.xcodeproj/project.xcproj", 6, "R3"),
    ("Compact.xcodeproj/project.xcproj", 8, "R3"),
    ("Compact.xcodeproj/project.xcproj", 12, "R3"),
    ("Other/Sub.xcproj", 1, "R3"),
    ("Framework/Info.plist.in", 2, "R3"),
    ("CMakeLists.txt", 2, "R3"),
    ("remote.sh", 1, "R4"),
    ("remote.sh", 2, "R4"),
    ("remote.sh", 3, "R4"),
    ("remote.sh", 6, "R4"),
    ("session.txt", 1, "R6"),
    ("series/topic.patch", 6, "R1"),
    ("series/topic.patch", 8, "R6"),
    ("series/topic.patch", 16, "R1"),
    ("series/topic.patch", 17, "R2"),
}
DENYLIST_EXPECTED = {
    ("tokens.txt", 1, "R5"),
    ("tokens.txt", 3, "R5"),
    ("tests/tooling/cmake/check_environment_portability.py", 2, "R5"),
    ("rule-lookbehind.txt", 1, "R5"),
}


def init_git(root: Path, home: Path) -> None:
    environment = {
        **os.environ,
        "HOME": str(home),
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_SYSTEM": os.devnull,
    }
    subprocess.run(
        ["git", "init", "-q", str(root)],
        check=True,
        env=environment,
        stdout=subprocess.DEVNULL,
    )
    # Tracked files and unignored new files must both be scanned.
    subprocess.run(
        ["git", "-C", str(root), "add", "paths.txt", "addresses.txt"],
        check=True,
        env=environment,
    )


def run_fixture(checker: Checker, directory: Path) -> None:
    root = directory / "repo"
    root.mkdir()
    make_fixture(root)
    init_git(root, directory / "home")
    empty_allowlist = directory / "empty-allow.tsv"
    empty_allowlist.write_text("# no entries\n", encoding="utf-8")
    denylist = directory / "denylist"
    denylist.write_text(
        "# fixture denylist\n"
        "secrethost\tskip/*,other/*\n"
        "(?<!com\\.)keep\n"
        "\n",
        encoding="utf-8",
    )
    common = ("--source-root", str(root), "--allowlist", str(empty_allowlist))

    generic = checker.run(*common, "--no-denylist")
    require(generic.returncode == 1, f"generic run must fail:\n{generic.stdout}")
    require(
        findings(generic.stdout) == EXPECTED,
        "generic findings differ:\n"
        f"missing {sorted(EXPECTED - findings(generic.stdout))}\n"
        f"extra {sorted(findings(generic.stdout) - EXPECTED)}\n"
        f"{generic.stdout}",
    )
    require(
        "paths.txt:1: R1: /Users/someone/work" in generic.stdout,
        f"excerpt format changed:\n{generic.stdout}",
    )

    named = checker.run(
        *common, "--denylist", str(denylist), "--no-denylist"
    )
    require(
        findings(named.stdout) == EXPECTED,
        "--no-denylist must win over --denylist",
    )

    full = checker.run(*common, environ={"UML_PORTABILITY_DENYLIST": str(denylist)})
    expected_all = EXPECTED | DENYLIST_EXPECTED
    require(
        findings(full.stdout) == expected_all,
        "denylist findings differ:\n"
        f"missing {sorted(expected_all - findings(full.stdout))}\n"
        f"extra {sorted(findings(full.stdout) - expected_all)}\n"
        f"{full.stdout}",
    )
    require(
        ("skip/tokens.txt", 1, "R5") not in findings(full.stdout),
        "a path-scoped exemption must hold",
    )

    # The XDG location is read when nothing names a denylist.
    configuration = directory / "xdg" / "linux-um-darwin"
    configuration.mkdir(parents=True)
    (configuration / "portability-denylist").write_text(
        "secrethost\n", encoding="utf-8"
    )
    discovered = checker.run(*common, environ={"XDG_CONFIG_HOME": str(directory / "xdg")})
    require(
        ("tokens.txt", 1, "R5") in findings(discovered.stdout),
        "the XDG denylist must be discovered",
    )
    home_default = directory / "home"
    (home_default / ".config" / "linux-um-darwin").mkdir(parents=True)
    (home_default / ".config" / "linux-um-darwin" / "portability-denylist").write_text(
        "secrethost\n", encoding="utf-8"
    )
    discovered = checker.run(*common)
    require(
        ("tokens.txt", 1, "R5") in findings(discovered.stdout),
        "the default $HOME/.config denylist must be discovered",
    )

    missing = checker.run(
        *common, environ={"UML_PORTABILITY_DENYLIST": str(directory / "nope")}
    )
    require(
        missing.returncode == 2 and "does not exist" in missing.stdout,
        f"a named denylist that is missing is an error:\n{missing.stdout}",
    )
    broken = directory / "broken"
    broken.write_text("(unclosed\n", encoding="utf-8")
    result = checker.run(*common, environ={"UML_PORTABILITY_DENYLIST": str(broken)})
    require(
        result.returncode == 2 and "bad denylist regex" in result.stdout,
        f"a malformed denylist pattern is an error:\n{result.stdout}",
    )

    # An allowlist entry suppresses exactly its finding and must stay live.
    allowlist = directory / "allow.tsv"
    entries = [
        "R1\tpaths.txt\t/Users/someone/work\tfixture for a parser or validator",
        "R1\tpaths.txt\t/Volumes/Data/linux\tguest-image path",
        "R1\tpaths.txt\t/home/bob\tguest-image path",
        "R1\tpaths.txt\t/private/var\texternal upstream content",
        "R1\tpaths.txt\tsee \\(/Volumes\trecorded provenance of an accepted artifact",
    ]
    allowlist.write_text("\n".join(entries) + "\n", encoding="utf-8")
    suppressed = checker.run(
        "--source-root", str(root), "--allowlist", str(allowlist), "--no-denylist"
    )
    remaining = findings(suppressed.stdout)
    require(
        not {item for item in remaining if item[0] == "paths.txt"},
        f"allowlisted findings must be suppressed:\n{suppressed.stdout}",
    )
    require(
        remaining == {item for item in EXPECTED if item[0] != "paths.txt"},
        "an allowlist entry suppressed other findings",
    )
    allowlist.write_text(
        "\n".join(entries + ["R1\tpaths.txt\tnever present\tguest-image path"])
        + "\n",
        encoding="utf-8",
    )
    stale = checker.run(
        "--source-root", str(root), "--allowlist", str(allowlist), "--no-denylist"
    )
    require(
        "stale allowlist entry" in stale.stdout and stale.returncode == 1,
        f"a stale allowlist entry must fail:\n{stale.stdout}",
    )
    for bad, message in (
        ("R5\tpaths.txt\tx\tguest-image path", "rule must be one of"),
        ("R1\tpaths.txt\tx\ta whim", "reason must be one of"),
        ("R1\tpaths.txt\t(\tguest-image path", "bad regex"),
        ("R1\tpaths.txt\tx", "expected rule, path glob, regex and reason"),
    ):
        allowlist.write_text(bad + "\n", encoding="utf-8")
        result = checker.run(
            "--source-root", str(root), "--allowlist", str(allowlist),
            "--no-denylist",
        )
        require(
            result.returncode == 2 and message in result.stdout,
            f"allowlist line {bad!r} must be rejected with {message!r}:\n"
            f"{result.stdout}",
        )

    # A tree without Git is walked, skipping the usual build directories.
    plain = directory / "plain"
    write(plain, "a.txt", "/Users/someone/x\n")
    write(plain, ".build/b.txt", "/Users/someone/y\n")
    write(plain, "external/c.txt", "/Users/someone/z\n")
    result = checker.run(
        "--source-root", str(plain), "--allowlist", str(empty_allowlist),
        "--no-denylist",
    )
    require(
        findings(result.stdout) == {("a.txt", 1, "R1")},
        f"the walk fallback scanned the wrong files:\n{result.stdout}",
    )

    clean = directory / "clean"
    write(clean, "ok.txt", "nothing site-specific here\n")
    result = checker.run(
        "--source-root", str(clean), "--allowlist", str(empty_allowlist),
        "--no-denylist",
    )
    require(
        result.returncode == 0 and "no findings" in result.stdout,
        f"a clean tree must pass:\n{result.stdout}",
    )

    rules = checker.run("--list-rules")
    require(
        rules.returncode == 0
        and all(rule in rules.stdout for rule in ("R1", "R2", "R3", "R4", "R5")),
        f"--list-rules must describe every rule:\n{rules.stdout}",
    )


def run_tree(checker: Checker, root: Path) -> None:
    for extra in ((), ("--no-denylist",)):
        result = checker.run("--source-root", str(root), *extra)
        label = "generic rules" if extra else "default denylist discovery"
        require(
            result.returncode == 0,
            f"the tree has findings under the {label}:\n{result.stdout}",
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checker", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="uml-portability-") as name:
        directory = Path(name)
        (directory / "home").mkdir()
        checker = Checker(arguments.checker, directory / "home")
        run_fixture(checker, directory)
    # The tree runs use the invoking environment, as a developer's would.
    run_tree(Checker(arguments.checker, None), arguments.source_root)
    print("environment portability checker passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
