#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus's top-byte-ignore memory records.

With top-byte ignore the hardware ignores the top byte of a data address, and
the virtual CPU does whenever the host passes --guest-top-byte-ignore.
tools/arm64-tests/tbi-memory-corpus.py writes the cases that reach memory
through a tagged base register: atomic read-modify-write, compare and swap,
load-acquire and store-release, RCpc and exclusive forms, from the
architecture's pseudocode. This test re-derives the contract from the current
script:

- provenance.json names the script and its case counts;
- the one arm64-tbi-memory record is the script's;
- the model agrees with values written out here by hand;
- every completed case keeps its base registers' tags, and moves only the
  bytes of its own access; and
- every fault with top-byte ignore on is at an address with a clear top byte,
  and every fault with it off is at the tagged pointer itself.

build-unit-corpus.py --check already refuses a pack whose recorded script
hash is stale.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import json
import pathlib
import re
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "tbi-memory-corpus.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("tbi_memory_corpus",
                                                  GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check_model(gen) -> None:
    """The read-modify-write results against values written out here."""
    for name, old, source, size, expected in (
            ("add", 0xFFFF_FFFF, 1, 2, 0),
            ("add", 0xFF, 0x101, 0, 0),
            ("clr", 0xFF, 0x0F, 0, 0xF0),
            ("eor", 0xA5A5, 0xFFFF, 1, 0x5A5A),
            ("set", 0x1, 0x6, 0, 0x7),
            ("smax", 0x80, 0x7F, 0, 0x7F),       # -128 against 127
            ("smin", 0x80, 0x7F, 0, 0x80),
            ("umax", 0x80, 0x7F, 0, 0x80),
            ("umin", 0x80, 0x7F, 0, 0x7F),
            ("smax", 0xFFFF_FFFF_FFFF_FFFF, 0, 3, 0),   # -1 against 0
            ("swp", 0x1122, 0x3344_5566, 1, 0x5566)):
        if gen.rmw_result(name, old, source, size) != expected:
            raise SystemExit(f"ld{name}: the script's model differs from the "
                             "hand-written expectation")
    if gen.mask(0) != 0xFF or gen.mask(2) != 0xFFFF_FFFF or \
            gen.signed(0x80, 0) != -128 or gen.signed(0x8000, 1) != -32768:
        raise SystemExit("the script's size helpers are wrong")
    # Encodings against words written out here: LDADDAL X3, X4, [X2],
    # CASAL W3, W4, [X2], CASP X4, X5, X6, X7, [X2], LDAR X4, [X2], STLRH W3,
    # [X2], LDAPR X4, [X2], LDAPURSW X4, [X2, #-3], STLUR X3, [X2, #4],
    # LDAXR W4, [X2], STLXR W5, X3, [X2], LDXP X4, X6, [X2], STXP W5, W3, W7,
    # [X2].
    for built, word in (
            (gen.rmw_word(3, 1, 1, 3, 0, 2, 4), 0xF8E3_0044),
            (gen.cas_word(2, 1, 1, 3, 2, 4), 0x88E3_FC44),
            (gen.casp_word(1, 0, 0, 4, 2, 6), 0x4824_7C46),
            (gen.ordered_word(3, True, 2, 4), 0xC8DF_FC44),
            (gen.ordered_word(1, False, 2, 3), 0x489F_FC43),
            (gen.ldapr_word(3, 2, 4), 0xF8BF_C044),
            (gen.ldapur_word(2, 2, -3, 2, 4), 0x999F_D044),
            (gen.ldapur_word(3, 0, 4, 2, 3), 0xD900_4043),
            (gen.exclusive_load_word(2, 1, 2, 4), 0x885F_FC44),
            (gen.exclusive_store_word(3, 1, 5, 2, 3), 0xC805_FC43),
            (gen.exclusive_load_pair_word(1, 0, 2, 4, 6), 0xC87F_1844),
            (gen.exclusive_store_pair_word(0, 0, 5, 2, 3, 7), 0x8825_1C43),
            (gen.branch_back(2, 5), 0x35FF_FFC5)):
        if built != word:
            raise SystemExit(f"{built:08x} is not the word {word:08x}")


