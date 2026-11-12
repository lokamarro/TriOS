#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the SPDX licence headers of tracked sources against the licensing rule.

The rule is by location (LICENSES/README.md): GPL-2.0-or-later by default,
Apache-2.0 for code that goes into the Android tree or image, and the patched
project's licence for patches to another project. Every source file or script
carries a header with an identifier allowed at its location, no first-party
header uses a deprecated identifier, and the root LICENSE and LICENSES/ hold a
text for every identifier in use. A reviewed exception lives in the exceptions
TSV (default: beside this script); an entry that matches nothing fails.

A file's own header is an SPDX line in a comment within its first lines. A
string that merely mentions the tag, as a test fixture does, is not a header.
Patch files carry another project's content and are not scanned.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

# The header must start within this many lines (a shebang, an XML declaration
# or a blank line may come first).
HEADER_LINES = 8
READ_BYTES = 4096

EXCLUDED_ROOT_DIRECTORIES = frozenset({".build", ".git", "external", "linux"})

# Carried content: a patch keeps the licence of the project it patches.
EXCLUDED_SUFFIXES = frozenset({".patch"})

# A file of one of these kinds must carry a header wherever it lives.
SOURCE_SUFFIXES = frozenset({
    ".bp", ".c", ".cc", ".cmake", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".inc", ".ipp", ".java", ".ld", ".m", ".mk", ".mm", ".py", ".rc", ".s",
    ".S", ".sh", ".swift", ".te",
})
SOURCE_NAMES = frozenset({"CMakeLists.txt"})

# Where every comment-capable file must carry a header: not data.
DATA_SUFFIXES = frozenset({".conf", ".json", ".md", ".png", ".txt"})


@dataclass(frozen=True)
class Location:
    pattern: re.Pattern[str]
    allowed: frozenset[str]
    reason: str
    every_file: bool = False


def location(pattern: str, allowed: set[str], reason: str,
             every_file: bool = False) -> Location:
    return Location(re.compile(pattern), frozenset(allowed), reason, every_file)


GPL = "GPL-2.0-or-later"

# First match wins.
LOCATIONS = (
    location(
        r"^android/lineage-24/overlay/", {"Apache-2.0"},
        "code that goes into the Android image (overlay, sepolicy, sensors) "
        "is Apache-2.0", every_file=True),
    location(
        r"^tests/integration/android/(motion|storage|webgl)/", {"Apache-2.0"},
        "the Android test and probe apps are built in the Android tree and "
        "are Apache-2.0", every_file=True),
    location(
        r"^android/lineage-24/third_party/[^/]+/soong-[^/]+\.toml$", {"MIT"},
        "the Mesa build files keep Mesa's licence"),
    location(
        r"^android/lineage-24/patches/projects\.tsv$", {"Apache-2.0"},
        "the route table of the patches to AOSP and LineageOS projects; its "
        "SHA-256 is recorded in accepted-release.env and in each release "
        "manifest, so a changed byte re-opens the accepted release"),
    location(
        r"^android/lineage-24/patches/", {"Apache-2.0"},
        "the series manifests of the patches to AOSP and LineageOS projects; "
        "regenerate-aosp-patches.py exports them as Apache-2.0"),
    location(
        r"^android/lineage-24/third_party/", {"Apache-2.0"},
        "the pins and series manifests of the Mesa and meson2hermetic "
        "sources built into the Android tree; pinned_source.py exports the "
        "series as Apache-2.0"),
    location(
        r"^android/lineage-24/(build|tools|metadata|support)/", {GPL},
        "build-host tooling and recorded metadata written in this project: "
        "they run on the build host, link nothing from Android and import "
        "GPL-2.0-or-later modules, so rule 3 applies"),
)
DEFAULT_LOCATION = location(
    r"", {GPL}, "everything else is GPL-2.0-or-later")

# Deprecated SPDX identifiers: the GNU family without -only or -or-later.
DEPRECATED = re.compile(r"^(?:A|L)?GPL-\d\.\d\+?$")

HEADER = re.compile(
    r"^\s*(?://|#|/\*+|\*|<!--|;|--|%)\s*"
    r"SPDX-License-Identifier:[ \t]*(?P<expression>\S.*?)[ \t]*"
    r"(?:\*/|-->)?[ \t]*$")
EXPRESSION = re.compile(
    r"^[A-Za-z0-9.+-]+(?: (?:WITH|OR|AND) [A-Za-z0-9.+-]+)*$")
OPERATORS = frozenset({"WITH", "OR", "AND"})

# What the root LICENSE and each LICENSES/<identifier>.txt must contain, and
# must not: the kernel's metadata header is stripped from the texts.
GPL_MARKERS = ("GNU GENERAL PUBLIC LICENSE", "Version 2, June 1991",
               "END OF TERMS AND CONDITIONS")
