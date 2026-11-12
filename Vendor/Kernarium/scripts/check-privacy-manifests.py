#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the privacy manifests and Info.plist files of built iOS bundles.

Each path is an application, a framework, an XCFramework or a directory
holding them. For every .app and .framework found, the check fails when:

  - the bundle has no PrivacyInfo.xcprivacy, or the manifest tracks, names
    tracking domains or declares collected data (this project collects
    nothing), or declares a category or reason code Apple does not define;
  - its executable uses a required-reason API whose category the manifest
    does not declare;
  - its Info.plist leaves an identity or version key empty, or has no
    MinimumOSVersion.

An API is found from the executable's undefined symbols (C functions,
Objective-C constants and classes, Swift overlay accessors) and from the
Objective-C selector names it calls, which have no symbol of their own.
The API list is Apple's "Describing use of required reason API".
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import plistlib
import re
import struct
import subprocess
import sys
from pathlib import Path

NM_TIMEOUT_SECONDS = 300
MANIFEST = "PrivacyInfo.xcprivacy"

PREFIX = "NSPrivacyAccessedAPICategory"
FILE_TIMESTAMP = PREFIX + "FileTimestamp"
SYSTEM_BOOT_TIME = PREFIX + "SystemBootTime"
DISK_SPACE = PREFIX + "DiskSpace"
ACTIVE_KEYBOARDS = PREFIX + "ActiveKeyboards"
USER_DEFAULTS = PREFIX + "UserDefaults"

# The reason codes Apple defines for each category.
REASONS = {
    FILE_TIMESTAMP: {"DDA9.1", "C617.1", "3B52.1", "0A2A.1"},
    SYSTEM_BOOT_TIME: {"35F9.1", "8FFB.1", "3D61.1"},
    DISK_SPACE: {"85F4.1", "E174.1", "7D9E.1", "B728.1"},
    ACTIVE_KEYBOARDS: {"3EC4.1", "54BD.1"},
    USER_DEFAULTS: {"CA92.1", "1C8F.1", "C56D.1", "AC6B.1"},
}

# Each rule maps an API to the categories that cover it: any one declared
# category is enough (getattrlist reads timestamps and volume sizes alike).
SYMBOL_RULES: dict[str, tuple[str, ...]] = {
    "stat": (FILE_TIMESTAMP,),
    "fstat": (FILE_TIMESTAMP,),
    "lstat": (FILE_TIMESTAMP,),
    "fstatat": (FILE_TIMESTAMP,),
    "getattrlist": (FILE_TIMESTAMP, DISK_SPACE),
    "fgetattrlist": (FILE_TIMESTAMP, DISK_SPACE),
    "getattrlistat": (FILE_TIMESTAMP, DISK_SPACE),
    "getattrlistbulk": (FILE_TIMESTAMP,),
    "NSFileCreationDate": (FILE_TIMESTAMP,),
    "NSFileModificationDate": (FILE_TIMESTAMP,),
    "NSURLContentModificationDateKey": (FILE_TIMESTAMP,),
    "NSURLCreationDateKey": (FILE_TIMESTAMP,),
    "mach_absolute_time": (SYSTEM_BOOT_TIME,),
    "statfs": (DISK_SPACE,),
    "fstatfs": (DISK_SPACE,),
    "statvfs": (DISK_SPACE,),
    "fstatvfs": (DISK_SPACE,),
    "NSURLVolumeAvailableCapacityKey": (DISK_SPACE,),
    "NSURLVolumeAvailableCapacityForImportantUsageKey": (DISK_SPACE,),
    "NSURLVolumeAvailableCapacityForOpportunisticUsageKey": (DISK_SPACE,),
    "NSURLVolumeTotalCapacityKey": (DISK_SPACE,),
    "NSFileSystemFreeSize": (DISK_SPACE,),
    "NSFileSystemSize": (DISK_SPACE,),
    "OBJC_CLASS_$_NSUserDefaults": (USER_DEFAULTS,),
}

