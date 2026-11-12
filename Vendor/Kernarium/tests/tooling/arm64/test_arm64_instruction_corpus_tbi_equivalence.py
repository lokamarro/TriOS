#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus's top-byte-ignore equivalence records.

With top-byte ignore the hardware ignores the top byte of every data address,
and the virtual CPU does whenever the host passes --guest-top-byte-ignore.
tools/arm64-tests/tbi-memory-corpus.py reruns existing memory cases with a
tagged base register, so their goldens are the source cases'. This test
re-derives the contract from the pack and the current script:

- provenance.json names the script and the case counts;
- the helpers agree with instruction words and values written out here by
  hand: the form of a word, the distance its address adds to the base, and
  the sum that carries into the tag byte;
- every case of the arm64-tbi-equivalence records is the script's derivation
  of some source case of the same record options and SVL in the pack, by
  the variant and tag its name gives; and
- every case keeps the shapes the contract names: a case with top-byte
  ignore on has a tagged base and completes as its source did or faults at an
  address with a clear top byte, and a control with it off faults at the
  tagged pointer itself.

build-unit-corpus.py --check already refuses a pack whose recorded script
hash is stale.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import collections
import hashlib
import importlib.util
import json
import pathlib
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


def check_helpers(gen) -> None:
    """The script's decoding against words written out here."""
    forms = (
        # (word, kind, base register, stores)
        (0xF940_0444, "unsigned", 2, False),     # ldr x4, [x2, #8]
        (0xF800_0044, "immediate", 2, True),     # stur x4, [x2]
        (0xF800_0444, "immediate", 2, True),     # str x4, [x2], #0
        (0xF865_7844, "register", 2, False),     # ldr x4, [x2, x5, lsl #3]
        (0xA9BF_0C44, "pair", 2, True),          # stp x4, x3, [x2, #-16]!
        (0x4C40_7040, "structure", 2, False),    # ld1 {v0.16b}, [x2]
        (0x4C9F_7040, "structure", 2, True),     # st1 {v0.16b}, [x2], #16
        (0xD50B_7424, "zva", 4, True),           # dc zva, x4
    )
    for word, kind, base, stores in forms:
        form = gen.base_form(word)
        if form is None or form.kind != kind or form.base != base or \
                form.stores != stores:
            raise SystemExit(f"{word:08x}: not the {kind} form on x{base}")
    # Words that are not ours: LDADD, LDAR, LDXR, CASAL, LDR (literal), PRFM
    # (immediate), LDAPUR and STGP.
    for word in (0xF823_0044, 0xC8DF_FC44, 0xC85F_7C44, 0xC8E3_FC44,
                 0x5800_0044, 0xF980_0440, 0xD940_0044, 0x6900_0C44):
        if gen.base_form(word) is not None:
            raise SystemExit(f"{word:08x}: is not a source form")
    # Distances: LDR X4,[X2,#8]; LDUR X4,[X2,#-3]; LDP X4,X3,[X2,#-16]!;
    # LDP X4,X3,[X2],#16; LD1 {V0.16B},[X2]; LDR X4,[X2,X5,LSL #3] with
    # X5 = 5; LDR W4,[X2,W5,SXTW] with W5 = -2.
    fill = {"x5": "0x0000000000000005"}
    negative = {"x5": "0x00000000fffffffe"}
    for word, initial, reach in (
            (0xF940_0444, {}, 8),
            (0xF81F_D044, {}, -3),
            (0xA9FF_0C44, {}, -16),
            (0xA8C1_0C44, {}, 0),
            (0x4C40_7040, {}, 0),
            (0xF865_7844, fill, 40),
            (0xB865_C844, negative, -2)):
        form = gen.base_form(word)
        if form is None or gen.displacement(word, form, initial) != reach:
            raise SystemExit(f"{word:08x}: the distance is not {reach}")
    # A carry into the tag byte: a base a page below the top of the 56-bit
    # range and an index that wraps the sum round to an address.
    base = 0x00FF_FFFF_FFFF_F000
    address = 0x5100_0800
    index = address + 0x1000
    total = (0x5A << 56) + base + index & gen.MASK64
    if total & gen.LOW56 != address or total >> 56 != 0x5B:
        raise SystemExit("the script's carry arithmetic is wrong")
    if gen.LOW56 - 0xFFF != base:
        raise SystemExit("the script's carry base is not a page below")


