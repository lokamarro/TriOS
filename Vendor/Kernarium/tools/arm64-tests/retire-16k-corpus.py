#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Retire the 16 KiB guest page profile from an A64 instruction corpus dump.

usage: retire-16k-corpus.py DUMP OUTPUT

The guest has 4 KiB pages only. The producer export that the corpus came from
ran both page profiles, so its dump holds records for the retired one. This
step removes them:

- a record for both profiles becomes a record for 4 KiB, with its 16 KiB
  producer check ordinals cleared;
- a record for 16 KiB alone is deleted, except for the cases below;
- a 16 KiB case whose entry word no 4 KiB case executes is carried over, so the
  encodings the corpus covers do not shrink, only its page-geometry cases. The
  twin sites sample the boundaries of their own page size, so a word that
  reaches only a 16 KiB boundary (a branch displacement or literal-load offset
  near +-16 KiB) has no 4 KiB twin.

A carried case keeps its instruction word, its program and its architectural
state, and changes only where it runs. The 4 KiB code page holds offsets 0 to
4092, so the program moves down by the multiple of 4 KiB that brings its entry
into the page. The memory regions a PC-relative word reaches move with it, and
so do the results that depend on the PC: the final PC, X30 after BL and BLR,
and the fault address. A program that straddles a 4 KiB boundary and reaches no
memory moves to the middle of the page instead, and a program whose last word
lies a page past its first puts that word into a code region after the page.
The corpus runner checks the placement: it executes the carried cases in both
lanes at 4 KiB like any other.

Run it on the dump of the SM = 0 vector-length step, before the
invalid-encoding step, and --repack the result.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import pathlib
import sys
import zlib

CODE_BASE = 0x4F00_0000
PAGE = 4096
RETIRED_PAGE = 16384
# The flags of a memory region that holds guest code.
CODE_REGION_FLAGS = 31

SCRIPT = "tools/arm64-tests/retire-16k-corpus.py"


def fail(message: str) -> None:
    sys.exit(f"retire-16k-corpus.py: {message}")


# --------------------------------------------------------------------------
# Instruction classes the relocation depends on


def is_pc_relative(word: int) -> bool:
    """B, BL, B.cond, CBZ, CBNZ, TBZ, TBNZ, LDR (literal), PRFM (literal)
    and ADR, ADRP: the words whose result depends on where they run."""
    return ((word & 0x7C00_0000) == 0x1400_0000 or
            (word & 0xFF00_0000) == 0x5400_0000 or
            (word & 0x7E00_0000) in (0x3400_0000, 0x3600_0000) or
            (word & 0x3B00_0000) == 0x1800_0000 or
            (word & 0x1F00_0000) == 0x1000_0000)


def links(word: int) -> bool:
    """BL and BLR: the words that set X30 to the next instruction."""
    return (word & 0xFC00_0000) == 0x9400_0000 or \
        (word & 0xFFFF_FC1F) == 0xD63F_0000


# --------------------------------------------------------------------------
# Relocation


class Unplaceable(Exception):
    pass


def in_retired_code(value: int) -> bool:
    return CODE_BASE <= value < CODE_BASE + RETIRED_PAGE


def hex64(value: int) -> str:
    return f"0x{value:016x}"


def placement(entry: int, offsets: list[int], regions: bool) -> tuple[int, bool]:
    """The shift of a program into the 4 KiB code page and whether words past
    the page's end go into a code region."""
    aligned = -(entry & ~(PAGE - 1))
    low, high = min(offsets) + aligned, max(offsets) + aligned
    if low >= 0 and high <= PAGE - 4:
        return aligned, False
    if low >= 0 and high - low >= PAGE:
        return aligned, True
    if regions:
        raise Unplaceable("memory regions cannot follow an unaligned shift")
    shift = PAGE // 2 - entry
    if min(offsets) + shift < 0 or max(offsets) + shift > PAGE - 4:
        raise Unplaceable("the program does not fit the code page")
    return shift, False


