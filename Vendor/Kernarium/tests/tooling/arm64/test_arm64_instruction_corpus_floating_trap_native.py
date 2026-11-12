#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Re-derive the A64 instruction corpus's native floating-point trap cases.

tools/arm64-tests/floating-trap-native-corpus.py authors the
arm64-floating-trap-native records and has the host decide the outcomes of
those records and of the register-only arm64-floating-trap-scalar, -vector
and -control cases. This
test re-derives everything but the outcomes from the current script, so a
pack can only hold the records of the request the script writes now:

- provenance.json names the script's request;
- the records are the script's groups, in the 4 KiB profile, without
  harness options, and each case is the script's case in order: its name,
  program, native requirement and initial state;
- each authored outcome is the cause the script gives the case, or a
  completion at its SVC;
- every trap, authored or rerecorded, reports one cause with EC 0x2c, IL
  and TFV, and one at the program's entry changes no state.

build-unit-corpus.py --check already refuses a pack whose recorded script
hash is stale.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import importlib.util
import json
import pathlib
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
GENERATOR = REPOSITORY / "tools" / "arm64-tests" / \
    "floating-trap-native-corpus.py"


def load_generator():
    spec = importlib.util.spec_from_file_location(
        "floating_trap_native_corpus", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def check_outcome(trap, name: str, case: dict, words: tuple[int, ...],
                  undefined: bool = False) -> int:
    """Checks a case's outcome form; returns its reported cause or 0."""
    code = trap.corpus.CODE_BASE
    pc = int(case["expected"].get("pc", trap.native.hex64(code)), 16)
    if case["retired"] != (pc - code) // 4:
        raise SystemExit(f"{name}: retired count and PC disagree")
    if case["event"] == "system-call":
        if "syndrome" in case or words[case["retired"] - 1] != \
                trap.native.SVC:
            raise SystemExit(f"{name}: does not complete at its SVC")
        return 0
    if undefined and case["event"] == "illegal-instruction":
        return 0
    if case["event"] != "floating-point-trap":
        raise SystemExit(f"{name}: {case['event']}")
    syndrome = int(case.get("syndrome", "0"), 16)
    cause = syndrome & trap.FLAGS
    if syndrome & ~trap.FLAGS != trap.SYNDROME or not cause or \
            cause & (cause - 1):
        raise SystemExit(f"{name}: syndrome {syndrome:#010x} is not one "
                         "trapped cause")
    if case["retired"] == 0 and case["expected"]:
        raise SystemExit(f"{name}: the trap changes state")
    return cause


def check(trap, lines: list[str], meta: dict) -> str:
    request = trap.request_bytes(trap.strip_native(lines))
    if meta.get("request_sha256") != hashlib.sha256(request).hexdigest() or \
            meta.get("script") != trap.SITE:
        raise SystemExit("the pack's native trap records answer another "
                         "request")
    items = list(trap.drafts())
    if "afp" in meta:
        if meta["afp"].get("request_sha256") != \
                hashlib.sha256(trap.afp_request_bytes()).hexdigest():
            raise SystemExit("the pack's FEAT_AFP trap records answer "
                             "another request")
        items += trap.afp_host_drafts()
    by_group: dict[str, list] = {}
    for draft in items:
        by_group.setdefault(draft.group, []).append(draft)
    records = [json.loads(line) for line in lines]
    rerecorded = 0
    for record in records:
        if record.get("producer") in trap.RERECORDED:
            for case in record["cases"]:
                if case.get("memory"):
                    continue
                program = record["programs"][case["program"]]
                words = tuple(int(word, 16) for _offset, word in
                              program["words"])
                check_outcome(trap, f"{record['record']}: {case['name']}",
                              case, words, undefined=True)
                rerecorded += 1
    native = [record for record in records
              if record.get("producer") == trap.PRODUCER]
    names = [f"{trap.PRODUCER}/{group}" for group in trap.GROUPS
             if group in by_group]
    if [record["record"] for record in native] != names:
        raise SystemExit("the native trap records are not the script's "
                         "groups")
    total = 0
    for record in native:
        group = record["record"].split("/", 1)[1]
        if (record["site"], record["profiles"], record["options"]) != \
                (trap.SITE, ["4k"], []) or "svl" in record:
            raise SystemExit(f"{record['record']}: not the script's record")
        expected = by_group[group]
        if len(record["cases"]) != len(expected):
            raise SystemExit(f"{record['record']}: {len(record['cases'])} "
                             f"cases, the script writes {len(expected)}")
        for draft, case in zip(expected, record["cases"]):
            program = record["programs"][case["program"]]
            words = tuple(int(word, 16) for _offset, word in program["words"])
            if words != (draft.word, trap.native.SVC) or program["entry"]:
                raise SystemExit(f"{draft.name}: not the script's program")
            skeleton = trap.case_skeleton(draft, case["program"])
            if {key: case.get(key) for key in skeleton} != skeleton:
                raise SystemExit(f"{draft.name}: not the script's case")
            if check_outcome(trap, draft.name, case, words) != draft.expect:
                raise SystemExit(f"{draft.name}: not the cause the script "
                                 "gives it")
            total += 1
    return (f"native floating-point trap records: {total} cases in "
            f"{len(native)} records re-derived from {trap.SITE}, and "
            f"{rerecorded} rerecorded outcomes are well formed")


def main() -> int:
    trap = load_generator()
    builder = trap.corpus
    pack = pathlib.Path(sys.argv[1])
    corpus, tokens = builder.decode_pack(pack.read_bytes())
    lines = [line for line in builder.dump_corpus(corpus, tokens)
             if '"cases"' in line]
    meta = corpus.meta.get("floating_trap_native")
    if meta is None:
        raise SystemExit("the pack holds no native floating-point trap "
                         "records")
    print(check(trap, lines, meta))
    return 0


if __name__ == "__main__":
    sys.exit(main())
