#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the AArch32 instruction corpus's source tables offline.

The tables under tests/unit/arm64/aarch32_corpus must generate the pack their
provenance records, every row must be well formed, the manifest, coverage and
exclusion tables must be what the producer derives from the rows, and every
inventory entry the corpus owes cases must have some, and every known
difference from a hardware oracle must name cases the corpus has. The oracle,
the Arm XML and LLVM are not needed: the tables hold what the oracle observed.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import fnmatch
import hashlib
import importlib.util
import json
import pathlib
import shutil
import struct
import sys
import tempfile

TOOLING = pathlib.Path(__file__).resolve().parent
REPOSITORY = TOOLING.parents[2]
sys.path.insert(0, str(TOOLING))
sys.path.insert(0, str(REPOSITORY / "tools/instruction-corpus"))
sys.path.insert(0, str(REPOSITORY / "tools/arm64-tests"))

import a32_corpus_wire as wire  # noqa: E402
import a32_profile_policy as policy  # noqa: E402
import aarch32 as corpus  # noqa: E402

DIRECTORY = REPOSITORY / "tests/unit/arm64/aarch32_corpus"
INVENTORY = REPOSITORY / "tests/unit/arm64/a32_instruction_inventory.tsv"
ORACLES = {"qemu", "pi4", "pi5", "profile"}
# The inventory levels whose executed entries the corpus owes cases.
REQUIRED_LEVELS = {"base", "simd", "complete"}
# Suites whose golden only hardware can settle.
HARDWARE_ONLY = {"unaligned", "fpscr-reserved", "positions-unaligned"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"error: {message}")


