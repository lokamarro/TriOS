#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the checked-in AArch32 encoding table against itself and its data.

generate_a32_encoding_table.py writes the decoder's tables
(darwin/cpu/arm64/aarch32) and the decoder test data
(tests/unit/arm64/aarch32_decoder). This offline test reads the generated
files back, without the Arm XML or LLVM, and requires:

- the declared counts, the Form enumeration and the FormInfo table to agree,
  and every index, range and tree edge to be in bounds;
- the decision trees, walked here from the rendered arrays, to find for every
  stored sample word the row of the form the sample names, and for every
  16-bit Thumb word the form of the stored run that holds it;
- the stored T16 runs to cover the whole 16-bit space once, in order;
- every form to have a witness word that the trees map back to it and, unless
  the profile makes the form undefined at EL0, an accepted sample; and
- the expected-operand digest header to mix every Operands member; and
- the inventory's engine statuses to survive a regeneration (the rules the
  generator applies to the checked-in inventory's statuses).

The freshness test (arm64-a32-encoding-table-freshness) compares the files
with a regeneration from the pinned release.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import re
import sys
import tempfile
from pathlib import Path

import a32_inventory as inventory

from a32_profile_policy import NEVER_EXECUTED
from a32_table_reader import (
    DATA,
    ISAS,
    REASONS,
    ROW_KINDS,
    STATUSES,
    THUMB32_FIRST,
    TableError,
    Tables,
    require,
    read,
)


def read_tsv(path: Path, columns: tuple[str, ...]) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle, delimiter="\t")
        require(tuple(reader.fieldnames or ()) == columns, f"{path.name} columns are not {columns}")
        return list(reader)


def check_samples(tables: Tables) -> None:
    columns = ("isa", "word", "kind", "form", "clean", "violation", "llvm", "soft",
               "status", "reason", "sysreg", "cond", "digest")
    rows = read_tsv(DATA / "samples.tsv", columns)
    require(len(rows) > 5000, f"only {len(rows)} stored samples")
    accepted: set[str] = set()
    seen: set[tuple[str, str]] = set()
    for row in rows:
        key = (row["isa"], row["word"])
        require(row["isa"] in ISAS, f"sample {key} has an unknown instruction set")
        require(key not in seen, f"sample {key} is stored twice")
        seen.add(key)
        word = int(row["word"], 16)
        require(row["kind"] in ROW_KINDS, f"sample {key} has kind {row['kind']}")
        require(row["form"] in tables.form_enum, f"sample {key} names {row['form']}")
        require(row["status"] in STATUSES and row["reason"] in REASONS,
                f"sample {key} has status {row['status']}/{row['reason']}")
        require((row["status"] == "undefined") == (row["reason"] != "none"),
                f"sample {key} status and reason disagree")
        require(row["cond"].isdigit() and int(row["cond"]) <= 14, f"sample {key} cond")
        require(re.fullmatch(r"[0-9a-f]{16}", row["digest"]) is not None, f"sample {key} digest")
        require(row["status"] == "ok" or int(row["digest"], 16) == 0,
                f"sample {key} has operands but is not ok")
        require(row["isa"] != "T16" or word & 0xFFFF == 0, f"sample {key} has bits below a halfword")
        require(row["isa"] != "T32" or word >> 16 >= THUMB32_FIRST, f"sample {key} is not 32-bit Thumb")
        found = tables.form_of(row["isa"], word)
        require(found == row["form"], f"sample {key} walks to {found}, not {row['form']}")
        if row["status"] != "undefined" and row["violation"] == "0":
            accepted.add(row["form"])
    for index, form in enumerate(tables.forms):
        if index < 2 or form["availability"] == "undefined_at_el0":
            continue
        if form["name"] in NEVER_EXECUTED:
            require(form["name"] not in accepted, f"{form['name']} has an accepted sample")
            continue
        require(form["name"] in accepted, f"no accepted sample for {form['name']}")


