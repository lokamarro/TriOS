#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate and check the acknowledgements Andarium shows in Info.

The application's Acknowledgements list is the resource
`Andarium/Andarium/Shared/Acknowledgements.json`. This script is its generator:
the table below names each project the application carries, its copyright
holders and its license, and takes every license text from a verified source
checkout (`dependencies/prepare.sh`) or the kernel source, never from the
network. Do not edit the resource by hand.

    acknowledgements.py             rewrite the resource from the sources
    acknowledgements.py --verify    fail if the resource differs from them
    acknowledgements.py --coverage  fail if the application carries a
                                    framework or prefix library the resource
                                    does not acknowledge (needs no sources)

Coverage is measured against what the application embeds: the frameworks in
Andarium/Andarium.xcodeproj and the components of the iOS device prefixes'
contracts under dependencies/platforms. A project lists the frameworks and
components it covers; a project that covers none (the system) lists none.

Line breaks inside a paragraph of a license text are reflowed so the text fits
a phone. The words are unchanged, which the generator checks.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RESOURCE = ROOT / "Andarium/Andarium/Shared/Acknowledgements.json"
PROJECT_FILE = ROOT / "Andarium/Andarium.xcodeproj/project.xcproj"
PLATFORMS = ROOT / "dependencies/platforms"
DEVICE_CONTRACTS = (
    "ios-device-gpu-vulkan-arm64.tsv",
    "ios-device-network-arm64.tsv",
    "ios-device-storage-arm64.tsv",
    "ios-device-compression-arm64.tsv",
)
SCHEMA = 1

GROUPS = [
    {"id": "system", "title": "Operating system"},
    {"id": "graphics", "title": "Graphics"},
    {"id": "libraries", "title": "Libraries"},
]

# Where each license text comes from: (source, path). A source is a directory
# under --source-root named as in dependencies/manifest.tsv, or "linux" for the
# kernel source.
SOURCES = {
    "vulkan-headers": "vulkan-headers",
    "mesa": "mesa",
    "libepoxy": "libepoxy",
    "virglrenderer": "virglrenderer",
    "glib-ios": "glib-ios",
    "gptfdisk": "gptfdisk",
    "libffi": "libffi",
    "libslirp": "libslirp",
    "pcre2": "pcre2",
    "proxy-libintl": "proxy-libintl",
    "zstd": "zstd",
}

MESA_HOLDERS = (
    "Intel Corporation, Google LLC, LunarG, Inc., Collabora Ltd., Valve Corporation, "
    "Red Hat, Inc., VMware, Inc., Advanced Micro Devices, Inc. and other Mesa contributors"
)


class Problem(Exception):
    pass


# --- text sources -----------------------------------------------------------


class Texts:
    """Reads license files from the verified source checkouts."""

    def __init__(self, source_root: Path, kernel_source: Path | None) -> None:
        self.source_root = source_root
        self.kernel_source = kernel_source

    def path(self, source: str, relative: str) -> Path:
        if source == "linux":
            if self.kernel_source is None:
                raise Problem("the kernel source is not configured (--kernel-source)")
            base = self.kernel_source
        else:
            base = self.source_root / SOURCES.get(source, source)
        path = base / relative
        if not path.is_file():
            raise Problem(f"{path} is missing; run dependencies/prepare.sh or pass --source-root")
        return path

    def read(
        self,
        source: str,
        relative: str,
        *,
        after: str | None = None,
        drop: str | None = None,
        uncomment: bool = False,
    ) -> str:
        text = self.path(source, relative).read_text(encoding="utf-8", errors="strict")
        text = text.replace("\r\n", "\n").replace("\r", "\n").expandtabs(8)
        if after is not None:
            _, found, rest = text.partition(after)
            if not found:
                raise Problem(f"{source}/{relative} has no {after!r} marker")
            text = rest
        if drop is not None:
            text = "\n".join(line for line in text.split("\n") if not re.match(drop, line))
        if uncomment:
            # A license quoted as a C comment: drop the comment delimiters.
            kept = [line for line in text.split("\n") if not re.fullmatch(r"\s*(/\*|\*/)\s*", line)]
            text = "\n".join(re.sub(r"^\s*\* ?", "", line) for line in kept)
        return text


LINE_WRAP_MINIMUM = 52


