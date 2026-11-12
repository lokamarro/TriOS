#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject host-, user-, device- and site-specific values in tracked files.

The checker scans the tracked files plus new unignored files, so a leak fails
before it is committed. Rules R1-R4 are generic. Rule R5 reads an optional
local denylist of private tokens, which a site keeps outside the repository.
`--list-rules` prints the rules and the denylist format.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import bisect
import fnmatch
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from concurrent.futures.process import BrokenProcessPool
from pathlib import Path
from typing import Callable, Iterable, NamedTuple

RULES = {
    "R1": "host-path: /Users/<name>, /Volumes/<name>, /home/<name> (not the "
    "guest accounts uml and debian) and /private/var/folders; placeholders "
    "(<x>, $X, {x}, NAME, Shared) pass",
    "R2": "private-address: IPv4 in 10/8 (not SLiRP's 10.0.2.0/24), 172.16/12, "
    "192.168/16, 100.64/10 and 169.254/16; IPv6 fdXX: unique-local; "
    "<label>.local mDNS names",
    "R3": "apple-signing: a literal DEVELOPMENT_TEAM, and a literal bundle "
    "identifier (PRODUCT_BUNDLE_IDENTIFIER, CFBundleIdentifier, "
    "MACOSX_FRAMEWORK_IDENTIFIER, MACOSX_BUNDLE_GUI_IDENTIFIER) outside the "
    "project namespaces; read from *.pbxproj and *.xcconfig settings, from "
    "the string or string-array values of *.xcproj JSON settings (a "
    "[condition] key suffix and JSON5 quoting included), plists and CMake; "
    "$(X), ${X}, @X@ and empty values pass",
    "R4": "ssh-endpoint: user@host remotes of scp, rsync, sftp and ssh "
    "(placeholders and git forges pass), and the ssh options ControlPath, "
    "ControlMaster and ControlPersist",
    "R5": "private-token: a pattern of the local denylist",
    "R6": "agent-session: a Claude-Session: trailer at the start of a line, "
    "which links a private coding session (never allowlisted; patch export "
    "drops it)",
}
ALLOWLIST_RULES = ("R1", "R2", "R3", "R4")
ALLOWLIST_REASONS = (
    "guest-image path",
    "recorded provenance of an accepted artifact",
    "fixture for a parser or validator",
    "external upstream content",
    "project signing identity",
)
# R3: bundle identifiers inside these namespaces are not site-specific.
PROJECT_NAMESPACES = ("org.linux-um-darwin.", "com.example.", "org.example.")
DENYLIST_HELP = """\
Denylist (rule R5): UML_PORTABILITY_DENYLIST names files separated by ':';
otherwise ${XDG_CONFIG_HOME:-$HOME/.config}/linux-um-darwin/portability-denylist
is read when present. Each line is a case-insensitive Python regular
expression, optionally followed by a TAB and comma-separated path globs of
files that are exempt from it. A line starting with # is a comment.
"""

# The checker, its self-test and the allowlist carry the patterns and fixture
# literals of the rules; R5 still applies to them.
SELF_PATHS = frozenset(
    {
        "tests/tooling/cmake/check_environment_portability.py",
        "tests/tooling/cmake/test_environment_portability.py",
        "tests/tooling/cmake/environment-portability-allow.tsv",
    }
)
EXCLUDED_ROOT_DIRECTORIES = frozenset({".build", ".git", "external", "linux"})
BINARY_PROBE_BYTES = 8192
MAXIMUM_WORKERS = 8

# Trailers that carry authentic Git identities in patch messages.
PATCH_TRAILER = re.compile(
    r"(?:From|Date|Signed-off-by|Co-developed-by|Co-Authored-By|Reviewed-by|"
    r"Acked-by|Tested-by|Reported-by|Suggested-by|Cc):",
    re.IGNORECASE,
)
PATCH_MBOX_SEPARATOR = re.compile(r"From [0-9a-f]{40} ")
SESSION_TRAILER = re.compile(r"^Claude-Session:", re.MULTILINE)

