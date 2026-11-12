#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the AArch32 instruction inventory against the decoder's table.

The inventory (tests/unit/arm64/a32_instruction_inventory.tsv, rules in
a32_inventory.py) must list the table's forms one for one, in order, with the
table's instruction set, mnemonic, level and feature, with the profile the
rules derive, and with an engine status the profile allows. The coverage
document states the counts. Nothing here reads the Arm XML or LLVM.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import re
import sys
from collections import Counter
from pathlib import Path

import a32_inventory as inventory
import a32_profile_policy as policy
from a32_table_reader import REPOSITORY, Tables, TableError, read, require


INVENTORY = REPOSITORY / "tests/unit/arm64/a32_instruction_inventory.tsv"
COVERAGE = REPOSITORY / "docs/a32-instruction-coverage.md"
ISA_NAMES = {"a32": "A32", "t16": "T16", "t32": "T32"}


def counts_sentence(rows: list[dict[str, str]]) -> str:
    sections = Counter(row["section"] for row in rows)
    statuses = Counter(row["engine_status"] for row in rows)
    return (
        f"`tests/unit/arm64/a32_instruction_inventory.tsv` lists {len(rows):,} "
        f"encodings ({sections['Base']:,} Base and {sections['FP&SIMD']:,} FP&SIMD): "
        f"{statuses['pending']:,} `pending`, {statuses['verified']:,} `verified` "
        f"and {statuses['absent']:,} `absent`."
    )


def levels_sentence(rows: list[dict[str, str]]) -> str:
    levels = Counter(row["level"] for row in rows)
    return (
        f"By level there are {levels['base']:,} `base`, {levels['simd']:,} "
        f"`simd` and {levels['complete']:,} `complete` encodings."
    )


def validate(rows: list[dict[str, str]], tables: Tables) -> None:
    forms = tables.forms[2:]
    require(len(rows) == len(forms), f"{len(rows)} rows for {len(forms)} forms")
    section_counts: Counter[str] = Counter()
    for row, form in zip(rows, forms):
        name = form["name"]
        require(row["encoding"] == name, f"{row['stable_id']} is {row['encoding']}, not {name}")
        section = row["section"]
        require(section in inventory.SECTION_PREFIXES, f"{name}: unknown section {section!r}")
        section_counts[section] += 1
        expected_id = f"{inventory.SECTION_PREFIXES[section]}-{section_counts[section]:04d}"
        require(row["stable_id"] == expected_id, f"{name}: stable ID {row['stable_id']}, not {expected_id}")
        require(row["heading"].strip() == row["heading"] and row["heading"], f"{name}: heading")
        require(row["isa"] == ISA_NAMES[form["isa"]], f"{name}: isa {row['isa']}")
        require(row["mnemonic"] == form["mnemonic"], f"{name}: mnemonic {row['mnemonic']}")
        require(row["level"] in inventory.LEVELS and row["level"] == form["level"],
                f"{name}: level {row['level']}")
        feature = "baseline" if form["feature"] is None else policy.FEATURE_ORDER[form["feature"]]
        require(row["feature"] == feature, f"{name}: feature {row['feature']}, not {feature}")
        profile = inventory.profile_of(
            name, form["availability"], form["check"], form["nop"]
        )
        require(row["profile"] == profile, f"{name}: profile {row['profile']}, not {profile}")
        status = row["engine_status"]
        require(status in inventory.ENGINE_STATUSES, f"{name}: engine status {status!r}")
        if profile == "undefined":
            require(status == "absent", f"{name}: an undefined encoding must be absent")
        else:
            require(status in ("pending", "verified"), f"{name}: an executed encoding is not {status}")
    require(len({row["encoding"] for row in rows}) == len(rows), "encodings are not unique")
    # Sections are index-ordered runs, so a Base row never follows an FP&SIMD one.
    seen_simd = False
    for row in rows:
        seen_simd = seen_simd or row["section"] == "FP&SIMD"
        require(not (seen_simd and row["section"] == "Base"), f"{row['encoding']}: section order")
    for name in policy.NEVER_EXECUTED:
        require(any(row["encoding"] == name and row["profile"] == "undefined" for row in rows),
                f"{name} is not marked undefined")


def main() -> int:
    with INVENTORY.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        require(tuple(reader.fieldnames or ()) == inventory.COLUMNS, f"columns are {reader.fieldnames!r}")
        rows = list(reader)
    tables = Tables()
    validate(rows, tables)
    document = re.sub(r"\s+", " ", read(COVERAGE))
    for sentence in (counts_sentence(rows), levels_sentence(rows)):
        require(sentence in document, f"docs/a32-instruction-coverage.md lacks: {sentence}")
    statuses = Counter(row["engine_status"] for row in rows)
    print(
        f"AArch32 instruction inventory valid: {len(rows)} rows; "
        f"{statuses['verified']} verified, {statuses['pending']} pending, "
        f"{statuses['absent']} absent"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, TableError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