def reflow(text: str) -> str:
    """Join the hard-wrapped lines of each paragraph (those whose median line is
    long); keep short-line blocks as they are, and
    drop the indentation that only centred or nested them in the original."""
    lines = [line.rstrip() for line in text.split("\n")]
    paragraphs: list[list[str]] = []
    current: list[str] = []
    for line in lines:
        if line.strip():
            current.append(line)
        elif current:
            paragraphs.append(current)
            current = []
    if current:
        paragraphs.append(current)
    out: list[str] = []
    for paragraph in paragraphs:
        indents = [len(line) - len(line.lstrip()) for line in paragraph]
        stripped = [line.strip() for line in paragraph]
        lengths = sorted(len(line) for line in stripped)
        wrapped = len(paragraph) > 1 and lengths[len(lengths) // 2] >= LINE_WRAP_MINIMUM
        if wrapped and max(indents) - min(indents) <= 8:
            lead = "  " if indents[0] >= 6 else ""
            out.append(lead + " ".join(stripped))
        else:
            out.append("\n".join(stripped))
    return "\n\n".join(out)


def clean(text: str, original: str | None = None) -> str:
    """Reflow a license text and check that no word changed."""
    result = reflow(text)
    if (original if original is not None else text).split() != result.split():
        raise Problem("reflowing changed the words of a license text")
    return result


def mit_template(texts: Texts, holders: str) -> str:
    """The MIT license with its copyright line filled in."""
    body = texts.read("mesa", "licenses/MIT", after="License-Text:")
    placeholder = "Copyright (c) <year> <copyright holders>"
    if placeholder not in body:
        raise Problem("mesa/licenses/MIT has no copyright placeholder")
    filled = body.replace(placeholder, f"Copyright (c) {holders}")
    return reflow(filled)


# --- the table --------------------------------------------------------------


def shared_licenses(texts: Texts) -> dict[str, dict[str, str]]:
    def license(title: str, source: str, relative: str, *, after: str | None = None) -> dict[str, str]:
        return {"title": title, "text": clean(texts.read(source, relative, after=after))}

    return {
        "apache-2.0": license(
            "Apache License, Version 2.0", "vulkan-headers", "LICENSES/Apache-2.0.txt"
        ),
        "gpl-2.0": license(
            "GNU General Public License, Version 2",
            "linux",
            "LICENSES/preferred/GPL-2.0",
            after="License-Text:",
        ),
        "linux-syscall-note": license(
            "Linux Kernel Syscall Exception",
            "linux",
            "LICENSES/exceptions/Linux-syscall-note",
            after="License-Text:",
        ),
        "lgpl-2.1": license(
            "GNU Lesser General Public License, Version 2.1",
            "glib-ios",
            "LICENSES/LGPL-2.1-or-later.txt",
        ),
    }


def projects(texts: Texts) -> list[dict]:
    def own(title: str | None, source: str, relative: str, **options) -> dict[str, str]:
        text = clean(texts.read(source, relative, **options))
        return {"title": title, "text": text} if title else {"text": text}

    def entry(**fields) -> dict:
        base = {"frameworks": [], "components": [], "licenses": [], "texts": []}
        base.update(fields)
        return base

    return [
        entry(
            id="android",
            group="system",
            name="Android Open Source Project",
            copyright="The Android Open Source Project",
            license="Apache License 2.0",
            spdx="Apache-2.0",
            licenses=["apache-2.0"],
        ),
        entry(
            id="lineageos",
            group="system",
            name="LineageOS",
            copyright="The LineageOS Project",
            license="Apache License 2.0",
            spdx="Apache-2.0",
            licenses=["apache-2.0"],
        ),
        entry(
            id="linux",
            group="system",
            name="Linux",
            copyright="Linus Torvalds and the Linux kernel contributors",
            license="GPL v2 with Linux syscall exception",
            spdx="GPL-2.0-only WITH Linux-syscall-note",
            frameworks=["KRuntime_Android_6_18_4K"],
            licenses=["linux-syscall-note", "gpl-2.0"],
        ),
        entry(
            id="mesa",
            group="graphics",
            name="Mesa",
            copyright=MESA_HOLDERS,
            license="MIT License",
            spdx="MIT",
            frameworks=["MesaEGL", "MesaGallium", "KosmicKrisp"],
            components=["mesa"],
            texts=[{"text": mit_template(texts, MESA_HOLDERS)}],
        ),
        entry(
            id="virglrenderer",
            group="graphics",
            name="virglrenderer",
            copyright="Red Hat, Inc., Google LLC, VMware, Inc., Collabora Ltd., Intel Corporation and other virglrenderer contributors",
            license="MIT License",
            spdx="MIT",
            components=["virglrenderer"],
            texts=[own(None, "virglrenderer", "COPYING")],
        ),
        entry(
            id="libepoxy",
            group="graphics",
            name="libepoxy",
            copyright="2013-2014 Intel Corporation; 2013 The Khronos Group Inc.",
            license="MIT License",
            spdx="MIT",
            components=["libepoxy"],
            texts=[own(None, "libepoxy", "COPYING", uncomment=True)],
        ),
        entry(
            id="vulkan-headers",
            group="graphics",
            name="Vulkan Headers",
            copyright="2015-2026 The Khronos Group Inc.",
            license="Apache License 2.0 or MIT License",
            spdx="Apache-2.0 OR MIT",
            components=["vulkan-headers"],
            licenses=["apache-2.0"],
            texts=[
                {
                    "title": "MIT License",
                    "text": clean(texts.read("vulkan-headers", "LICENSES/MIT.txt")),
                }
            ],
        ),
        entry(
            id="venus-protocol",
            group="graphics",
            name="Venus Protocol",
            copyright="Google LLC",
            license="MIT License",
            spdx="MIT",
            components=["venus-protocol"],
            texts=[{"text": mit_template(texts, "2020 Google LLC")}],
        ),
        entry(
            id="glib",
            group="libraries",
            name="GLib",
            copyright="Peter Mattis, Spencer Kimball, Josh MacDonald, Red Hat, Inc. and other GLib contributors",
            license="GNU LGPL v2.1 or later",
            spdx="LGPL-2.1-or-later",
            components=["glib-ios"],
            licenses=["lgpl-2.1"],
        ),
        entry(
            id="gvdb",
            group="libraries",
            name="GVDB",
            copyright="2010 Codethink Limited",
            license="GNU LGPL v2.1 or later",
            spdx="LGPL-2.1-or-later",
            components=["gvdb"],
            licenses=["lgpl-2.1"],
        ),
        entry(
            id="pcre2",
            group="libraries",
            name="PCRE2",
            copyright="1997-2007 University of Cambridge; 2007-2024 Philip Hazel; 2009-2024 Zoltan Herczeg",
            license="BSD 3-Clause License with PCRE2 exception",
            spdx="BSD-3-Clause WITH PCRE2-exception",
            components=["pcre2"],
            texts=[own(None, "pcre2", "LICENCE.md", drop=r"^\|")],
        ),
        entry(
            id="libffi",
            group="libraries",
            name="libffi",
            copyright="1996-2026 Anthony Green, Red Hat, Inc. and others",
            license="MIT License",
            spdx="MIT",
            components=["libffi"],
            texts=[own(None, "libffi", "LICENSE")],
        ),
        entry(
            id="proxy-libintl",
            group="libraries",
            name="proxy-libintl",
            copyright="2008 Tor Lillqvist",
            license="GNU LGPL v2 or later",
            spdx="LGPL-2.0-or-later",
            components=["proxy-libintl"],
            texts=[own("GNU Library General Public License, Version 2", "proxy-libintl", "COPYING")],
        ),
        entry(
            id="libslirp",
            group="libraries",
            name="libslirp",
            copyright="1995, 1996 Danny Gasparovski and other contributors",
            license="BSD 3-Clause License",
            spdx="BSD-3-Clause",
            components=["libslirp"],
            texts=[
                own(None, "libslirp", "LICENSE"),
                own("Other copyright holders", "libslirp", "COPYRIGHT"),
            ],
        ),
        entry(
            id="gptfdisk",
            group="libraries",
            name="GPT fdisk",
            copyright="2009-2024 Roderick W. Smith",
            license="GNU GPL v2",
            spdx="GPL-2.0-only",
            components=["gptfdisk-core"],
            texts=[own("GNU General Public License, Version 2", "gptfdisk", "COPYING")],
        ),
        entry(
            id="zstd",
            group="libraries",
            name="Zstandard",
            copyright="Meta Platforms, Inc. and affiliates",
            license="BSD 3-Clause License",
            spdx="BSD-3-Clause",
            frameworks=["LibZstd"],
            components=["zstd-decompression"],
            texts=[own(None, "zstd", "LICENSE")],
        ),
    ]


def build(texts: Texts) -> dict:
    shared = shared_licenses(texts)
    table = projects(texts)
    used = {name for project in table for name in project["licenses"]}
    if used != set(shared):
        raise Problem(f"shared licenses {sorted(set(shared) ^ used)} are unused or missing")
    return {"schema": SCHEMA, "groups": GROUPS, "licenses": shared, "projects": table}


def render(document: dict) -> str:
    return json.dumps(document, indent=1, ensure_ascii=False) + "\n"


# --- coverage ---------------------------------------------------------------


def embedded_frameworks(project_file: Path = PROJECT_FILE) -> set[str]:
    """The xcframeworks the Andarium project embeds or links."""
    found = set(re.findall(r"/([A-Za-z0-9_]+)\.xcframework", project_file.read_text(encoding="utf-8")))
    return found


def device_components(platforms: Path = PLATFORMS) -> set[str]:
    """The components the iOS device prefixes' contracts declare."""
    components: set[str] = set()
    for name in DEVICE_CONTRACTS:
        for line in (platforms / name).read_text(encoding="utf-8").splitlines():
            if line.startswith("components\t"):
                components.update(part for part in line.split("\t", 1)[1].split(",") if part)
    return components


def check_document(document: dict, frameworks: set[str], components: set[str]) -> list[str]:
    """Return what is wrong with `document` as the acknowledgements of the app."""
    problems: list[str] = []
    if document.get("schema") != SCHEMA:
        problems.append(f"schema is {document.get('schema')!r}, expected {SCHEMA}")
    groups = {group["id"] for group in document.get("groups", [])}
    licenses = document.get("licenses", {})
    seen: set[str] = set()
    covered_frameworks: set[str] = set()
    covered_components: set[str] = set()
    referenced: set[str] = set()
    for project in document.get("projects", []):
        name = project.get("id", "?")
        if name in seen:
            problems.append(f"{name}: duplicate id")
        seen.add(name)
        for field in ("name", "copyright", "license", "spdx", "group"):
            if not str(project.get(field) or "").strip():
                problems.append(f"{name}: no {field}")
        if project.get("group") not in groups:
            problems.append(f"{name}: unknown group {project.get('group')!r}")
        body = [t for t in project.get("texts", []) if str(t.get("text", "")).strip()]
        refs = project.get("licenses", [])
        if not body and not refs:
            problems.append(f"{name}: no license text")
        for ref in refs:
            referenced.add(ref)
            if not str(licenses.get(ref, {}).get("text", "")).strip():
                problems.append(f"{name}: license text {ref!r} is missing")
        covered_frameworks.update(project.get("frameworks", []))
        covered_components.update(project.get("components", []))
    for ref in sorted(set(licenses) - referenced):
        problems.append(f"license text {ref!r} is not used by any project")
    for framework in sorted(frameworks - covered_frameworks):
        problems.append(f"embedded framework {framework} has no acknowledgement")
    for component in sorted(components - covered_components):
        problems.append(f"prefix library {component} has no acknowledgement")
    for framework in sorted(covered_frameworks - frameworks):
        problems.append(f"acknowledged framework {framework} is not embedded")
    for component in sorted(covered_components - components):
        problems.append(f"acknowledged library {component} is in no iOS device prefix")
    return problems


# --- command line -----------------------------------------------------------


def kernel_source_from(root: Path) -> Path | None:
    value = os.environ.get("UML_KERNEL_SOURCE_DIR")
    if value:
        return Path(value)
    configuration = root / "kernel/mainline-6.18/source-location.conf"
    if configuration.is_file():
        for line in configuration.read_text(encoding="utf-8").splitlines():
            if line.startswith("UML_KERNEL_SOURCE_DIR="):
                return Path(line.split("=", 1)[1].strip())
    return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--verify", action="store_true", help="fail if the resource differs from the sources")
    mode.add_argument("--coverage", action="store_true", help="fail if the app embeds something unacknowledged")
    parser.add_argument(
        "--source-root",
        type=Path,
        default=Path(os.environ.get("UML_DEPENDENCY_SOURCE_ROOT", ROOT / "external/sources")),
        help="prepared dependency sources (default: external/sources)",
    )
    parser.add_argument("--kernel-source", type=Path, help="Linux source (default: the configured replay)")
    parser.add_argument("--resource", type=Path, default=RESOURCE, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    try:
        if args.coverage:
            document = json.loads(args.resource.read_text(encoding="utf-8"))
            problems = check_document(document, embedded_frameworks(), device_components())
            for problem in problems:
                print(f"acknowledgements.py: {problem}", file=sys.stderr)
            return 1 if problems else 0
        texts = Texts(args.source_root, args.kernel_source or kernel_source_from(ROOT))
        document = build(texts)
        problems = check_document(document, embedded_frameworks(), device_components())
        if problems:
            for problem in problems:
                print(f"acknowledgements.py: {problem}", file=sys.stderr)
            return 1
        rendered = render(document)
        if args.verify:
            if not args.resource.is_file() or args.resource.read_text(encoding="utf-8") != rendered:
                print(f"acknowledgements.py: {args.resource} is out of date; run the script", file=sys.stderr)
                return 1
            return 0
        args.resource.write_text(rendered, encoding="utf-8")
    except Problem as error:
        print(f"acknowledgements.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