TEXT_MARKERS = {
    "Apache-2.0": ("Apache License", "Version 2.0, January 2004",
                   "END OF TERMS AND CONDITIONS"),
    "BSD-3-Clause": ("Redistribution and use in source and binary forms",
                     "Neither the name of"),
    "MIT": ("Permission is hereby granted, free of charge",),
    "Linux-syscall-note": ("user programs",),
}
KERNEL_METADATA = ("Valid-License-Identifier", "SPDX-URL:", "Usage-Guide:",
                   "License-Text:")
GPL_IDENTIFIERS = frozenset({GPL, "GPL-2.0-only", "GPL-2.0", "GPL-2.0+"})


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    rule: str
    message: str


@dataclass
class ExceptionEntry:
    path: str
    expression: str
    reason: str
    number: int
    used: bool = False


def git_files(root: Path) -> list[str] | None:
    if not (root / ".git").exists():
        return None
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
             "--exclude-standard"],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    names = completed.stdout.decode("utf-8", "surrogateescape").split("\0")
    return sorted(set(name for name in names if name))


def walked_files(root: Path) -> list[str]:
    names: list[str] = []
    for directory, subdirectories, files in os.walk(root):
        relative = Path(directory).relative_to(root)
        if relative == Path("."):
            subdirectories[:] = [name for name in subdirectories
                                 if name not in EXCLUDED_ROOT_DIRECTORIES]
        subdirectories[:] = [name for name in subdirectories if name != ".git"]
        names.extend(str(relative / name) for name in files)
    return sorted(names)


def source_files(root: Path) -> list[str]:
    names = git_files(root)
    return walked_files(root) if names is None else names


def load_exceptions(path: Path) -> list[ExceptionEntry]:
    entries: list[ExceptionEntry] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split("\t")
        where = f"{path}:{number}"
        if len(fields) != 3 or not all(field.strip() for field in fields):
            raise ValueError(f"{where}: expected path, expression and reason")
        file_path, expression, reason = fields
        if EXPRESSION.match(expression) is None:
            raise ValueError(f"{where}: malformed expression {expression!r}")
        entries.append(ExceptionEntry(file_path, expression, reason, number))
    return entries


def classify(path: str) -> Location:
    for candidate in LOCATIONS:
        if candidate.pattern.search(path):
            return candidate
    return DEFAULT_LOCATION


def read_head(root: Path, name: str) -> str | None:
    """The first lines of a regular text file, or None for anything else."""
    full = root / name
    try:
        if full.is_symlink() or not full.is_file():
            return None
        with open(full, "rb") as handle:
            head = handle.read(READ_BYTES)
    except OSError:
        return None
    if b"\0" in head:
        return None
    return head.decode("utf-8", "replace").lstrip("﻿")


def needs_header(name: str, head: str, where: Location) -> bool:
    path = Path(name)
    if path.suffix in SOURCE_SUFFIXES or path.name in SOURCE_NAMES:
        return True
    if head.startswith("#!"):
        return True
    return where.every_file and path.suffix not in DATA_SUFFIXES


def find_header(head: str) -> tuple[int, str | None]:
    """Return the line and expression of the file's own header.

    A first line that names the tag in any other form, such as an empty
    identifier, returns its line with no expression. A file with no mention in
    its first lines returns line 0.
    """
    for number, line in enumerate(head.split("\n")[:HEADER_LINES], 1):
        match = HEADER.match(line)
        if match is not None:
            return number, match.group("expression")
        if "SPDX-License-Identifier" in line:
            return number, None
    return 0, None


def identifiers(expression: str) -> list[str]:
    return [token for token in expression.split() if token not in OPERATORS]


def check_file(root: Path, name: str, exceptions: dict[str, ExceptionEntry],
               in_use: set[str]) -> list[Finding]:
    if Path(name).suffix in EXCLUDED_SUFFIXES:
        return []
    head = read_head(root, name)
    if head is None:
        return []
    where = classify(name)
    line, expression = find_header(head)
    if expression is None:
        if line:
            return [Finding(name, line, "H2",
                            "the SPDX identifier is empty or not in a comment "
                            "on its own")]
        if needs_header(name, head, where):
            return [Finding(name, 1, "H1",
                            "no SPDX-License-Identifier header; this location "
                            f"allows {sorted(where.allowed)}")]
        return []
    if EXPRESSION.match(expression) is None:
        return [Finding(name, line, "H2",
                        f"malformed SPDX expression {expression!r}")]
    entry = exceptions.get(name)
    if entry is not None:
        entry.used = True
        if expression != entry.expression:
            return [Finding(name, line, "H5",
                            f"the exception allows {entry.expression!r} "
                            f"({entry.reason}), the header says {expression!r}")]
        in_use.update(identifiers(expression))
        return []
    findings = []
    for identifier in identifiers(expression):
        if DEPRECATED.match(identifier):
            findings.append(Finding(
                name, line, "H3",
                f"deprecated identifier {identifier}; use GPL-2.0-or-later, or "
                "GPL-2.0-only for code derived from GPL-2.0-only code"))
    if not findings and expression not in where.allowed:
        findings.append(Finding(
            name, line, "H2",
            f"{expression!r} is not allowed here ({where.reason}); allowed: "
            f"{sorted(where.allowed)}"))
    if not findings:
        in_use.update(identifiers(expression))
    return findings