def without_invalid_bit(case: dict) -> dict:
    return {key: value for key, value in case.items()
            if key != "invalid_encoding"}


def program_of(record: dict, case: dict) -> list:
    return record["programs"][case["program"]]["words"]


def main() -> int:
    gen = load_generator()
    builder = gen.corpus
    corpus, tokens = builder.decode_pack(pathlib.Path(sys.argv[1]).read_bytes())
    records = [item for item in map(json.loads,
                                    builder.dump_corpus(corpus, tokens))
               if "cases" in item]
    check_helpers(gen)
    ours = [item for item in records if item["producer"] == gen.EQUIVALENCE]
    if not ours:
        raise SystemExit("no arm64-tbi-equivalence record")
    meta = corpus.meta.get("tbi_memory", {})
    if meta.get("script") != gen.SITE or meta.get("sha256") != \
            hashlib.sha256(GENERATOR.read_bytes()).hexdigest():
        raise SystemExit("provenance.json describes another script")
    # The sources, by name.
    sources: dict[str, list] = collections.defaultdict(list)
    for record in records:
        if record["producer"] == gen.EQUIVALENCE or \
                "4k" not in record["profiles"]:
            continue
        for case in record["cases"]:
            source = gen.as_source(record, case)
            if source is not None:
                sources[case["name"]].append(source)
    total = collections.Counter()
    for record in ours:
        if record["profiles"] != ["4k"] or record["site"] != gen.SITE:
            raise SystemExit(f"{record['record']}: not a 4 KiB record of the "
                             "script")
        names = set()
        for case in record["cases"]:
            if case["name"] in names:
                raise SystemExit(f"{case['name']}: two cases share a name")
            names.add(case["name"])
            match = gen.NAME.match(case["name"])
            if match is None:
                raise SystemExit(f"{case['name']}: not a derived name")
            variant = match.group("variant")
            tag = int(match.group("tag"), 16)
            if variant not in ("base",) + gen.VARIANTS:
                raise SystemExit(f"{case['name']}: unknown variant")
            found = False
            for source in sources[match.group("source")]:
                if source.record["options"] != record["options"] or \
                        source.record.get("svl", 0) != record.get("svl", 0):
                    continue
                for other in gen.TAGS:
                    derived = gen.derive(source, variant, tag, other)
                    if derived is None:
                        continue
                    words, want = derived
                    want["program"] = case["program"]
                    if [int(word, 16) for _offset, word in
                            program_of(record, case)] != words:
                        continue
                    stem = gen.NAME.match(want["name"]).group("source")
                    got = without_invalid_bit(case)
                    want = without_invalid_bit(want)
                    got["name"] = want["name"] = stem
                    if got == want:
                        found = True
                        break
                if found:
                    break
            if not found:
                raise SystemExit(f"{case['name']}: not the script's "
                                 "derivation of a source in the pack")
            initial = case["initial"]
            if variant == "control" or variant == "sp control":
                if "tbi_enabled" in initial or case["event"] != \
                        "memory-fault":
                    raise SystemExit(f"{case['name']}: a control must fault "
                                     "with top-byte ignore off")
                address = int(case["fault"]["address"], 16)
                if address >> 56 != tag:
                    raise SystemExit(f"{case['name']}: a control must fault "
                                     "at the tagged pointer")
            else:
                if int(initial.get("tbi_enabled", "0x0"), 16) != 1:
                    raise SystemExit(f"{case['name']}: top-byte ignore is "
                                     "not on")
                if case["event"] == "memory-fault" and int(
                        case["fault"]["address"], 16) >> 56:
                    raise SystemExit(f"{case['name']}: a fault at a tagged "
                                     "address with top-byte ignore on")
            total[variant] += 1
            total["cases"] += 1
    summary = meta.get("equivalence", {})
    if summary.get("cases") != total["cases"] or \
            summary.get("base_cases") != total["base"] or \
            summary.get("tbi_off_cases") != total["control"] or \
            summary.get("sp_cases") != total["sp"] + total["sp control"] or \
            summary.get("index_cases") != total["index"] + total["carry"]:
        raise SystemExit("provenance.json counts another set of cases")
    missing = [variant for variant in ("base",) + gen.VARIANTS
               if not total[variant]]
    if missing:
        raise SystemExit("no case for " + ", ".join(missing))
    print(f"top-byte-ignore equivalence records: {total['cases']} cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
