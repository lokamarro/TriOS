#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus's SM = 0 family records.

The virtual CPU, with FEAT_SME and never FEAT_SVE, executes CNT[BHWD], RDVL,
ADDVL, ADDPL, the scalar INC[BHWD] and DEC[BHWD], and LDR and STR of Z and P
registers outside streaming mode for a constant vector length of 128 bits, a
deliberate departure from the architecture
(darwin/cpu/arm64/detail/sm0_vector_length.hpp). An M4 traps those words, so
their results are the project's, and tools/arm64-tests/sm0-vl-corpus.py
writes them from the instructions' pseudocode. This test re-derives the
contract from the current script and encoding table:

- provenance.json names the script and the table, and its case counts;
- the one arm64-sm0-vl record is the script's, and
  every row of the family has a completed case in it, the spill rows
  with page crossings, unmapped and read-only pages, tagged pointers and
  misaligned SPs;
- the model agrees with values written out here by hand;
- the control cases, words that stay UNDEFINED beside the spills, hold no
  family word;
- no arm64-sme-native case holds a family word at PSTATE.SM = 0, whose
  results are the M4's trap; and
- the family words are undefined only where SME is not implemented: they
  are in the arm64-absent-sve record under without_sme, and in no other
  record that expects them undefined, so no other SVE word has moved.

build-unit-corpus.py --check already refuses a pack whose recorded script or
table hash is stale.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import json
import pathlib
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / "sm0-vl-corpus.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("sm0_vl_corpus", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def registers_after(gen, word: int) -> dict[int, int]:
    """The registers a word changes, by number (31 is SP)."""
    after = gen.execute(word)
    return {number: value for number, value in enumerate(after.value)
            if value != gen.corpus.FILL_REGISTERS[number]}


def check_model(gen) -> None:
    fill = gen.corpus.FILL_REGISTERS
    mask = (1 << 64) - 1
    # CNTB X0 (ALL) is 16, CNTD X1 is 2, CNTW X2, VL3, MUL #2 is 6, a
    # pattern that counts more than there is counts nothing, MUL3 of 16
    # bytes is 15, and a reserved pattern counts nothing.
    expectations = [
        (gen.cnt(0, 31, 1, 0), {0: 16}),
        (gen.cnt(3, 31, 1, 1), {1: 2}),
        (gen.cnt(2, 3, 2, 2), {2: 6}),
        (gen.cnt(3, 3, 1, 3), {3: 0}),  # VL3 of two doublewords
        (gen.cnt(0, 30, 1, 4), {4: 15}),  # MUL3
        (gen.cnt(0, 29, 16, 5), {5: 256}),  # MUL4, MUL #16
        (gen.cnt(1, 0, 1, 6), {6: 8}),  # POW2 of eight halfwords
        (gen.cnt(2, 0, 1, 7), {7: 4}),
        (gen.cnt(0, 14, 1, 8), {8: 0}),  # a reserved pattern
        (gen.cnt(0, 9, 1, 9), {9: 16}),  # VL16
        (gen.cnt(0, 10, 1, 10), {10: 0}),  # VL32 of 16 bytes
        (gen.inc_dec(False, 0, 31, 1, 11), {11: (fill[11] + 16) & mask}),
        (gen.inc_dec(True, 3, 31, 3, 12), {12: (fill[12] - 6) & mask}),
        (gen.inc_dec(False, 0, 31, 1, 31), {}),  # XZR discards
        (gen.inc_dec(False, 0, 14, 1, 13), {}),  # a reserved pattern
        (gen.rdvl(1, 14), {14: 16}),
        (gen.rdvl(-32, 15), {15: (-512) & mask}),
        (gen.rdvl(31, 31), {}),  # XZR discards
        (gen.add_length(False, 1, 2, 3), {1: (fill[2] + 48) & mask}),
        (gen.add_length(True, 1, 2, 3), {1: (fill[2] + 6) & mask}),
        (gen.add_length(False, 31, 31, -1), {31: (fill[31] - 16) & mask}),
        (gen.add_length(True, 5, 31, 31), {5: (fill[31] + 62) & mask}),
    ]
    for word, changed in expectations:
        if registers_after(gen, word) != changed:
            raise SystemExit(f"{word:08x}: the script's model differs from "
                             "the hand-written expectation")