PATH_SEGMENT = r"[^\s/\"'`<>()\[\]{},;:|\\*?$%@!&=+]+"
HOST_PATH = re.compile(
    r"/(?:Users|Volumes|home|private/var/folders)/(?P<segment>"
    + PATH_SEGMENT
    + ")?"
)
IPV4 = re.compile(r"\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}")
IPV6_ULA = re.compile(
    r"fd[0-9a-f]{2}(?::[0-9a-f]{0,4}){2,7}(?![0-9A-Fa-f])", re.IGNORECASE
)
MDNS_SUFFIX = re.compile(r"\.local\b(?![\w.(\[-])(?![ \t]*[=(\[])")
SSH_OPTION = re.compile(r"\bControl(?:Path|Master|Persist)\b")
REMOTE_COMMAND = re.compile(r"\b(?:scp|rsync|sftp|ssh)\b")
REMOTE_SPEC = re.compile(
    r"(?<![\w.%+-])(?P<user>[\w.-]+)@(?P<host>[\w.-]+)(?::[~/\w.-]|\b)"
)
REMOTE_PLACEHOLDERS = frozenset(
    {"user", "host", "hostname", "username", "remote", "server", "example"}
)
REMOTE_FORGES = frozenset({"git", "hg", "svn"})

SIGNING_FILE = re.compile(r"\.(?:pbxproj|xcconfig)$")
JSON_PROJECT_FILE = re.compile(r"\.xcproj$")
PLIST_FILE = re.compile(r"\.(?:plist|plist\.in|entitlements)$")
CMAKE_FILE = re.compile(r"(?:^|/)CMakeLists\.txt$|\.cmake$")
DEVELOPMENT_TEAM = re.compile(r"\bDEVELOPMENT_TEAM[ \t]*=[ \t]*(?P<value>[^;\n]*)")
BUNDLE_SETTING = re.compile(
    r"\bPRODUCT_BUNDLE_IDENTIFIER\s*=\s*(?P<value>\"[^\"\n]*\"|[^;\n]*)"
)
PLIST_IDENTIFIER = re.compile(
    r"<key>CFBundleIdentifier</key>\s*<string>(?P<value>[^<]*)</string>"
)
CMAKE_IDENTIFIER = re.compile(
    r"\b(?:MACOSX_FRAMEWORK_IDENTIFIER|MACOSX_BUNDLE_GUI_IDENTIFIER)\s+"
    r"(?P<value>\"[^\"\n]*\"|[^\s)]+)"
)
PLACEHOLDER = re.compile(r"@\w+@|\$\{[^}]*\}|\$\([^)]*\)")

# A JSON project (`project.xcproj`) is JSON5: keys may be unquoted or single
# quoted, and a build-setting key may carry a `[condition]` suffix inside its
# quotes. A setting's value is one string or an array of strings.
JSON_STRING = r"\"(?:[^\"\\\n]|\\.)*\"|'(?:[^'\\\n]|\\.)*'"
JSON_COMMENT = r"//[^\n]*(?=\n|\Z)|/\*(?:[^*]|\*(?!/))*\*/"
JSON_ARRAY = rf"\[(?:\s|,|{JSON_COMMENT}|{JSON_STRING})*\]"
JSON_LITERAL = re.compile(rf"{JSON_COMMENT}|(?P<string>{JSON_STRING})")


def json_setting(name: str) -> re.Pattern[str]:
    return re.compile(
        rf"(?P<quote>[\"']?)\b{name}(?:\[[^\]\"'\n]*\])?(?P=quote)\s*:\s*"
        rf"(?P<value>{JSON_ARRAY}|{JSON_STRING})"
    )


JSON_TEAM = json_setting("DEVELOPMENT_TEAM")
JSON_BUNDLE = json_setting("PRODUCT_BUNDLE_IDENTIFIER")


class Finding(NamedTuple):
    rule: str
    path: str
    line: int
    excerpt: str
    text: str


class AllowEntry(NamedTuple):
    rule: str
    glob: str
    pattern: re.Pattern[str]
    reason: str
    number: int