# Swift's Foundation overlay reaches the same data through mangled accessors,
# which carry the property name after its length: ...V40volumeAvailable...
MANGLED_RULES: list[tuple[re.Pattern[str], tuple[str, ...]]] = [
    (re.compile(r"URLResourceValuesV\d+(creationDate|contentModificationDate)"),
     (FILE_TIMESTAMP,)),
    (re.compile(
        r"URLResourceValuesV\d+(volumeAvailableCapacity|"
        r"volumeAvailableCapacityForImportantUsage|"
        r"volumeAvailableCapacityForOpportunisticUsage|volumeTotalCapacity)"),
     (DISK_SPACE,)),
    (re.compile(r"FileAttributeKeyV\d+(creationDate|modificationDate)"),
     (FILE_TIMESTAMP,)),
    (re.compile(r"FileAttributeKeyV\d+(systemFreeSize|systemSize)"),
     (DISK_SPACE,)),
    (re.compile(r"ProcessInfoC\d+systemUptime"), (SYSTEM_BOOT_TIME,)),
    (re.compile(r"\$s10Foundation12UserDefaultsC"), (USER_DEFAULTS,)),
]

# Objective-C methods named by the selector alone.
SELECTOR_RULES: dict[str, tuple[str, ...]] = {
    "systemUptime": (SYSTEM_BOOT_TIME,),
    "activeInputModes": (ACTIVE_KEYBOARDS,),
    "fileModificationDate": (FILE_TIMESTAMP,),
    "fileCreationDate": (FILE_TIMESTAMP,),
}

VERSION = re.compile(r"^\d+(\.\d+){0,2}$")
IDENTITY_KEYS = ("CFBundleIdentifier", "CFBundleExecutable", "CFBundleName")
VERSION_KEYS = ("CFBundleShortVersionString", "CFBundleVersion")

MACHO_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
CPU_TYPE_ARM64 = 0x0100000C
LC_SEGMENT_64 = 0x19


def thin_offset(data: bytes) -> int | None:
    """Where the arm64 Mach-O begins in `data`, or None if there is none."""
    if len(data) < 8:
        return None
    magic = struct.unpack_from(">I", data, 0)[0]
    if magic == FAT_MAGIC:
        count = struct.unpack_from(">I", data, 4)[0]
        for index in range(min(count, 16)):
            cpu, _sub, offset, _size, _align = struct.unpack_from(
                ">IIIII", data, 8 + 20 * index)
            if cpu == CPU_TYPE_ARM64:
                return offset
        return None
    if struct.unpack_from("<I", data, 0)[0] == MACHO_64:
        return 0
    return None


def section(data: bytes, base: int, wanted: tuple[str, str]) -> bytes:
    """The bytes of one section of the thin Mach-O at `base`."""
    count = struct.unpack_from("<I", data, base + 16)[0]
    position = base + 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", data, position)
        if command == LC_SEGMENT_64:
            sections = struct.unpack_from("<I", data, position + 64)[0]
            entry = position + 72
            for _ in range(sections):
                name = data[entry:entry + 16].rstrip(b"\0").decode()
                segment = data[entry + 16:entry + 32].rstrip(b"\0").decode()
                length, offset = struct.unpack_from("<QI", data, entry + 40)
                if (segment, name) == wanted:
                    return data[base + offset:base + offset + length]
                entry += 80
        position += size
    return b""


def selectors(path: Path) -> set[str]:
    data = path.read_bytes()
    base = thin_offset(data)
    if base is None:
        return set()
    names = section(data, base, ("__TEXT", "__objc_methname"))
    return {part.decode("utf-8", "replace") for part in names.split(b"\0") if part}


def undefined_symbols(path: Path, nm: list[str]) -> list[str]:
    listed = subprocess.run(
        [*nm, "-u", "-P", str(path)],
        check=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
        timeout=NM_TIMEOUT_SECONDS,
    )
    if listed.returncode != 0:
        raise SystemExit(f"{path}: nm failed: {listed.stderr.strip()}")
    names = []
    for line in listed.stdout.splitlines():
        fields = line.split()
        if fields:
            names.append(fields[0][1:] if fields[0].startswith("_") else fields[0])
    return names


def used_apis(path: Path, nm: list[str]) -> dict[tuple[str, ...], str]:
    """Each required-reason API the executable uses, with how it was found."""
    found: dict[tuple[str, ...], str] = {}
    for symbol in undefined_symbols(path, nm):
        rule = SYMBOL_RULES.get(symbol)
        if rule is None:
            for pattern, categories in MANGLED_RULES:
                if pattern.search(symbol):
                    rule = categories
                    break
        if rule is not None:
            found.setdefault(rule, f"symbol {symbol}")
    for name in selectors(path):
        rule = SELECTOR_RULES.get(name)
        if rule is not None:
            found.setdefault(rule, f"selector {name}")
    return found