def check_spill_model(gen) -> None:
    """The spill words against values written out here by hand."""
    fill = gen.corpus.FILL_VECTORS
    for word, fields in (
            (0xE580_43EF, (True, False, 15, 31, 0)),          # str z15, [sp]
            (0x8581_5FE8, (False, False, 8, 31, 15)),         # ldr z8, [sp, #15, mul vl]
            (0x85BD_57A0, (False, False, 0, 29, -19)),        # ldr z0, [x29, #-19, mul vl]
            (0xE581_17E6, (True, True, 6, 31, 13)),           # str p6, [sp, #13, mul vl]
            (0x8580_03E4, (False, True, 4, 31, 0))):          # ldr p4, [sp]
        spill = gen.decode_spill(word)
        if spill != gen.Spill(*fields) or spill.word != word:
            raise SystemExit(f"{word:08x}: the script decodes {spill}")
    if gen.Spill(False, False, 0, 29, -19).offset != -304 or \
            gen.Spill(True, True, 6, 31, 13).offset != 26:
        raise SystemExit("MUL VL does not scale by the register's size")
    mid, wide = gen.MIDDLE, ((gen.DATA, gen.WIDE, gen.READ_WRITE),)

    def run(spill, address, regions=wide, sp=None):
        return gen.run_scenario(gen.Scenario(spill, address, regions, sp=sp))

    # STR Z15, [SP] writes the vector's 16 bytes and nothing else.
    event, fault, regions, changes = run(gen.Spill(True, False, 15, 31, 0),
                                         mid, sp=mid)
    offset = mid - gen.DATA
    if event != "system-call" or fault or changes or \
            bytes(regions[0].data[offset:offset + 16]) != fill[15] or \
            regions[0].data[offset + 16:] != regions[0].initial[offset + 16:]:
        raise SystemExit("STR Z does not store the vector")
    # LDR Z8 fills V8 from the 16 bytes at SP + 15 * 16.
    event, fault, regions, changes = run(gen.Spill(False, False, 8, 31, 15),
                                         mid + 240, sp=mid)
    if changes != {"v8": regions[0].initial[offset + 240:offset + 256].hex()}:
        raise SystemExit("LDR Z does not load the vector")
    # STR P writes two zero bytes; LDR P changes nothing.
    event, fault, regions, changes = run(gen.Spill(True, True, 6, 31, 13),
                                         mid + 26, sp=mid)
    if bytes(regions[0].data[offset + 26:offset + 28]) != bytes(2) or \
            regions[0].initial[offset + 26:offset + 28] == bytes(2) or changes:
        raise SystemExit("STR P does not store zeros")
    event, fault, regions, changes = run(gen.Spill(False, True, 4, 31, 0),
                                         mid, sp=mid)
    if changes or regions[0].data != regions[0].initial:
        raise SystemExit("LDR P changes state")
    # A vector access across a page that does not translate faults at the
    # first byte that does not, and a store writes nothing; a read-only page
    # faults a store only; a misaligned SP faults at SP.
    first = (gen.DATA, gen.PAGE, gen.READ_WRITE)
    for store in (False, True):
        event, fault, regions, changes = run(
            gen.Spill(store, False, 3, 1, 0), gen.BOUNDARY - 8, (first,))
        access = "write" if store else "read"
        if event != "memory-fault" or fault != (gen.BOUNDARY, access) or \
                regions[0].data != regions[0].initial:
            raise SystemExit("a vector access into an unmapped page")
        event, fault, regions, changes = run(
            gen.Spill(store, True, 3, 1, 0), gen.BOUNDARY - 1, (first,))
        if event != "memory-fault" or fault != (gen.BOUNDARY, access):
            raise SystemExit("a predicate access into an unmapped page")
        event, fault, _regions, _changes = run(
            gen.Spill(store, False, 3, 1, 0), mid,
            ((gen.DATA, gen.WIDE, gen.READ_ONLY),))
        if (event == "memory-fault") != store or \
                store and fault != (mid, "write"):
            raise SystemExit("a read-only page")
        event, fault, _regions, _changes = run(
            gen.Spill(store, False, 3, 31, 1), mid + 8 + 16, sp=mid + 8)
        if event != "memory-fault" or fault != (mid + 8, access):
            raise SystemExit("a misaligned SP")


def without_invalid_bits(item: dict) -> dict:
    """The record without its cases' invalid-encoding bits, which the last
    step of the chain (invalid-encoding-corpus.py) owns: it sets the bit of
    the control words that no instruction allocates."""
    return dict(item, cases=[{key: value for key, value in case.items()
                              if key != "invalid_encoding"}
                             for case in item["cases"]])