class DenyEntry(NamedTuple):
    pattern: re.Pattern[str]
    exempt: tuple[str, ...]
    source: str


def git_files(root: Path) -> list[str] | None:
    try:
        completed = subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "ls-files",
                "-z",
                "--cached",
                "--others",
                "--exclude-standard",
            ],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    names = completed.stdout.decode("utf-8", "surrogateescape").split("\0")
    return sorted(set(name for name in names if name))


def walked_files(root: Path) -> list[str]:
    names: list[str] = []
    for directory, subdirectories, files in os.walk(root):
        relative = Path(directory).relative_to(root)
        if relative == Path("."):
            subdirectories[:] = [
                name
                for name in subdirectories
                if name not in EXCLUDED_ROOT_DIRECTORIES
            ]
        subdirectories[:] = [name for name in subdirectories if name != ".git"]
        names.extend(str(relative / name) for name in files)
    return sorted(names)


def source_files(root: Path) -> list[str]:
    names = git_files(root)
    return walked_files(root) if names is None else names


def load_allowlist(path: Path) -> list[AllowEntry]:
    entries: list[AllowEntry] = []
    for number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), 1
    ):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split("\t")
        where = f"{path}:{number}"
        if len(fields) != 4 or not all(field.strip() for field in fields):
            raise ValueError(
                f"{where}: expected rule, path glob, regex and reason"
            )
        rule, glob, pattern, reason = fields
        if rule not in ALLOWLIST_RULES:
            raise ValueError(f"{where}: rule must be one of {ALLOWLIST_RULES}")
        if reason not in ALLOWLIST_REASONS:
            raise ValueError(
                f"{where}: reason must be one of {list(ALLOWLIST_REASONS)}"
            )
        try:
            compiled = re.compile(pattern)
        except re.error as error:
            raise ValueError(f"{where}: bad regex: {error}") from error
        entries.append(AllowEntry(rule, glob, compiled, reason, number))
    return entries


def denylist_sources(
    options: argparse.Namespace, environ: dict[str, str]
) -> tuple[list[Path], bool]:
    """Return the denylist files and whether each must exist."""
    if options.no_denylist:
        return [], False
    if options.denylist:
        return options.denylist, True
    named = environ.get("UML_PORTABILITY_DENYLIST")
    if named is not None:
        return [Path(name) for name in named.split(":") if name], True
    base = environ.get("XDG_CONFIG_HOME", "")
    if not base.startswith("/") and environ.get("HOME"):
        base = str(Path(environ["HOME"]) / ".config")
    if not base.startswith("/"):
        return [], False
    default = Path(base) / "linux-um-darwin" / "portability-denylist"
    return [default], False


def load_denylist(paths: Iterable[Path], explicit: bool) -> list[DenyEntry]:
    entries: list[DenyEntry] = []
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8")
        except FileNotFoundError:
            if explicit:
                raise ValueError(f"denylist does not exist: {path}") from None
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if not line.strip() or line.startswith("#"):
                continue
            pattern, _, globs = line.partition("\t")
            try:
                compiled = re.compile(pattern, re.IGNORECASE)
            except re.error as error:
                raise ValueError(
                    f"{path}:{number}: bad denylist regex: {error}"
                ) from error
            exempt = tuple(
                glob.strip() for glob in globs.split(",") if glob.strip()
            )
            entries.append(DenyEntry(compiled, exempt, f"{path}:{number}"))
    return entries


def is_placeholder_segment(segment: str) -> bool:
    return (
        segment.isupper()
        or segment == "Shared"
        or not segment.strip(".")
    )


def excerpt_of(line: str) -> str:
    text = " ".join(line.split())
    return text if len(text) <= 120 else text[:117] + "..."


