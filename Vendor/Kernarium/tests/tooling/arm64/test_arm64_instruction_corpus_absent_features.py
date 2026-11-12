#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus against its absent-feature rewrite.

build-unit-corpus.py --check verifies that the pack records the current
absent-feature-corpus.py, its feature list, and its failing list, and the
pack encoder rejects the removed features' state fields, harness options,
events, validators, allocation tags, FPCR.EBF, and FEAT_AFP controls without
the native requirement. This test checks the rest of the rewrite's contract
on the generated pack:

- no case exercises a removed feature, and no case that does not expect the
  undefined-instruction trap holds one of its encodings;
- no case keeps a FEAT_CSSC or FEAT_MOPS host gate;
- no case needs FEAT_AFP natively without setting one of its controls, and
  the cases that pin the controls RES0 are exactly those of without_afp
  records;
- every arm64-absent-<feature> record is the rewrite's, every
  arm64-absent-mte-tbi case is the TBI variant of a kept case, and the
  rewrite's addition counts match those records.

The rewrite's drop, strip and replacement counts describe the export it read,
which the repository does not keep, so they are not checked beyond bounding
the replacements: the retirement of the 16 KiB profile removed some. The
native SME records that sme-native-corpus.py appends after the rewrite
exercise FEAT_SME by design, and so do the SM = 0 vector-length records of
sm0-vl-corpus.py and the top-byte-ignore equivalence records of
tbi-memory-corpus.py, which rerun the native SME cases with a tagged base;
arm64-instruction-corpus-sme-native, arm64-instruction-corpus-sm0-vl and
arm64-instruction-corpus-tbi-equivalence check them.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import collections
import functools
import importlib.util
import json
import pathlib
import re
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
REWRITE = REPOSITORY / "tools" / "arm64-tests" / "absent-feature-corpus.py"
TBI_NAME = re.compile(r"^(.*) with TBI tag 0x([0-9a-f]{2})$")
# The producers of tools/arm64-tests/sme-native-corpus.py,
# sm0-vl-corpus.py and the equivalence section of tbi-memory-corpus.py.
NATIVE_PRODUCERS = ("arm64-sme-native", "arm64-sm0-vl",
                    "arm64-tbi-equivalence")


def load_rewrite():
    spec = importlib.util.spec_from_file_location("absent_feature_corpus",
                                                  REWRITE)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def words_of(program: dict) -> tuple[int, ...]:
    return tuple(int(word, 16) for _offset, word in program["words"])


def entry_of(program: dict) -> int:
    return int(dict(program["words"])[program["entry"]], 16)


def encoding_owner(rewriter):
    """rewriter.encoding_feature, with its patterns indexed by the top
    encoding byte as build-unit-corpus.py's Classifier indexes its own."""
    by_top = [[(pattern, name) for pattern, name in rewriter.encodings
               if pattern.admits_top_byte(top)] for top in range(256)]

    @functools.cache
    def owner(word: int) -> str | None:
        return next((name for pattern, name in by_top[word >> 24]
                     if pattern.matches(word)), None)
    return owner


def check(rewrite, records: list[dict], step: dict) -> str:
    """Checks dumped records against the rewrite's contract; returns a
    summary or raises SystemExit naming the first violation."""
    rewriter = rewrite.Rewriter(tuple(step["features"]))
    encoding_feature = encoding_owner(rewriter)
    gates = set(rewrite.HOST_GATES)
    authored: list[dict] = []
    kept: dict[tuple, list[tuple[dict, dict]]] = collections.defaultdict(list)
    count = 0
    for item in records:
        if item["producer"] in NATIVE_PRODUCERS:
            continue
        if item["producer"].startswith("arm64-absent-"):
            authored.append(item)
            continue
        pinned = rewrite.WITHOUT_AFP in item["options"]
        for case in item["cases"]:
            program = item["programs"][case["program"]]
            words = words_of(program)
            where = f"{item['record']}: {case['name']}"
            feature = rewriter.exercised_feature(item, case)
            if feature is not None:
                raise SystemExit(f"{where}: exercises removed {feature}")
            if case["event"] != "illegal-instruction":
                feature = next((owner for owner in map(
                    encoding_feature, words) if owner), None)
                if feature is not None:
                    raise SystemExit(f"{where}: holds a {feature} encoding "
                                     "without expecting it to be undefined")
            if gates & set(case["native"]):
                raise SystemExit(f"{where}: keeps a host-gate requirement")
            # afp_rule rewrites only the native requirement.
            rule = rewrite.afp_rule(dict(case, native=list(case["native"])),
                                    entry_of(program))
            if rule in ("added", "gate"):
                raise SystemExit(f"{where}: FEAT_AFP requirement does not "
                                 "match its FPCR controls")
            if (rule == "pin") != pinned:
                raise SystemExit(f"{where}: FEAT_AFP RES0 pin is "
                                 f"{'outside' if rule == 'pin' else 'in'} "
                                 "a without_afp record")
            kept[(case["name"], words)].append((item, case))
            count += 1

    expected = {}
    for feature in step["features"]:
        for record in rewrite.undefined_records(feature):
            expected[record["record"]] = record
    replacements = [item for item in authored
                    if item["producer"] == rewrite.REPLACEMENT_PRODUCER]
    others = {item["record"]: item for item in authored
              if item["producer"] != rewrite.REPLACEMENT_PRODUCER}
    if others != expected:
        names = sorted(set(others) ^ set(expected)) or sorted(
            name for name in expected if others[name] != expected[name])
        raise SystemExit(f"{names[0]}: not the rewrite's record")
    added = sum(len(item["cases"]) for item in expected.values())
    if added != step["added_cases"]:
        raise SystemExit(f"{added} absent-feature cases, but the rewrite "
                         f"counts {step['added_cases']}")

    replaced = 0
    for item in replacements:
        for case in item["cases"]:
            program = item["programs"][case["program"]]
            match = TBI_NAME.match(case["name"])
            candidates = [] if match is None else \
                kept[(match.group(1), words_of(program))]
            if not any(
                    (record["profiles"], record["options"]) ==
                    (item["profiles"], item["options"]) and
                    rewrite.tbi_variant(original, entry_of(program),
                                        int(match.group(2), 16)) ==
                    dict(case, program=original["program"])
                    for record, original in candidates):
                raise SystemExit(f"{item['record']}: {case['name']}: not the "
                                 "TBI variant of a kept case")
            replaced += 1
    # The rewrite counts the export's replacements; those of the retired
    # 16 KiB profile are gone from the pack.
    if replaced > step["tbi_replacement_cases"]:
        raise SystemExit(f"{replaced} TBI replacement cases, but the rewrite "
                         f"counts {step['tbi_replacement_cases']}")
    return (f"absent-feature rewrite: {count} kept cases hold no removed "
            f"feature; {added} absent-feature and {replaced} TBI replacement "
            "cases are the rewrite's")


def main() -> int:
    rewrite = load_rewrite()
    builder = rewrite.corpus
    corpus, tokens = builder.decode_pack(
        pathlib.Path(sys.argv[1]).read_bytes())
    records = [item for item in map(json.loads,
                                    builder.dump_corpus(corpus, tokens))
               if "cases" in item]
    print(check(rewrite, records, corpus.meta["absent_features"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
