#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Set the invalid-encoding bit of every A64 corpus illegal-instruction case.

usage: invalid-encoding-corpus.py DUMP OUTPUT

The bit follows the convention of tests/unit/arm64/corpus/README.md and is
decided by invalid_encoding.py from the checked-in decode partitions,
allocation table, and SVE/SME encoding table: set when no virtual-CPU
configuration decodes the word as an instruction, clear for a valid encoding
that traps for its exception level, mode, or state, or for a feature that
another configuration implements.

The producers disagree on those cases: the exported checks set the bit
wherever they expect an undefined-instruction trap, and the maintained
scripts each decide for their own records. This is the last step of the
chain. It takes the trapping word of every illegal-instruction case that
retires nothing, which is the entry word, and writes the classifier's answer
into the case, so no producer needs to compute the bit. A word the tables do
not settle keeps its recorded bit: a reserved operand value or a CONSTRAINED
UNPREDICTABLE combination inside an allocated encoding, which a producer sets,
and a named system instruction that EL0 does not execute, which it clears.
The opt-in LLVM check of arm64-instruction-corpus-invalid-encoding covers
those with a second decoder.

Run it on the dump of the 16 KiB retirement step and --repack the result.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import hashlib
import importlib.util
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent


def _load(name: str, filename: str):
    module = sys.modules.get(name)
    if module is None:
        spec = importlib.util.spec_from_file_location(name, HERE / filename)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return module


corpus = _load("build_unit_corpus", "build-unit-corpus.py")
invalid_encoding = _load("invalid_encoding", "invalid_encoding.py")

SITE = "tools/arm64-tests/invalid-encoding-corpus.py"
REPOSITORY = HERE.parents[1]


def sha256_file(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tables() -> list[dict]:
    """The checked-in tables the classifier reads, with their hashes."""
    return [{"file": path.relative_to(REPOSITORY).as_posix(),
             "sha256": sha256_file(path)}
            for path in (corpus.ALLOCATIONS, corpus.PARTITIONS,
                         invalid_encoding.sme_table.TABLE)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    options = parser.parse_args()
    classifier = invalid_encoding.classifier()
    lines = options.dump.read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    out = [lines[0]]
    illegal = decided = 0
    changed: collections.Counter = collections.Counter()
    for line in lines[1:]:
        if not line.strip():
            continue
        item = json.loads(line)
        touched = False
        if "blob" not in item:
            for case in item["cases"]:
                if case["event"] != "illegal-instruction":
                    continue
                illegal += 1
                decision = classifier.decide_case(item, case)
                if decision is None:
                    continue
                decided += 1
                if bool(case["invalid_encoding"]) != decision.invalid:
                    case["invalid_encoding"] = decision.invalid
                    changed[("set" if decision.invalid else "clear",
                             item["producer"])] += 1
                    touched = True
        out.append(json.dumps(item, sort_keys=True) if touched else line)
    header["meta"]["invalid_encoding"] = {
        "script": SITE,
        "sha256": sha256_file(pathlib.Path(__file__)),
        "classifier": {"file": invalid_encoding.SITE,
                       "sha256": sha256_file(
                           pathlib.Path(invalid_encoding.__file__))},
        "tables": tables(),
        # What the step changed depends on the bits its input held, which a
        # carried hardware step leaves as the earlier pack had them, so the
        # counts here are the ones the input cannot change.
        "illegal_cases": illegal, "decided_cases": decided,
    }
    out[0] = json.dumps(header, sort_keys=True)
    with options.output.open("w", encoding="utf-8") as sink:
        for line in out:
            sink.write(line + "\n")
    print(f"{illegal} illegal-instruction cases, {decided} decided, "
          f"{sum(changed.values())} changed")
    for (bit, producer), count in sorted(changed.items()):
        print(f"  {count:6d} {bit:5s} {producer}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