def relocate(record: dict, case: dict, new_blobs: dict[str, str]):
    """The 4 KiB program and case that carry a 16 KiB case's entry word."""
    program = record["programs"][case["program"]]
    entry = program["entry"]
    words = [(offset, int(word, 16)) for offset, word in program["words"]]
    entry_word = dict(words)[entry]
    pc_relative = is_pc_relative(entry_word)
    memory = case.get("memory", [])
    shift, spill = placement(entry, [offset for offset, _ in words],
                             bool(memory))
    if memory and shift % PAGE != 0:
        raise Unplaceable("memory regions cannot follow an unaligned shift")

    def moved(value: int) -> int:
        """A PC-dependent address: all of them move for a PC-relative word,
        and only those in the retired code window for any other."""
        return value + shift if pc_relative or in_retired_code(value) else value

    result = json.loads(json.dumps(case))
    placed, overflow = [], {}
    for offset, word in words:
        offset += shift
        if offset > PAGE - 4:
            overflow.setdefault(offset // PAGE, bytearray(PAGE))[
                offset % PAGE:offset % PAGE + 4] = word.to_bytes(4, "little")
        else:
            placed.append([offset, f"{word:08x}"])
    new_program = {"entry": entry + shift, "words": placed}
    for state in ("initial", "expected"):
        values = result[state]
        for name, text in values.items():
            value = int(text, 16)
            if name == "pc" or (name == "x30" and (links(entry_word) or
                                                   in_retired_code(value))):
                values[name] = hex64(moved(value))
            elif (name == "sp" or (name[0] == "x" and name[1:].isdigit())) and \
                    in_retired_code(value) and name != "x30":
                raise Unplaceable(f"{name} holds a code address")
    if "fault" in result:
        address = int(result["fault"]["address"], 16)
        result["fault"]["address"] = hex64(moved(address))
    for region in result.get("memory", []):
        if pc_relative:
            region["address"] = hex64(int(region["address"], 16) + shift)
    for page, data in sorted(overflow.items()):
        digest = hashlib.sha256(bytes(data)).hexdigest()
        new_blobs[digest] = json.dumps(
            {"blob": digest, "zlib": zlib.compress(bytes(data), 9).hex()},
            sort_keys=True)
        result.setdefault("memory", []).append({
            "address": hex64(CODE_BASE + page * PAGE), "size": PAGE,
            "flags": CODE_REGION_FLAGS, "blob": digest, "expected": [],
            "tags": [], "expected_tags": []})
    for region in result.get("memory", []):
        start = int(region["address"], 16)
        if start < CODE_BASE + PAGE and CODE_BASE < start + region["size"]:
            raise Unplaceable("a memory region overlaps the code page")
    result["ordinals"] = [0, 0]
    return new_program, result


# --------------------------------------------------------------------------
# The step


def entry_word(record: dict, case: dict) -> int:
    program = record["programs"][case["program"]]
    return dict((offset, int(word, 16))
                for offset, word in program["words"])[program["entry"]]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    options = parser.parse_args()

    lines = options.dump.read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    blob_lines: dict[str, str] = {}
    records: list[dict] = []
    for line in lines[1:]:
        if not line.strip():
            continue
        item = json.loads(line)
        if "blob" in item:
            blob_lines[item["blob"]] = line
        else:
            records.append(item)

    executed = {entry_word(record, case)
                for record in records if "4k" in record["profiles"]
                for case in record["cases"]}
    kept: list[dict] = []
    dropped_records = dropped_cases = 0
    for record in records:
        if "4k" not in record["profiles"]:
            dropped_records += 1
            dropped_cases += len(record["cases"])
        else:
            if record["profiles"] != ["4k"]:
                record["profiles"] = ["4k"]
            for case in record["cases"]:
                case["ordinals"] = [0, case["ordinals"][1]]
            kept.append(record)
    by_name = {record["record"]: record for record in kept}

    new_blobs: dict[str, str] = {}
    carried = []
    for record in records:
        if "4k" in record["profiles"]:
            continue
        for case in record["cases"]:
            word = entry_word(record, case)
            if word in executed:
                continue
            try:
                program, relocated = relocate(record, case, new_blobs)
            except Unplaceable as error:
                fail(f"{record['record']}: {case['name']}: {error}")
            executed.add(word)
            name = record["record"].removesuffix("/16k") + "/4k"
            target = by_name.get(name)
            if target is None:
                target = {key: value for key, value in record.items()
                          if key not in ("cases", "programs")}
                target.update(record=name, profiles=["4k"], programs=[],
                              cases=[])
                by_name[name] = target
                kept.append(target)
            elif any(target.get(key) != record.get(key)
                     for key in ("producer", "site", "options", "svl")):
                fail(f"{name}: differs from {record['record']} in its harness")
            if program in target["programs"]:
                index = target["programs"].index(program)
            else:
                index = len(target["programs"])
                target["programs"].append(program)
            relocated["program"] = index
            target["cases"].append(relocated)
            carried.append(word)

    used = {region["blob"] for record in kept for case in record["cases"]
            for region in case.get("memory", [])}
    blob_lines.update(new_blobs)
    missing = used - set(blob_lines)
    if missing:
        fail(f"{len(missing)} memory blobs are missing from the dump")
    header["meta"].get("exports", {}).pop("16k", None)
    header["meta"]["retired_16k"] = {
        "script": SCRIPT,
        "sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
        "dropped_records": dropped_records, "dropped_cases": dropped_cases,
        "carried_cases": len(carried),
    }
    with options.output.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for digest in sorted(used):
            sink.write(blob_lines[digest] + "\n")
        for record in kept:
            sink.write(json.dumps(record, sort_keys=True) + "\n")
    print(f"dropped {dropped_records} records ({dropped_cases} cases), "
          f"carried {len(carried)} cases for entry words only they execute")
    return 0


if __name__ == "__main__":
    sys.exit(main())