def producer():
    spec = importlib.util.spec_from_file_location(
        "a32_corpus_producer", REPOSITORY / "tools/arm64-tests/a32-corpus.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def inventory_rows() -> list[dict[str, str]]:
    lines = INVENTORY.read_text(encoding="utf-8").splitlines()
    names = lines[0].split("\t")
    return [dict(zip(names, line.split("\t"))) for line in lines[1:]]


def check_source() -> dict:
    source = DIRECTORY / "source"
    index = json.loads((source / "index.json").read_text(encoding="utf-8"))
    require(index.get("schema") == 1 and index.get("kind") == "aarch32", "source index")
    require(index["files"][0] == "states.jsonl", "the state recipes come first")
    pack = corpus.generate(source, index)
    provenance = json.loads((DIRECTORY / "provenance.json").read_text(encoding="utf-8"))
    require(hashlib.sha256(pack).hexdigest() == provenance["pack_sha256"],
            "the source tables do not generate the recorded pack")
    parsed = corpus.parse_pack(pack)
    require(provenance["cases"] == sum(len(r["cases"]) for r in parsed["records"]), "case count")
    require(provenance["architecture_archive_sha256"] ==
            "b389a93f41504376895b723ed6e9861ce369e2253c138e6f4c46e27e95375887", "archive pin")
    names = [record["name"] for record in parsed["records"]]
    require(names == index["records"] and len(set(names)) == len(names), "record names")
    for record in parsed["records"]:
        oracle = record["oracle"]
        require(oracle in ORACLES, f"{record['name']}: unknown oracle {oracle!r}")
        suite = record["name"].removesuffix("-" + oracle)
        require(record["name"] == f"{suite}-{oracle}", f"{record['name']}: named for its suite and oracle")
        require(not (suite in HARDWARE_ONLY and oracle in ("qemu", "profile")),
                f"{record['name']}: only hardware settles this suite")
        keys = set()
        for case in record["cases"]:
            key = (case["name"], case["form"], case["recipe"], tuple(case["halfwords"]))
            require(key not in keys, f"{record['name']}: {case['name']} is stored twice")
            keys.add(key)
            require(case["name"].startswith(case["form"] + "/") or "/" not in case["name"] or True, "name")
            require(case["target"] < len(case["halfwords"]), f"{case['name']}: target")
            require(case["event"] == 0 or case["fault_instruction"] < len(case["halfwords"]),
                    f"{case['name']}: fault position")
    return {"provenance": provenance, "parsed": parsed}


def check_derived(prior: dict) -> None:
    """The manifest, coverage and exclusion tables are the producer's output."""
    module = producer()
    with tempfile.TemporaryDirectory(prefix="a32-corpus-test-") as scratch:
        copy = pathlib.Path(scratch) / "aarch32_corpus"
        shutil.copytree(DIRECTORY, copy)
        for name in ("provenance.json", "manifest.tsv", "coverage.tsv", "excluded.tsv"):
            (copy / name).unlink()
        module.finish(type("Arguments", (), {"directory": str(copy)}))
        for name in ("provenance.json", "manifest.tsv", "coverage.tsv", "excluded.tsv"):
            require((copy / name).read_bytes() == (DIRECTORY / name).read_bytes(),
                    f"{name} differs from what the source tables derive")


def check_coverage(prior: dict) -> None:
    rows = inventory_rows()
    forms = {row["encoding"]: row for row in rows}
    counts: dict[str, int] = {}
    for record in prior["parsed"]["records"]:
        for case in record["cases"]:
            counts[case["form"]] = counts.get(case["form"], 0) + 1
    for form in counts:
        require(form in forms or form in ("Undefined", "ReservedHint"),
                f"a case names {form}, which is not an inventory encoding")
    owed = [row for row in rows
            if row["level"] in REQUIRED_LEVELS and row["profile"] != "undefined"
            and row["encoding"] not in policy.NEVER_EXECUTED]
    missing = [row["encoding"] for row in owed if row["encoding"] not in counts]
    require(not missing, f"{len(missing)} inventory entries have no case: {missing[:8]}")


def check_known_differences(prior: dict) -> None:
    """Each known difference names an oracle, a suite the corpus records and
    cases of it, and fields compare reads."""
    module = producer()
    cases: dict[str, list[str]] = {}
    for record in prior["parsed"]["records"]:
        suite = record["name"].removesuffix("-" + record["oracle"])
        cases[suite] = [case["name"] for case in record["cases"]]
    for row in module.known_differences():
        require(row["oracle"] in ORACLES - {"profile"}, f"known difference for oracle {row['oracle']!r}")
        require(row["suite"] in cases, f"known difference in {row['suite']!r}, which has no record")
        require(any(fnmatch.fnmatchcase(name, row["cases"]) for name in cases[row["suite"]]),
                f"known difference {row['suite']} {row['cases']} matches no case")
        require(row["reason"].strip() != "", f"known difference {row['cases']} without a reason")


def check_run_disagreements() -> None:
    """observe accepts a difference between an oracle's two runs only in the
    fields known-differences.tsv leaves open for that oracle and case."""
    module = producer()
    recipes = {name: dict(state) for name, state in __import__("a32_corpus_plan").make_recipes().items()}
    case = wire.Case("memory", "LDRD_i_A1_off/3.wrap", "LDRD_i_A1_off", "A", (0xE1C487D3,), 0,
                     "edges3-a", {"apsr": 0x80000000, "r4": 0xFFFFFF89})
    start = wire.initial_state(case, recipes)

    def observed(address: int, **changes: int) -> wire.Observation:
        return wire.Observation(11, 1, address, 0x4F000000, 0, wire.initial_data_digest(case),
                                start | changes, [])

    def runs(oracle: str, second: wire.Observation) -> tuple[list, list]:
        return module.run_disagreements(oracle, "memory", [case], recipes, [observed(0xFFFFFFFC)],
                                        [second])

    require(runs("pi4", observed(0xFFFFFFFC)) == ([], []), "identical runs")
    require(runs("pi4", observed(0)) == ([(case.name, ["fa"])], []),
            "a fault address the Cortex-A72 reports either way is open between its runs")
    require(runs("qemu", observed(0)) == ([], [(case.name, ["fa"])]),
            "a difference is open only for the oracle known-differences.tsv names")
    require(runs("pi4", observed(0, r0=1)) == ([], [(case.name, ["fa", "x"])]),
            "a difference in a field the row does not list is not open")


def check_wire() -> None:
    cases = [
        wire.Case("t", "ADC_i_A1/0", "ADC_i_A1", "A", (0xE2A16C3C,), 0, "zero-a"),
        wire.Case("t", "ADD_r_T1/0", "ADD_r_T1", "T", (0x1888,), 0, "zero-t",
                  {"r1": 5}, data_patches=[(8, b"\x01\x02\x03")]),
    ]
    recipes = {name: dict(state) for name, state in __import__("a32_corpus_plan").make_recipes().items()}
    request = wire.request(cases, recipes)
    require(request[:8] == wire.REQUEST_MAGIC and struct.unpack_from("<I", request, 8)[0] == 2,
            "request header")
    require(cases[0].halfwords() == [0x6C3C, 0xE2A1] and cases[0].program_text() == "e2a16c3c",
            "an A32 word is two halfwords, low first")
    require(cases[1].starts() == [0] and wire.Case("t", "n", "f", "T", (0xF8DF, 0xF000, 0xBF00),
                                                   0, "zero-t").starts() == [0, 2], "Thumb instruction starts")
    require(wire.fnv32(b"") == 0x811C9DC5 and wire.initial_data_digest(cases[1]) != 0, "data digest")
    state = corpus.state_wire(recipes["zero-a"])
    record = struct.pack("<8I", 32 + len(state), 4, 2, 0x4F000000, 0x4F000000, 0, 7, 0) + state
    response = wire.RESPONSE_MAGIC + struct.pack("<4I", 1, 5, 6, 0) + record
    meta, observations = wire.parse_response(response)
    require(meta == {"hwcap": 5, "hwcap2": 6} and observations[0].signal == 4
            and observations[0].digest == 7 and observations[0].state == recipes["zero-a"] | {
                "apsr": 0}, "response")
    for bad in (response + b"\0", b"A32ORS0\0" + response[8:], response[:-4]):
        try:
            wire.parse_response(bad)
        except (corpus.CorpusError, struct.error):
            continue
        raise SystemExit("error: a malformed response was accepted")


def main() -> int:
    prior = check_source()
    check_derived(prior)
    check_coverage(prior)
    check_known_differences(prior)
    check_run_disagreements()
    check_wire()
    print(f"AArch32 corpus: {prior['provenance']['cases']} cases in "
          f"{len(prior['parsed']['records'])} records")
    return 0


if __name__ == "__main__":
    sys.exit(main())