def check_record(gen, record: dict, meta: dict) -> int:
    item = gen.record()
    if without_invalid_bits(record) != without_invalid_bits(item):
        raise SystemExit(f"{record['record']}: not the script's record")
    for case in record["cases"]:
        if case["invalid_encoding"] and case["event"] != "illegal-instruction":
            raise SystemExit(f"{case['name']}: invalid encoding, yet it "
                             "executes")
    if record["options"] != [] or record["svl"] != gen.SVL:
        raise SystemExit(f"{record['record']}: not optionless at the "
                         "profile's SVL")
    counts = gen.counts()
    if (meta.get("script"), meta.get("vector_length")) != \
            (gen.SITE, gen.VL) or \
            any(meta.get(key) != value for key, value in counts.items()) or \
            meta.get("sha256") != hashlib.sha256(
                GENERATOR.read_bytes()).hexdigest() or \
            meta.get("table", {}).get("sha256") != gen.table().sha256:
        raise SystemExit("provenance.json describes another script or table")
    completed = set()
    controls = 0
    for case in record["cases"]:
        program = record["programs"][case["program"]]
        word = int(program["words"][0][1], 16)
        if case["event"] == "illegal-instruction":
            # A control: a word that stays undefined beside the spills.
            if gen.sme_table.sm0_family_word(word) or case["retired"] != 0 or \
                    case["expected"] or "memory" in case:
                raise SystemExit(f"{case['name']}: not a control")
            controls += 1
            continue
        if not gen.sme_table.sm0_family_word(word):
            raise SystemExit(f"{case['name']}: not a family word")
        spill = gen.sme_table.sm0_spill_word(word)
        if spill != ("memory" in case):
            raise SystemExit(f"{case['name']}: memory regions and spill "
                             "words disagree")
        if case["event"] == "system-call":
            if case["retired"] != 2 or "fault" in case:
                raise SystemExit(f"{case['name']}: not a completed word")
            completed.add(gen.table().row_for(word).witness)
        elif not spill or case["event"] != "memory-fault" or \
                case["retired"] != 0 or "fault" not in case:
            raise SystemExit(f"{case['name']}: neither completed nor a "
                             "fault of a spill")
    family = {row.witness for row in gen.table().rows
              if gen.sme_table.sm0_family_word(row.witness)}
    if len(family) != 19 or completed != family:
        raise SystemExit("the record does not cover every row of the family")
    names = "\n".join(case["name"] for case in record["cases"])
    for place in ("across a page", "into an unmapped one", "from an unmapped one",
                  "into a read-only page", "through a tagged pointer",
                  "at a misaligned SP", "as the compilers emit it",
                  "ZA = 1"):
        if place not in names:
            raise SystemExit(f"no spill case {place}")
    if controls < 20 or len(record["cases"]) < 2000:
        raise SystemExit("the record lost cases")
    return len(record["cases"])


def check_others(gen, records: list[dict]) -> None:
    table = gen.sme_table
    for item in records:
        if item["producer"] == gen.PRODUCER:
            continue
        for case in item["cases"]:
            program = item["programs"][case["program"]]
            word = int(program["words"][0][1], 16)
            if not table.sm0_family_word(word):
                continue
            streaming = int(case["initial"].get("svcr", "0x0"), 16) & 1
            undefined = case["event"] == "illegal-instruction"
            where = f"{item['record']}: {case['name']}"
            if item["producer"] == "arm64-sme-native" and not streaming:
                raise SystemExit(f"{where}: an M4 result for a word the "
                                 "virtual CPU executes")
            if undefined and not streaming and (
                    item["producer"] != "arm64-absent-sve" or
                    item["options"] != ["without_sme"]):
                raise SystemExit(f"{where}: expects a family word undefined "
                                 "at SM = 0 where SME may be implemented")


def main() -> int:
    gen = load_generator()
    builder = gen.corpus
    corpus, tokens = builder.decode_pack(pathlib.Path(sys.argv[1]).read_bytes())
    records = [item for item in map(json.loads,
                                    builder.dump_corpus(corpus, tokens))
               if "cases" in item]
    check_model(gen)
    check_spill_model(gen)
    ours = [item for item in records if item["producer"] == gen.PRODUCER]
    if len(ours) != 1:
        raise SystemExit(f"{len(ours)} arm64-sm0-vl records, expected one")
    cases = check_record(gen, ours[0], corpus.meta.get("sm0_vl", {}))
    check_others(gen, records)
    print(f"SM = 0 family records: {cases} project-defined cases cover the "
          "family's 19 rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