def check_text(root: Path, relative: str, markers: tuple[str, ...],
               label: str) -> list[Finding]:
    path = root / relative
    if not path.is_file():
        return [Finding(relative, 1, "H4", f"no licence text for {label}")]
    text = path.read_text(encoding="utf-8", errors="replace")
    findings = []
    for marker in markers:
        if marker not in text:
            findings.append(Finding(relative, 1, "H4",
                                    f"not the {label} text: {marker!r} missing"))
    for marker in KERNEL_METADATA:
        if marker in text:
            findings.append(Finding(
                relative, 1, "H4",
                f"the kernel's metadata header ({marker.rstrip(':')}) must be "
                "stripped from the text"))
    if not text.strip():
        findings.append(Finding(relative, 1, "H4", f"the {label} text is empty"))
    return findings


def check_licenses(root: Path, in_use: set[str]) -> list[Finding]:
    """Every identifier in use has its text; the root LICENSE is GPL-2.0."""
    texts: dict[str, tuple[tuple[str, ...], str]] = {
        "LICENSE": (GPL_MARKERS, "GPL-2.0"),
    }
    for identifier in in_use - GPL_IDENTIFIERS:
        texts[f"LICENSES/{identifier}.txt"] = (
            TEXT_MARKERS.get(identifier, ()), identifier)
    findings: list[Finding] = []
    for relative, (markers, label) in sorted(texts.items()):
        findings.extend(check_text(root, relative, markers, label))
    return findings


def check(
    root: Path, exceptions_path: Path, names: list[str] | None = None
) -> tuple[list[Finding], list[ExceptionEntry], int, set[str]]:
    entries = load_exceptions(exceptions_path)
    exceptions = {entry.path: entry for entry in entries}
    if len(exceptions) != len(entries):
        raise ValueError(f"{exceptions_path}: a path appears twice")
    names = source_files(root) if names is None else names
    in_use: set[str] = set()
    findings: list[Finding] = []
    for name in names:
        findings.extend(check_file(root, name, exceptions, in_use))
    findings.extend(check_licenses(root, in_use))
    findings.sort(key=lambda finding: (finding.path, finding.line, finding.rule))
    stale = [entry for entry in entries if not entry.used]
    return findings, stale, len(names), in_use


def parse_arguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--exceptions", type=Path,
                        help="exceptions TSV (default: beside this script)")
    parser.add_argument("--list-rules", action="store_true")
    return parser.parse_args(argv)


def list_rules() -> None:
    print("H1  a source file or script has no SPDX-License-Identifier header")
    print("H2  the identifier is empty, malformed or not allowed at the "
          "location")
    print("H3  the identifier is deprecated (GPL-2.0, GPL-2.0+, ...)")
    print("H4  the root LICENSE or LICENSES/<identifier>.txt is missing or is "
          "not the text")
    print("H5  an exception does not match its file's header, or matches no "
          "file")
    print()
    print("Locations, first match wins:")
    for candidate in (*LOCATIONS, DEFAULT_LOCATION):
        print(f"  {candidate.pattern.pattern or '(everything else)'}: "
              f"{sorted(candidate.allowed)}")
        print(f"      {candidate.reason}")


def main(argv: list[str] | None = None) -> int:
    options = parse_arguments(argv)
    if options.list_rules:
        list_rules()
        return 0
    if options.source_root is None:
        print("check_license_headers: --source-root is required", file=sys.stderr)
        return 2
    root = options.source_root.resolve()
    exceptions_path = options.exceptions or (
        Path(__file__).resolve().parent / "license-header-exceptions.tsv")
    try:
        findings, stale, scanned, in_use = check(root, exceptions_path)
    except (OSError, ValueError) as error:
        print(f"check_license_headers: {error}", file=sys.stderr)
        return 2
    for finding in findings:
        print(f"{finding.path}:{finding.line}: {finding.rule}: {finding.message}")
    for entry in stale:
        print(f"{exceptions_path}:{entry.number}: H5: the exception for "
              f"{entry.path} matches no file")
    if findings or stale:
        print(f"license headers: {len(findings)} finding(s), {len(stale)} stale "
              f"exception(s)", file=sys.stderr)
        return 1
    print(f"license headers: {scanned} files scanned, no findings "
          f"({', '.join(sorted(in_use))})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