class FileScan:
    """Scan one decoded file; findings carry their line text."""

    def __init__(self, path: str, text: str) -> None:
        self.path = path
        self.text = text
        self.findings: list[Finding] = []
        self._starts: list[int] | None = None

    def line_of(self, offset: int) -> tuple[int, str]:
        if self._starts is None:
            self._starts = [0] + [
                match.end() for match in re.finditer("\n", self.text)
            ]
        index = bisect.bisect_right(self._starts, offset) - 1
        start = self._starts[index]
        end = self.text.find("\n", start)
        end = len(self.text) if end < 0 else end
        return index + 1, self.text[start:end]

    def add(self, rule: str, offset: int) -> None:
        number, line = self.line_of(offset)
        self.findings.append(
            Finding(rule, self.path, number, excerpt_of(line), line)
        )


def scan_host_paths(scan: FileScan) -> None:
    text = scan.text
    for match in HOST_PATH.finditer(text):
        start = match.start()
        if start and re.match(r"[\w.~-]", text[start - 1]):
            continue
        prefix = match.group(0)
        segment = match.group("segment")
        if prefix.startswith("/private/var/folders"):
            scan.add("R1", start)
            continue
        if segment is None or is_placeholder_segment(segment):
            continue
        if prefix.startswith("/home/") and segment in ("uml", "debian"):
            continue
        scan.add("R1", start)


def private_ipv4(octets: tuple[int, ...]) -> bool:
    first, second, third = octets[0], octets[1], octets[2]
    if first == 10:
        return not (second == 0 and third == 2)
    return (
        (first == 172 and 16 <= second <= 31)
        or (first == 192 and second == 168)
        or (first == 100 and 64 <= second <= 127)
        or (first == 169 and second == 254)
    )


def scan_addresses(scan: FileScan) -> None:
    text = scan.text
    for match in IPV4.finditer(text):
        start, end = match.span()
        if start and re.match(r"[\w.]", text[start - 1]):
            continue
        if end < len(text) and re.match(r"\w|\.\d", text[end : end + 2]):
            continue
        octets = tuple(int(part) for part in match.group(0).split("."))
        if max(octets) <= 255 and private_ipv4(octets):
            scan.add("R2", start)
    for match in IPV6_ULA.finditer(text):
        start = match.start()
        if start and re.match(r"[0-9A-Fa-f:]", text[start - 1]):
            continue
        scan.add("R2", start)
    for match in MDNS_SUFFIX.finditer(text):
        start = match.start()
        if start and re.match(r"[A-Za-z0-9]", text[start - 1]):
            scan.add("R2", start)


def scan_endpoints(scan: FileScan) -> None:
    text = scan.text
    for match in SSH_OPTION.finditer(text):
        scan.add("R4", match.start())
    position = 0
    while True:
        at = text.find("@", position)
        if at < 0:
            break
        position = at + 1
        line_start = text.rfind("\n", 0, at) + 1
        line_end = text.find("\n", at)
        line_end = len(text) if line_end < 0 else line_end
        line = text[line_start:line_end]
        if not REMOTE_COMMAND.search(line):
            position = max(position, line_end)
            continue
        for found in REMOTE_SPEC.finditer(line):
            user, host = found.group("user"), found.group("host")
            if (
                user in REMOTE_FORGES
                or user.lower() in REMOTE_PLACEHOLDERS
                or host.lower() in REMOTE_PLACEHOLDERS
                or host.lower().startswith("example.")
                or host.lower().endswith(".example.com")
            ):
                continue
            scan.add("R4", line_start + found.start())
        position = max(position, line_end)


def accepted_bundle_identifier(value: str) -> bool:
    """A bundle identifier is site-neutral when it starts with a placeholder
    or inside a project namespace."""
    literal = PLACEHOLDER.split(value, maxsplit=1)[0].strip()
    return not literal or any(
        literal.startswith(prefix) for prefix in PROJECT_NAMESPACES
    )


def accepted_team(value: str) -> bool:
    """A team is site-neutral when it is empty or starts with a placeholder."""
    return not PLACEHOLDER.split(value, maxsplit=1)[0].strip()


