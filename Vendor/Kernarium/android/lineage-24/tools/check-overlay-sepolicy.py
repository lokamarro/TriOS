#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reject overlay sepolicy that the platform's property neverallow would refuse.

The platform keeps its core domains from setting a vendor-owned property
(system/sepolicy/private/property.te). The policy build reports a violation
only after Soong has analysed the tree, so this check reads the overlay's own
sources first: a property type that a vendor_*_prop macro declares may be set
only by vendor_init or by a domain that the overlay itself declares without the
coredomain attribute. A core domain such as adbd reports through a type of the
system_ext policy (overlay/sepolicy/system_ext/private) instead.

Init parses the product's init script as vendor_init and drops a trigger on a
property that domain may not read. A system_internal_prop is closed to it by
neverallow, so a property that a trigger of overlay/config/init.vendor.rc names
must not be one, and a system-owned one needs a get_prop(vendor_init, ...)
rule. The exact checks run on the built policy in verify-target-files.py.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import sys
from pathlib import Path

VENDOR_PROPERTY_MACRO = re.compile(r"\bvendor_(?:internal|restricted|public)_prop\(\s*(\w+)\s*\)")
DOMAIN_DECLARATION = re.compile(r"^\s*type\s+(\w+)\s*,([^;]*);", re.MULTILINE)
SET_PROP = re.compile(r"\bset_prop\(\s*(\w+)\s*,\s*(\w+)\s*\)")
SYSTEM_PROPERTY_MACRO = re.compile(r"\b(system_(?:internal|restricted|public))_prop\(\s*(\w+)\s*\)")
GET_PROP = re.compile(r"\b(?:get|set)_prop\(\s*vendor_init\s*,\s*(\w+)\s*\)")
INIT_PROPERTY_TRIGGER = re.compile(r"property:([^=\s&]+)=")
ALLOW_SET = re.compile(
    r"\ballow\s+(\w+)\s+(\w+)\s*:\s*property_service\s+(?:set|\{[^}]*\bset\b[^}]*\})\s*;"
)


def fail(message: str) -> None:
    print(f"overlay sepolicy: {message}", file=sys.stderr)
    raise SystemExit(1)


def strip_comments(text: str) -> str:
    return re.sub(r"#[^\n]*", "", text)


def check_init_triggers(overlay: Path, sources: dict[Path, str]) -> None:
    """A property named by a product init trigger is one that vendor_init may read."""
    rc = overlay / "config/init.vendor.rc"
    if not rc.is_file():
        return
    triggered = {
        name
        for line in strip_comments(rc.read_text(encoding="utf-8")).splitlines()
        if line.startswith("on ")
        for name in INIT_PROPERTY_TRIGGER.findall(line)
    }
    system_types: dict[str, str] = {}
    readable_by_vendor_init: set[str] = set()
    for text in sources.values():
        system_types.update({name: kind for kind, name in SYSTEM_PROPERTY_MACRO.findall(text)})
        readable_by_vendor_init.update(GET_PROP.findall(text))
    for path in sorted((overlay / "sepolicy").rglob("property_contexts")):
        for line in strip_comments(path.read_text(encoding="utf-8")).splitlines():
            fields = line.split()
            if len(fields) < 2 or fields[0] not in triggered:
                continue
            name, label = fields[0], fields[1].split(":")[2]
            where = f"{path.relative_to(overlay)}: init.vendor.rc triggers on {name} (type {label})"
            if system_types.get(label) == "system_internal":
                fail(f"{where}, which is a system_internal_prop that vendor_init may not read, so "
                     "init drops the trigger; declare it with system_public_prop")
            if label in system_types and label not in readable_by_vendor_init:
                fail(f"{where}, which lacks get_prop(vendor_init, {label}), so init drops the trigger")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--overlay", type=Path, required=True)
    args = parser.parse_args()
    sources = {
        path: strip_comments(path.read_text(encoding="utf-8"))
        for path in sorted((args.overlay / "sepolicy").rglob("*.te"))
    }
    vendor_properties: set[str] = set()
    vendor_domains = {"vendor_init"}
    for text in sources.values():
        vendor_properties.update(VENDOR_PROPERTY_MACRO.findall(text))
        for name, attributes in DOMAIN_DECLARATION.findall(text):
            words = {word.strip() for word in attributes.split(",")}
            if "domain" in words and "coredomain" not in words:
                vendor_domains.add(name)
    for path, text in sources.items():
        for pattern in (SET_PROP, ALLOW_SET):
            for match in pattern.finditer(text):
                domain, target = match.groups()
                if target in vendor_properties and domain not in vendor_domains:
                    fail(
                        f"{path.relative_to(args.overlay)}: {domain} may be a core domain and "
                        f"may not set the vendor-owned property type {target}; declare the "
                        "property with a system_*_prop macro under sepolicy/system_ext/private"
                    )
    check_init_triggers(args.overlay, sources)
    print(f"overlay sepolicy: {len(sources)} sources checked")


if __name__ == "__main__":
    main()