def load_plist(path: Path) -> dict | None:
    try:
        with path.open("rb") as handle:
            loaded = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException, ValueError):
        return None
    return loaded if isinstance(loaded, dict) else None


def manifest_problems(manifest: dict) -> tuple[list[str], set[str]]:
    """What is wrong with a manifest, and the categories it declares."""
    found = []
    if manifest.get("NSPrivacyTracking") is not False:
        found.append("NSPrivacyTracking must be false")
    if manifest.get("NSPrivacyTrackingDomains") != []:
        found.append("NSPrivacyTrackingDomains must be an empty array")
    if manifest.get("NSPrivacyCollectedDataTypes") != []:
        found.append("NSPrivacyCollectedDataTypes must be an empty array")
    declared: set[str] = set()
    entries = manifest.get("NSPrivacyAccessedAPITypes")
    if not isinstance(entries, list):
        return found + ["NSPrivacyAccessedAPITypes must be an array"], declared
    for entry in entries:
        category = entry.get("NSPrivacyAccessedAPIType") if isinstance(entry, dict) else None
        if category not in REASONS:
            found.append(f"unknown API category {category!r}")
            continue
        if category in declared:
            found.append(f"{category} is declared twice")
        declared.add(category)
        reasons = entry.get("NSPrivacyAccessedAPITypeReasons")
        if not isinstance(reasons, list) or not reasons:
            found.append(f"{category} declares no reason")
            continue
        for reason in reasons:
            if reason not in REASONS[category]:
                found.append(f"{category} has no reason code {reason!r}")
    return found, declared


def bundle_problems(bundle: Path, nm: list[str]) -> list[str]:
    found = []
    info = load_plist(bundle / "Info.plist")
    if info is None:
        return [f"{bundle}: has no readable Info.plist"]
    for key in IDENTITY_KEYS:
        if not isinstance(info.get(key), str) or not info[key]:
            found.append(f"{bundle}: Info.plist {key} is empty or missing")
    for key in VERSION_KEYS:
        value = info.get(key)
        if not isinstance(value, str) or not VERSION.match(value):
            found.append(
                f"{bundle}: Info.plist {key} is {value!r}, not a dotted number")
    minimum = info.get("MinimumOSVersion")
    if not isinstance(minimum, str) or not VERSION.match(minimum):
        found.append(f"{bundle}: Info.plist MinimumOSVersion is {minimum!r}")

    manifest = load_plist(bundle / MANIFEST)
    declared: set[str] = set()
    if manifest is None:
        found.append(f"{bundle}: has no readable {MANIFEST}")
    else:
        problems, declared = manifest_problems(manifest)
        found.extend(f"{bundle}: {MANIFEST}: {problem}" for problem in problems)

    executable = bundle / str(info.get("CFBundleExecutable", ""))
    if not executable.is_file():
        found.append(f"{bundle}: executable {executable.name!r} is missing")
        return found
    if thin_offset(executable.read_bytes()[:4096]) is None:
        found.append(f"{bundle}: executable is not an arm64 Mach-O")
        return found
    for categories, how in sorted(used_apis(executable, nm).items()):
        if not declared.intersection(categories):
            names = " or ".join(categories)
            found.append(
                f"{bundle}: uses a required-reason API ({how}) but "
                f"{MANIFEST} declares none of {names}")
    return found


def bundles(root: Path) -> list[Path]:
    if not root.exists():
        raise SystemExit(f"{root}: no such path")
    candidates = [root, *sorted(root.rglob("*"))]
    return [
        path for path in candidates
        if path.is_dir() and not path.is_symlink()
        and path.suffix in (".app", ".framework")
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--nm", default="xcrun nm", help="the nm command")
    options = parser.parse_args()
    nm = options.nm.split()

    checked = 0
    problems: list[str] = []
    for root in options.paths:
        for bundle in bundles(root):
            checked += 1
            problems.extend(bundle_problems(bundle, nm))
    if checked == 0:
        print("no application or framework bundle found", file=sys.stderr)
        return 1
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"checked {checked} bundle(s): privacy manifests and Info.plist keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