def scan_json_setting(
    scan: FileScan,
    pattern: re.Pattern[str],
    accepted: Callable[[str], bool],
) -> None:
    """Report each literal string of a JSON setting, at the literal's line."""
    for match in pattern.finditer(scan.text):
        base = match.start("value")
        for literal in JSON_LITERAL.finditer(match.group("value")):
            text = literal.group("string")
            if text is not None and not accepted(text[1:-1].strip()):
                scan.add("R3", base + literal.start())


def scan_signing(scan: FileScan) -> None:
    path = scan.path
    text = scan.text
    if SIGNING_FILE.search(path):
        for match in DEVELOPMENT_TEAM.finditer(text):
            value = match.group("value").strip().strip('"')
            if not accepted_team(value):
                scan.add("R3", match.start())
        for match in BUNDLE_SETTING.finditer(text):
            value = match.group("value").strip().strip('"')
            if not accepted_bundle_identifier(value):
                scan.add("R3", match.start())
    if JSON_PROJECT_FILE.search(path):
        scan_json_setting(scan, JSON_TEAM, accepted_team)
        scan_json_setting(scan, JSON_BUNDLE, accepted_bundle_identifier)
    if PLIST_FILE.search(path):
        for match in PLIST_IDENTIFIER.finditer(text):
            value = match.group("value").strip()
            if not accepted_bundle_identifier(value):
                scan.add("R3", match.start())
    if CMAKE_FILE.search(path):
        for match in CMAKE_IDENTIFIER.finditer(text):
            value = match.group("value").strip().strip('"')
            if not accepted_bundle_identifier(value):
                scan.add("R3", match.start())


def patch_lines(text: str) -> Iterable[tuple[int, str]]:
    """Yield the message and added lines of a patch, skipping identity trailers."""
    in_diff = False
    for number, line in enumerate(text.split("\n"), 1):
        if PATCH_MBOX_SEPARATOR.match(line):
            in_diff = False
            continue
        if line.startswith("diff --git "):
            in_diff = True
            continue
        if in_diff:
            if line.startswith("+") and not line.startswith("+++"):
                yield number, line[1:]
        elif not PATCH_TRAILER.match(line):
            yield number, line


def scan_sessions(scan: FileScan) -> None:
    for match in SESSION_TRAILER.finditer(scan.text):
        scan.add("R6", match.start())


def scan_rules(scan: FileScan) -> None:
    scan_sessions(scan)
    scan_host_paths(scan)
    scan_addresses(scan)
    scan_endpoints(scan)
    scan_signing(scan)


def scan_denylist(scan: FileScan, denylist: list[DenyEntry]) -> None:
    for entry in denylist:
        if any(fnmatch.fnmatchcase(scan.path, glob) for glob in entry.exempt):
            continue
        for match in entry.pattern.finditer(scan.text):
            scan.add("R5", match.start())


def scan_file(
    root: Path, path: str, denylist: list[DenyEntry]
) -> list[Finding]:
    location = root / path
    try:
        if location.is_symlink() or not location.is_file():
            return []
        data = location.read_bytes()
    except OSError:
        return []
    if b"\0" in data[:BINARY_PROBE_BYTES]:
        return []
    text = data.decode("utf-8", "surrogateescape")
    numbers: list[int] | None = None
    if path.endswith(".patch"):
        kept = list(patch_lines(text))
        numbers = [number for number, _ in kept]
        text = "\n".join(line for _, line in kept)
    scan = FileScan(path, text)
    if path not in SELF_PATHS:
        scan_rules(scan)
    scan_denylist(scan, denylist)
    if numbers is None:
        return scan.findings
    return [
        finding._replace(line=numbers[finding.line - 1])
        for finding in scan.findings
    ]


def apply_allowlist(
    findings: list[Finding], allowlist: list[AllowEntry]
) -> tuple[list[Finding], set[int]]:
    used: set[int] = set()
    remaining: list[Finding] = []
    for finding in findings:
        suppressed = False
        for entry in allowlist:
            if (
                entry.rule == finding.rule
                and fnmatch.fnmatchcase(finding.path, entry.glob)
                and entry.pattern.search(finding.text)
            ):
                used.add(entry.number)
                suppressed = True
        if not suppressed:
            remaining.append(finding)
    return remaining, used


