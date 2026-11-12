#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the A64 instruction corpus's invalid-encoding bits.

tests/unit/arm64/corpus/README.md defines the bit: set when no virtual-CPU
configuration decodes the word as an instruction, clear for a valid encoding
that traps for its exception level, mode, or state, or for a feature that
another configuration implements. tools/arm64-tests/invalid_encoding.py
decides it from the checked-in decode partitions, allocation table, and
SVE/SME encoding table, and invalid-encoding-corpus.py, the last step of the
chain, writes its answers into the pack. This test requires:

- every illegal-instruction case that retires nothing and whose word those
  tables settle to carry the classifier's bit (a word they leave open keeps
  its producer's bit, which `--llvm` checks);
- the classifier's feature lists to cover every allocation-table feature
  token and the absent-feature script's features, so a new feature has to be
  decided; and
- provenance.json to record the step.

`--llvm` adds the cross-check that the tables cannot make: a case whose bit
is clear names a valid encoding, so the pinned LLVM MC must decode its word
with every feature enabled. It skips (exit 77) where that decoder is not
installed.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import json
import pathlib
import subprocess
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
TOOLS = REPOSITORY / "tools" / "arm64-tests"
SKIP = 77


def load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def records_of(builder, pack: pathlib.Path):
    corpus, tokens = builder.decode_pack(pack.read_bytes())
    items = [json.loads(line) for line in builder.dump_corpus(corpus, tokens)]
    return corpus.meta, [item for item in items if "record" in item]


def check_features(ie) -> list[str]:
    """The classifier's feature lists against the tables and the engines."""
    problems = []
    decided = ie.OMITTED_TOKENS | ie.IMPLEMENTED_TOKENS
    unknown = sorted(token for token in ie.classifier().tokens - decided
                     if not token.startswith(("sve", "sme", "ssve")))
    if unknown:
        problems.append("allocation-table feature tokens that "
                        "invalid_encoding.py decides neither implemented "
                        "nor omitted: " + ", ".join(unknown))
    # The virtual CPU executes SVE only in streaming mode, so every SVE
    # feature of the encoding table is unimplemented outside the SME rows.
    sve = frozenset(token for token in ie.sme_table.FEATURES
                    if token.startswith("sve"))
    if sve - ie.UNIMPLEMENTED_PROFILE_FEATURES:
        problems.append("SVE features of the encoding table that "
                        "invalid_encoding.py treats as implemented: " +
                        ", ".join(sorted(
                            sve - ie.UNIMPLEMENTED_PROFILE_FEATURES)))
    both = sorted(ie.OMITTED_TOKENS & ie.IMPLEMENTED_TOKENS)
    if both:
        problems.append("tokens listed as implemented and omitted: " +
                        ", ".join(both))
    absent = load("absent_feature_corpus",
                  TOOLS / "absent-feature-corpus.py")
    # FEAT_SME and FEAT_SVE are two-sided, and FEAT_EBF16 has no encodings.
    removed = [feature for name, feature in absent.FEATURES.items()
               if name not in ("sme", "sve", "ebf16")]
    for feature in removed:
        missing = sorted(feature.tokens - ie.OMITTED_TOKENS)
        if missing:
            problems.append("the absent-feature script removes tokens that "
                            "invalid_encoding.py does not omit: " +
                            ", ".join(missing))
        missing = sorted(feature.conditions - ie.OMITTED_CONDITIONS)
        if missing:
            problems.append("the absent-feature script removes conditions "
                            "that invalid_encoding.py does not omit: " +
                            ", ".join(missing))
    return problems


# (word, record options, invalid): one word of each rule of the classifier.
EXAMPLES = (
    # ADR (vectors) is non-streaming SVE, which no configuration executes;
    # RDVL executes in streaming mode and, as the SM = 0 family, outside it,
    # and is a valid encoding where SME is absent.
    (0x04A0_A000, (), True),
    (0x04BF_5000, (), False),
    (0x04BF_5000, ("without_sme",), False),
    # An SVE row nothing executes, and AND (immediate) with a reserved imm13.
    (0x0406_A000, (), True),
    (0x0583_FFE0, (), True),
    # A shift by immediate with Q = 0 and a 64-bit element.
    (0x2F40_0400, (), True),
    # HVC and MSR FPMR decode, and EL0 takes the trap.
    (0xD400_0002, (), False),
    (0xD51B_4440, (), False),
    # MSR TCO, #imm (FEAT_MTE), RETAASPPC and RETAASPPCR (FEAT_PAuth_LR).
    (0xD503_409F, (), True),
    (0x5500_001F, (), True),
    (0xD65F_0BE0, (), True),
    # A branch-register word nothing allocates.
    (0xD780_0000, (), True),
)