def check_witnesses(tables: Tables) -> None:
    for index, form in enumerate(tables.forms):
        if index < 2:
            require(form["witness"] == 0, f"{form['name']} has a witness")
            continue
        isa = {"a32": "A32", "t16": "T16", "t32": "T32"}[form["isa"]]
        word = form["witness"]
        require(tables.form_of(isa, word) == form["name"],
                f"the witness {word:#010x} of {form['name']} walks to "
                f"{tables.form_of(isa, word)}")


def check_t16(tables: Tables) -> None:
    columns = ("first", "last", "form", "llvm", "soft", "status", "reason")
    runs = read_tsv(DATA / "t16_reference.tsv", columns)
    next_halfword = 0
    for run in runs:
        first, last = int(run["first"], 16), int(run["last"], 16)
        require(first == next_halfword and last >= first, f"T16 run {run['first']} is not contiguous")
        next_halfword = last + 1
        require(run["form"] in tables.form_enum, f"T16 run {run['first']} names {run['form']}")
        require(run["status"] in STATUSES and run["reason"] in REASONS, f"T16 run {run['first']} status")
        for halfword in range(first, last + 1):
            found = tables.form_of("T16", halfword << 16)
            require(found == run["form"], f"T16 {halfword:04x} walks to {found}, not {run['form']}")
    require(next_halfword == THUMB32_FIRST, f"T16 runs end at {next_halfword:#06x}")


def check_digest_header(tables: Tables) -> None:
    operands = re.search(r"struct Operands final \{\n(.*?)\n\};", tables.header, re.S)
    require(operands is not None, "a32_encodings.hpp does not declare Operands")
    members = re.findall(r"^\t[\w:<> ]+? (\w+)\{?\}?(?: = [^;]+)?;", operands.group(1), re.M)
    require(members and members[-1] == "carry_flags", "Operands does not end with carry_flags")
    text = read(DATA / "operands_digest.hpp")
    mixed = re.findall(r"mix\(static_cast<std::uint64_t>\(ops\.(\w+)\)\);", text)
    require(mixed == members, "operands_digest.hpp does not mix the members of Operands in order")


def check_inventory_statuses() -> None:
    """A regeneration keeps pending and verified, and absent follows the
    profile."""
    def entry(name: str, availability: str = "implemented", check: str = "none") -> tuple:
        return ("Base", "heading", name, "A32", "MNEMONIC", "base", None, availability, check, False)

    entries = [entry("A"), entry("B"), entry("C"), entry("D", "undefined_at_el0"), entry("E")]
    previous = {"A": "verified", "B": "pending", "D": "verified", "E": "absent"}
    got = {row["encoding"]: row["engine_status"] for row in inventory.rows(entries, previous)}
    require(
        got == {"A": "verified", "B": "pending", "C": "pending", "D": "absent", "E": "pending"},
        f"inventory statuses {got}",
    )
    require(
        {row["engine_status"] for row in inventory.rows(entries)} == {"pending", "absent"},
        "an inventory without statuses starts pending",
    )
    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch) / "inventory.tsv"
        require(inventory.read_statuses(path) == {}, "a missing inventory has no statuses")
        rows = inventory.rows(entries, previous)
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=inventory.COLUMNS, delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)
        require(inventory.read_statuses(path) == {row["encoding"]: row["engine_status"] for row in rows},
                "read_statuses does not read back what rows wrote")
        rows[0]["engine_status"] = "done"
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=inventory.COLUMNS, delimiter="\t")
            writer.writeheader()
            writer.writerows(rows)
        try:
            inventory.read_statuses(path)
        except ValueError:
            return
        raise TableError("an unknown engine status was accepted")


def main() -> int:
    tables = Tables()
    tables.check_structure()
    check_witnesses(tables)
    check_samples(tables)
    check_t16(tables)
    check_digest_header(tables)
    check_inventory_statuses()
    counts = tables.counts
    print(
        f"AArch32 encoding table is consistent: {counts['kFormCount']} forms, "
        f"{counts['kRowCount']} rows, {counts['kProgramCount']} programs"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, TableError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