_WORKER: tuple[Path, list[DenyEntry]] | None = None


def _initialize_worker(root: Path, denylist: list[DenyEntry]) -> None:
    global _WORKER
    _WORKER = (root, denylist)


def _scan_in_worker(path: str) -> list[Finding]:
    assert _WORKER is not None
    return scan_file(_WORKER[0], path, _WORKER[1])


def scan_all(
    root: Path, names: list[str], denylist: list[DenyEntry]
) -> list[Finding]:
    """Scan the files, in parallel when the host has cores to spare."""

    def size(name: str) -> int:
        try:
            return (root / name).stat().st_size
        except OSError:
            return 0

    ordered = sorted(names, key=size, reverse=True)
    workers = min(MAXIMUM_WORKERS, os.cpu_count() or 1)
    if workers > 1 and len(ordered) > 64:
        try:
            with ProcessPoolExecutor(
                max_workers=workers,
                initializer=_initialize_worker,
                initargs=(root, denylist),
            ) as pool:
                results = list(pool.map(_scan_in_worker, ordered, chunksize=16))
            return [finding for batch in results for finding in batch]
        except (OSError, BrokenProcessPool):
            pass
    return [
        finding for name in ordered for finding in scan_file(root, name, denylist)
    ]


def check(
    root: Path,
    allowlist_path: Path,
    denylist: list[DenyEntry],
    paths: list[str] | None = None,
) -> tuple[list[Finding], list[AllowEntry], int]:
    allowlist = load_allowlist(allowlist_path)
    names = source_files(root) if paths is None else paths
    findings = scan_all(root, names, denylist)
    findings.sort(key=lambda finding: (finding.path, finding.line, finding.rule))
    remaining, used = apply_allowlist(findings, allowlist)
    stale = [entry for entry in allowlist if entry.number not in used]
    return remaining, stale, len(names)


def parse_arguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source-root", type=Path)
    parser.add_argument(
        "--allowlist",
        type=Path,
        help="allowlist TSV (default: beside this script)",
    )
    parser.add_argument(
        "--denylist",
        action="append",
        type=Path,
        help="denylist file, repeatable (default: see --list-rules)",
    )
    parser.add_argument(
        "--no-denylist",
        action="store_true",
        help="apply the generic rules only",
    )
    parser.add_argument("--list-rules", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    options = parse_arguments(argv)
    if options.list_rules:
        for rule, summary in RULES.items():
            print(f"{rule}  {summary}")
        print()
        print(DENYLIST_HELP, end="")
        return 0
    if options.source_root is None:
        print("check_environment_portability: --source-root is required",
              file=sys.stderr)
        return 2
    root = options.source_root.resolve()
    allowlist_path = options.allowlist or (
        Path(__file__).resolve().parent / "environment-portability-allow.tsv"
    )
    try:
        paths, required = denylist_sources(options, dict(os.environ))
        denylist = load_denylist(paths, required)
        findings, stale, scanned = check(root, allowlist_path, denylist)
    except (OSError, ValueError) as error:
        print(f"check_environment_portability: {error}", file=sys.stderr)
        return 2
    shown: set[tuple[str, str, int]] = set()
    for finding in findings:
        key = (finding.rule, finding.path, finding.line)
        if key not in shown:
            shown.add(key)
            print(f"{finding.path}:{finding.line}: {finding.rule}: "
                  f"{finding.excerpt}")
    for entry in stale:
        print(
            f"{allowlist_path}:{entry.number}: stale allowlist entry "
            f"({entry.rule} {entry.glob}) matches nothing"
        )
    if findings or stale:
        print(
            f"environment portability: {len(findings)} finding(s), "
            f"{len(stale)} stale allowlist entr{'y' if len(stale) == 1 else 'ies'}",
            file=sys.stderr,
        )
        return 1
    print(
        f"environment portability: {scanned} files scanned, no findings "
        f"({len(denylist)} denylist patterns)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
