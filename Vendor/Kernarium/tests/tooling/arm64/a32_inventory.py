#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The AArch32 instruction inventory: one row per XML encoding.

tests/unit/arm64/a32_instruction_inventory.tsv lists every encoding of the
pinned Arm AArch32 release (A32, T16 and T32, Base and FP&SIMD) with what the
virtual CPU's profile does with it. generate_a32_encoding_table.py writes the
file from the model; validate_a32_instruction_inventory.py checks it against
the generated table without the XML. Both use the rules below.

Columns:

- stable_id: BASE-nnnn or FPSIMD-nnnn, in the release's index order.
- section: Base or FP&SIMD, as the release indexes the page.
- heading: the instruction page's heading in the release's index.
- encoding: the XML encoding name, which is also the decoder's Form.
- isa: A32, T16 or T32.
- mnemonic: the encoding's mnemonic.
- level: the part of the profile it belongs to (base, simd, complete).
- feature: the optional extension a host must have, or baseline.
- profile: executed, restricted (the fields select what EL0 may touch),
  nop (executes as a NOP) or undefined (no word of it executes).
- engine_status: absent for undefined encodings, whose corpus cases expect an
  illegal instruction; pending for the others until an engine passes the
  corpus for them (verified).

engine_status is the one column the model cannot derive: an engine lane marks
an encoding verified once the corpus passes for it. The generator therefore
reads the statuses of the checked-in inventory (read_statuses) and keeps each
encoding's pending or verified; absent follows the profile, and an encoding
the inventory does not list starts pending. A regeneration from the same
release thus reproduces the checked-in file.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
from pathlib import Path

import a32_profile_policy as policy


COLUMNS = (
    "stable_id", "section", "heading", "encoding", "isa", "mnemonic", "level",
    "feature", "profile", "engine_status",
)
SECTION_PREFIXES = {"Base": "BASE", "FP&SIMD": "FPSIMD"}
ENGINE_STATUSES = ("absent", "pending", "verified")
PROFILES = ("executed", "restricted", "nop", "undefined")
LEVELS = ("base", "simd", "complete")


def profile_of(name: str, availability: str, check: str, nop: bool) -> str:
    """What the profile does with an encoding, from its table entry."""
    if availability == "undefined_at_el0" or name in policy.NEVER_EXECUTED:
        return "undefined"
    if check != "none":
        return "restricted"
    return "nop" if nop else "executed"


def status_of(profile: str, previous: str | None = None) -> str:
    """The engine status of an encoding with `profile`: absent when no word
    of it executes, else the status the inventory already records for it
    (pending or verified), pending when it records none."""
    if profile == "undefined":
        return "absent"
    return previous if previous in ("pending", "verified") else "pending"


def read_statuses(path: Path) -> dict[str, str]:
    """The engine status of each encoding in the inventory at `path`, or
    nothing when there is none yet."""
    if not path.is_file():
        return {}
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        if tuple(reader.fieldnames or ()) != COLUMNS:
            raise ValueError(f"{path}: columns are not {COLUMNS}")
        statuses = {}
        for row in reader:
            if row["engine_status"] not in ENGINE_STATUSES:
                raise ValueError(f"{path}: {row['encoding']}: engine status {row['engine_status']!r}")
            statuses[row["encoding"]] = row["engine_status"]
        return statuses


def rows(entries, statuses: dict[str, str] | None = None) -> list[dict[str, str]]:
    """Rows from (section, heading, name, isa, mnemonic, level, feature,
    availability, check, nop) tuples in index order; `statuses` carries the
    engine status each encoding already has (read_statuses)."""
    statuses = statuses or {}
    counts = {section: 0 for section in SECTION_PREFIXES}
    result = []
    for section, heading, name, isa, mnemonic, level, feature, availability, check, nop in entries:
        counts[section] += 1
        profile = profile_of(name, availability, check, nop)
        result.append({
            "stable_id": f"{SECTION_PREFIXES[section]}-{counts[section]:04d}",
            "section": section,
            "heading": heading,
            "encoding": name,
            "isa": isa,
            "mnemonic": mnemonic,
            "level": level,
            "feature": feature or "baseline",
            "profile": profile,
            "engine_status": status_of(profile, statuses.get(name)),
        })
    return result