def check_examples(ie) -> None:
    classifier = ie.classifier()
    for word, options, invalid in EXAMPLES:
        decision = classifier.decide(word, options)
        if decision is None or decision.invalid != invalid:
            raise SystemExit(
                f"{word:08x} {options}: expected "
                f"{'an invalid' if invalid else 'a valid'} encoding, got "
                f"{decision}")


def check_pack(ie, meta: dict, records: list[dict]) -> str:
    step = meta.get("invalid_encoding")
    if step is None:
        raise SystemExit("the pack records no invalid-encoding step")
    classifier = ie.classifier()
    illegal = decided = 0
    wrong: list[tuple[str, str]] = []
    for record in records:
        for case in record["cases"]:
            if case["event"] != "illegal-instruction":
                continue
            illegal += 1
            decision = classifier.decide_case(record, case)
            if decision is None:
                continue
            decided += 1
            if bool(case["invalid_encoding"]) != decision.invalid:
                word = classifier.trapping_word(record, case)
                wrong.append((record["record"], (
                    f"{case['name']} ({word:08x}): bit "
                    f"{'set' if case['invalid_encoding'] else 'clear'}, "
                    f"should be {'set' if decision.invalid else 'clear'}: "
                    f"{decision.reason}")))
    if wrong:
        by_record: dict[str, list[str]] = {}
        for name, line in wrong:
            by_record.setdefault(name, []).append(line)
        for name, lines in by_record.items():
            print(f"  {name}: {len(lines)} cases, e.g. {lines[0]}",
                  file=sys.stderr)
        raise SystemExit(
            f"{len(wrong)} of {decided} decided cases carry the wrong "
            f"invalid-encoding bit, in {len(by_record)} records")
    if (step["illegal_cases"], step["decided_cases"]) != (illegal, decided):
        raise SystemExit("provenance.json counts "
                         f"{step['illegal_cases']} illegal-instruction cases "
                         f"and {step['decided_cases']} decided; the pack "
                         f"holds {illegal} and {decided}")
    return (f"invalid-encoding bits: {decided} of {illegal} "
            "illegal-instruction cases agree with invalid_encoding.py")


def check_llvm(ie, records: list[dict]) -> str | None:
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    from a64_llvm_oracle import (PINNED_LLVM_VERSION, decoder_instructions,
                                 find_llvm_mc, llvm_version)
    try:
        tool = find_llvm_mc().resolve(strict=True)
        version = llvm_version(tool)
    except (OSError, ValueError, subprocess.SubprocessError):
        version = None
    if version != PINNED_LLVM_VERSION:
        print(f"skipped: LLVM MC {PINNED_LLVM_VERSION} is not installed")
        return None
    classifier = ie.classifier()
    clear: dict[int, str] = {}
    for record in records:
        for case in record["cases"]:
            if case["event"] == "illegal-instruction" and \
                    not case["invalid_encoding"]:
                word = classifier.trapping_word(record, case)
                if word is not None:
                    clear.setdefault(word, f"{record['record']}: "
                                           f"{case['name']}")
    words = sorted(clear)
    decoded = decoder_instructions(tool, words, mattr="+all")
    undecoded = [(word, clear[word]) for word, result in zip(words, decoded)
                 if result is None]
    if undecoded:
        for word, where in undecoded[:25]:
            print(f"  {word:08x} {where}", file=sys.stderr)
        raise SystemExit(f"{len(undecoded)} of {len(words)} words with a "
                         "clear invalid-encoding bit are invalid encodings "
                         f"to LLVM MC {version}")
    return (f"LLVM MC {version} decodes all {len(words)} words with a clear "
            "invalid-encoding bit")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pack", type=pathlib.Path)
    parser.add_argument("--llvm", action="store_true")
    options = parser.parse_args()
    ie = load("invalid_encoding", TOOLS / "invalid_encoding.py")
    meta, records = records_of(ie.corpus, options.pack)
    if options.llvm:
        message = check_llvm(ie, records)
        if message is None:
            return SKIP
        print(message)
        return 0
    problems = check_features(ie)
    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        raise SystemExit("invalid_encoding.py's feature lists are stale")
    check_examples(ie)
    print(check_pack(ie, meta, records))
    return 0


if __name__ == "__main__":
    sys.exit(main())
