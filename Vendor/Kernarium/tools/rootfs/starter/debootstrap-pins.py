#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record the exact inputs of a debootstrapped Debian root.

debootstrap verifies the archive's signed InRelease file, the package index
named in it and every package it downloads, then installs them. This reads
what it left behind and writes the pins a release manifest needs: an
inventory with one row per installed package (its file name in the archive,
its size and its SHA-256) and the signed release and package index that
named them. It cross-checks the three sources, so a package installed from
anywhere but the verified index, or an index that no longer matches the
signed release, is an error.

The inventory columns are tab-separated and unheadered:
package (with an :ARCH suffix for a Multi-Arch: same package, as
dpkg-query's ${binary:Package} prints it), version, architecture, size in
bytes and SHA-256 of the .deb file.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


class PinError(RuntimeError):
    pass


def digest(path: Path) -> tuple[str, int]:
    value = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            value.update(chunk)
            size += len(chunk)
    return value.hexdigest(), size


def stanzas(path: Path):
    """Yield each blank-line-separated control stanza as a field dict."""
    for block in path.read_text(encoding="utf-8").split("\n\n"):
        fields: dict[str, str] = {}
        key = None
        for line in block.splitlines():
            if line.startswith((" ", "\t")):
                if key is not None:
                    fields[key] += "\n" + line
            elif ":" in line:
                key, _, value = line.partition(":")
                fields[key] = value.strip()
        if fields:
            yield fields


def installed_packages(root: Path) -> list[dict[str, str]]:
    status = root / "var/lib/dpkg/status"
    packages = []
    for fields in stanzas(status):
        if fields.get("Status") != "install ok installed":
            raise PinError(
                f"{fields.get('Package')} is {fields.get('Status')!r}, "
                "not installed")
        packages.append(fields)
    if not packages:
        raise PinError(f"no installed packages in {status}")
    return packages


def package_index(path: Path, wanted: set[tuple[str, str, str]]
                  ) -> dict[tuple[str, str, str], dict[str, str]]:
    found = {}
    for fields in stanzas(path):
        key = (fields.get("Package", ""), fields.get("Version", ""),
               fields.get("Architecture", ""))
        if key in wanted:
            found[key] = fields
    return found


def release_hashes(release: Path) -> tuple[dict[str, str], dict[str, tuple[str, int]]]:
    """Return the Release header fields and its SHA256 table."""
    header: dict[str, str] = {}
    table: dict[str, tuple[str, int]] = {}
    section = None
    for line in release.read_text(encoding="utf-8").splitlines():
        if line.startswith(" "):
            if section == "SHA256":
                checksum, size, name = line.split()
                table[name] = (checksum, int(size))
        elif ":" in line:
            section, _, value = line.partition(":")
            header[section] = value.strip()
    return header, table


def signed_content(in_release: Path) -> str:
    """Return the text a clear-signed file signs, with its dashes unescaped."""
    text = in_release.read_text(encoding="utf-8")
    start = text.index("\n\n") + 2
    end = text.index("\n-----BEGIN PGP SIGNATURE-----")
    return "\n".join(line[2:] if line.startswith("- ") else line
                     for line in text[start:end].splitlines()).rstrip("\n")


def locate(lists: Path, suffix: str) -> Path:
    matches = sorted(lists.glob(f"*{suffix}"))
    if len(matches) != 1:
        raise PinError(f"expected one *{suffix} in {lists}, found {len(matches)}")
    return matches[0]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--cache", required=True, type=Path,
                        help="debootstrap's --cache-dir, holding the .deb files")
    parser.add_argument("--architecture", required=True)
    parser.add_argument("--suite", required=True)
    parser.add_argument("--component", default="main")
    parser.add_argument("--inventory", required=True, type=Path)
    parser.add_argument("--pins", required=True, type=Path)
    options = parser.parse_args(argv)

    root = options.root.resolve(strict=True)
    lists = root / "var/lib/apt/lists"
    prefix = f"_dists_{options.suite}_"
    in_release = locate(lists, f"{prefix}InRelease")
    release = locate(lists, f"{prefix}Release")
    index = locate(lists, f"{prefix}{options.component}_binary-"
                   f"{options.architecture}_Packages")

    header, table = release_hashes(release)
    index_name = f"{options.component}/binary-{options.architecture}/Packages"
    compressed_name = f"{index_name}.xz"
    for name in (index_name, compressed_name):
        if name not in table:
            raise PinError(f"the release lists no {name}")
    index_sha256, index_size = digest(index)
    if (index_sha256, index_size) != table[index_name]:
        raise PinError("the package index does not match the release")
    if signed_content(in_release) != release.read_text(encoding="utf-8").rstrip("\n"):
        raise PinError("the Release file is not the signed InRelease content")

    packages = installed_packages(root)
    keys = {(p["Package"], p["Version"], p["Architecture"]) for p in packages}
    entries = package_index(index, keys)
    missing = sorted(keys - set(entries))
    if missing:
        raise PinError(f"installed but not in the verified index: {missing}")

    cache = options.cache.resolve(strict=True)
    expected: dict[str, tuple[str, str, str]] = {}
    rows = []
    for fields in packages:
        name, version, architecture = (fields["Package"], fields["Version"],
                                       fields["Architecture"])
        filename = f"{name}_{version.replace(':', '%3a')}_{architecture}.deb"
        path = cache / filename
        if not path.is_file() or path.is_symlink():
            raise PinError(f"{filename} was not downloaded")
        checksum, size = digest(path)
        indexed = entries[(name, version, architecture)]
        if checksum != indexed.get("SHA256") or size != int(indexed["Size"]):
            raise PinError(f"{filename} does not match the verified index")
        qualified = (f"{name}:{architecture}"
                     if fields.get("Multi-Arch") == "same" else name)
        expected[filename] = (name, version, architecture)
        rows.append((qualified, version, architecture, size, checksum))
    extra = sorted(entry.name for entry in cache.iterdir()
                   if entry.name not in expected)
    if extra:
        raise PinError(f"downloaded but not installed: {extra}")
    rows.sort()
    options.inventory.write_text(
        "".join("\t".join(map(str, row)) + "\n" for row in rows),
        encoding="utf-8")

    in_release_sha256, in_release_size = digest(in_release)
    package_bytes = sum(row[3] for row in rows)
    compressed_sha256, compressed_size = table[compressed_name]
    pins = {
        "architecture": options.architecture,
        "component": options.component,
        "suite": options.suite,
        "release": {
            "codename": header.get("Codename"),
            "date": header["Date"],
            "version": header.get("Version"),
            "in_release_sha256": in_release_sha256,
            "in_release_size": in_release_size,
            "package_index": {
                "path": compressed_name,
                "sha256": compressed_sha256,
                "size": compressed_size,
                "unpacked_path": index_name,
                "unpacked_sha256": index_sha256,
                "unpacked_size": index_size,
            },
        },
        "downloads": {
            "packages": len(rows),
            "package_bytes": package_bytes,
            "index_bytes": compressed_size + in_release_size,
            "total_bytes": package_bytes + compressed_size + in_release_size,
        },
    }
    options.pins.write_text(json.dumps(pins, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
    print(f"pinned {len(rows)} packages, {package_bytes} package bytes, "
          f"{pins['downloads']['total_bytes']} downloaded bytes")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (PinError, OSError) as error:
        print(f"debootstrap-pins.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