def without_invalid_bits(item: dict) -> dict:
    return dict(item, cases=[{key: value for key, value in case.items()
                              if key != "invalid_encoding"}
                             for case in item["cases"]])


def check_record(gen, record: dict, meta: dict) -> int:
    item = gen.build()[0]
    if without_invalid_bits(record) != without_invalid_bits(item):
        raise SystemExit(f"{record['record']}: not the script's record")
    counts = gen.counts()
    if meta.get("script") != gen.SITE or \
            any(meta.get(key) != value for key, value in counts.items()) or \
            meta.get("sha256") != hashlib.sha256(
                GENERATOR.read_bytes()).hexdigest():
        raise SystemExit("provenance.json describes another script")
    if record["options"] != [] or record["profiles"] != ["4k"]:
        raise SystemExit(f"{record['record']}: not an optionless 4 KiB record")
    untagged = (1 << 56) - 1
    names = set()
    for case in record["cases"]:
        names.add(case["name"])
        if case["invalid_encoding"] or case["event"] not in (
                "system-call", "memory-fault"):
            raise SystemExit(f"{case['name']}: not a memory case")
        initial = case["initial"]
        base = int(initial["x2"], 16)
        tbi = int(initial.get("tbi_enabled", "0x0"), 16)
        fault = case.get("fault")
        # A monitor case holds the load's pointer in x2 and the store's in x8,
        # and either may have no tag; every other case has x2 alone.
        bases = [base]
        if "[x2] and [x8]" in case["name"]:
            bases.append(int(initial["x8"], 16))
        if tbi and (not any(address >> 56 for address in bases) or
                    "x2" in case["expected"] or "x8" in case["expected"]):
            raise SystemExit(f"{case['name']}: no base register is tagged, "
                             "or the case changes its tag")
        if case["event"] == "system-call":
            if fault is not None or not tbi or case["retired"] == 0:
                raise SystemExit(f"{case['name']}: not a completed case")
            window = case["memory"][0]
            moved = sum(len(data) // 2 for _offset, data in window["expected"])
            if moved > 16 + 7:
                raise SystemExit(f"{case['name']}: moves {moved} bytes")
        else:
            address = int(fault["address"], 16)
            if case["retired"] != 0 or case["expected"]:
                raise SystemExit(f"{case['name']}: a fault retires or "
                                 "changes state")
            if tbi and address >> 56:
                raise SystemExit(f"{case['name']}: a tagged fault address "
                                 "with top-byte ignore on")
            if not tbi and address != base:
                raise SystemExit(f"{case['name']}: with top-byte ignore off "
                                 "the fault is at the pointer")
            offset = re.search(r"#(-?\d+)\]", case["name"])
            effective = (base & untagged) + (int(offset.group(1)) if offset
                                             else 0)
            if tbi and address != effective:
                raise SystemExit(f"{case['name']}: the fault is not at the "
                                 "untagged address")
    if len(names) != len(record["cases"]):
        raise SystemExit("two cases share a name")
    families = ("ldadd", "ldclr", "ldeor", "ldset", "ldsmax", "ldsmin",
                "ldumax", "ldumin", "swp", "cas ", "casp", "ldar", "stlr",
                "ldapr", "ldapur", "stlur", "ldxr", "ldaxr", "stxr", "stlxr",
                "ldxp", "ldaxp", "stxp", "stlxp", "across a block",
                "top-byte ignore off", "unmapped page", "read-only page")
    text = "\n".join(names)
    for family in families:
        if family not in text:
            raise SystemExit(f"no case for {family}")
    return len(record["cases"])


def main() -> int:
    gen = load_generator()
    builder = gen.corpus
    corpus, tokens = builder.decode_pack(pathlib.Path(sys.argv[1]).read_bytes())
    records = [item for item in map(json.loads,
                                    builder.dump_corpus(corpus, tokens))
               if "cases" in item]
    check_model(gen)
    ours = [item for item in records if item["producer"] == gen.PRODUCER]
    if len(ours) != 1:
        raise SystemExit(f"{len(ours)} arm64-tbi-memory records, expected one")
    cases = check_record(gen, ours[0], corpus.meta.get("tbi_memory", {}))
    print(f"top-byte-ignore memory records: {cases} cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
