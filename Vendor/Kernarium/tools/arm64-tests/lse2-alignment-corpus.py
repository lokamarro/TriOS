#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Carry the FEAT_LSE2 alignment cases into an A64 instruction corpus dump.

usage: lse2-alignment-corpus.py DUMP OUTPUT

With FEAT_LSE2 an LSE atomic, an exclusive, or a load-acquire/store-release
access that is not naturally aligned but lies inside one 16-byte block
completes single-copy atomically; only one that crosses a block takes an
alignment fault. Both engines advertise USCAT exactly when the host has LSE2.

The producer checks predate that rule and expect a fault from such in-block
accesses. The producer line keeps the harness interpreter lane, which does not
implement the rule, so this script replaces them after export instead: it
drops every producer case that expects an alignment fault from a misaligned
in-block access of these classes and appends the arm64-lse2-alignment records,
whose results follow the architecture and match native LSE2 hardware (Apple
M1 Ultra and Neoverse V2). Completing cases need the host's LSE2 in both
lanes; crossing faults hold on every host.

Run it on a --dump of every regenerated pack, then --repack the output.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import importlib.util
import json
import pathlib
import sys
import zlib

HERE = pathlib.Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "build_unit_corpus", HERE / "build-unit-corpus.py")
corpus = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = corpus
_spec.loader.exec_module(corpus)

PRODUCER = "arm64-lse2-alignment"
SITE = "tools/arm64-tests/lse2-alignment-corpus.py"
REGION = 0x5100_0000
SLOT_BASE = REGION + 0x100
SLOT = 0x20
SVC = 0xD400_0001
CLREX = 0xD503_3F5F
X3 = 0x0123_4567_89AB_CDEF
X7 = 0x7766_5544_3322_1100


# --------------------------------------------------------------------------
# Superseded producer cases


def superseded_size(name: str, word: int) -> int | None:
    """Access size of a producer alignment check this rule supersedes."""
    if name in ("atomic RMW natural alignment",
                "scalar compare-and-swap natural alignment",
                "scalar exclusive natural alignment"):
        return 1 << (word >> 30)
    if name in ("pair compare-and-swap transfer alignment",
                "pair exclusive transfer alignment"):
        return 8 << (word >> 30 & 1)
    return None


def is_superseded(record: dict, case: dict) -> bool:
    if case["event"] != "memory-fault" or "fault" not in case:
        return False
    program = record["programs"][case["program"]]
    word = int(dict(program["words"])[program["entry"]], 16)
    size = superseded_size(case["name"], word)
    if size is None:
        return False
    address = int(case["fault"]["address"], 16)
    return address % size != 0 and (address & 15) + size <= 16


# --------------------------------------------------------------------------
# Hand-authored cases


def mask(size: int) -> int:
    return (1 << (8 * size)) - 1


def signed(value: int, size: int) -> int:
    value &= mask(size)
    return value - (1 << (8 * size)) if value >> (8 * size - 1) else value


RMW = {
    "add": lambda old, source, size: old + source,
    "clr": lambda old, source, size: old & ~source,
    "eor": lambda old, source, size: old ^ source,
    "smax": lambda old, source, size: old if signed(old, size) >= signed(source, size) else source,
    "umin": lambda old, source, size: min(old, source & mask(size)),
    "swp": lambda old, source, size: source,
}

class Case:
    """One case: a program, its slot address, and how it changes state."""

    def __init__(self, name: str, words: list[int], offset: int, size: int,
                 access: str, native: list[str], fault: bool) -> None:
        self.name = name
        self.words = words
        self.offset = offset
        self.size = size
        self.access = access
        self.native = native if fault else native + ["lse2"]
        self.fault = fault
        self.registers: dict[int, int] = {}
        self.vectors: dict[int, bytes] = {}
        self.retired: object = 0 if fault else len(words)
        self.memory_value: bytes | None = None
        self.apply = None


def cases() -> list[Case]:
    result: list[Case] = []

    def add(case: Case) -> Case:
        result.append(case)
        return case

    def rmw(name, word, operation, size, offset, discard=False, fault=False):
        case = add(Case(name, [word, SVC], offset, size, "write", ["lse"], fault))
        case.registers = {3: X3}

        def apply(memory, registers, vectors):
            old = int.from_bytes(memory[:size], "little")
            new = RMW[operation](old, X3 & mask(size), size) & mask(size)
            memory[:size] = new.to_bytes(size, "little")
            if not discard:
                registers[4] = old
        case.apply = apply

    rmw("ldaddh in block", 0x7823_0044, "add", 2, 1)
    rmw("ldaddh across a block", 0x7823_0044, "add", 2, 15, fault=True)
    rmw("ldadd w in block", 0xB823_0044, "add", 4, 12)
    rmw("ldadd w across a block", 0xB823_0044, "add", 4, 13, fault=True)
    rmw("ldadd x in block", 0xF823_0044, "add", 8, 1)
    rmw("ldadd x across a block", 0xF823_0044, "add", 8, 9, fault=True)
    rmw("ldaddal x in block", 0xF8E3_0044, "add", 8, 4)
    rmw("stadd x in block", 0xF823_005F, "add", 8, 4, discard=True)
    rmw("stadd x across a block", 0xF823_005F, "add", 8, 12, fault=True)
    rmw("ldeor x in block", 0xF823_2044, "eor", 8, 2)
    rmw("ldsmax w in block", 0xB823_4044, "smax", 4, 6)
    rmw("ldclrh in block", 0x7823_1044, "clr", 2, 3)
    rmw("ldumin x in block", 0xF823_7044, "umin", 8, 7)
    rmw("swp w in block", 0xB823_8044, "swp", 4, 1)
    rmw("swph in block", 0x7823_8044, "swp", 2, 13)
    rmw("swpal x in block", 0xF8E3_8044, "swp", 8, 3)
    rmw("swpal x across a block", 0xF8E3_8044, "swp", 8, 10, fault=True)

    def cas(name, word, size, offset, match, fault=False):
        case = add(Case(name, [word, SVC], offset, size, "write", ["lse"], fault))
        case.registers = {3: X3, 4: X7}
        if match:
            case.memory_value = (X3 & mask(size)).to_bytes(size, "little")

        def apply(memory, registers, vectors):
            old = int.from_bytes(memory[:size], "little")
            if old == X3 & mask(size):
                memory[:size] = (X7 & mask(size)).to_bytes(size, "little")
            registers[3] = old
        case.apply = apply

    cas("cas x in block", 0xC8A3_7C44, 8, 4, True)
    cas("cas x in block, compare fails", 0xC8A3_7C44, 8, 5, False)
    cas("casal w in block", 0x88E3_FC44, 4, 2, True)
    cas("cash in block", 0x48A3_7C44, 2, 7, True)
    cas("cas x across a block", 0xC8A3_7C44, 8, 12, True, fault=True)
    cas("casal w across a block", 0x88E3_FC44, 4, 13, True, fault=True)

    def casp(name, word, offset, fault=False):
        total = 8 if word >> 30 & 1 == 0 else 16
        case = add(Case(name, [word, SVC], offset, total, "write", ["lse"], fault))
        case.registers = {4: 0x1111_1111_4444_4444, 5: 0x2222_2222_5555_5555,
                          6: 0x6666_6666_6666_6666, 7: X7}
        case.memory_value = ((case.registers[4] & mask(4)) |
                             (case.registers[5] & mask(4)) << 32
                             ).to_bytes(8, "little")

        def apply(memory, registers, vectors):
            first = int.from_bytes(memory[:4], "little")
            second = int.from_bytes(memory[4:8], "little")
            if (first, second) == (case.registers[4] & mask(4),
                                   case.registers[5] & mask(4)):
                memory[:4] = (case.registers[6] & mask(4)).to_bytes(4, "little")
                memory[4:8] = (case.registers[7] & mask(4)).to_bytes(4, "little")
            registers[4] = first
            registers[5] = second
        case.apply = apply

    casp("casp w in block", 0x0824_7C46, 4)
    casp("casp w across a block", 0x0824_7C46, 12, fault=True)
    casp("casp x misaligned", 0x4824_7C46, 8, fault=True)

    def exclusive(name, load, store, size, offset, pair=False):
        # load; store-exclusive; retry on failure; clear the monitor.
        words = [load, store, 0x35FF_FFC5, CLREX, SVC]
        case = add(Case(name, words, offset, 2 * size if pair else size,
                        "read", [], False))
        case.registers = {3: X3, 7: X7}
        case.retired = {"minimum": 5, "step": 3, "maximum": None}

        def apply(memory, registers, vectors):
            if pair:
                registers[4] = int.from_bytes(memory[:size], "little")
                registers[6] = int.from_bytes(memory[size:2 * size], "little")
                memory[:size] = (X3 & mask(size)).to_bytes(size, "little")
                memory[size:2 * size] = (X7 & mask(size)).to_bytes(size, "little")
            else:
                registers[4] = int.from_bytes(memory[:size], "little")
                memory[:size] = (X3 & mask(size)).to_bytes(size, "little")
            registers[5] = 0
        case.apply = apply

    exclusive("ldxr and stxr x in block", 0xC85F_7C44, 0xC805_7C43, 8, 4)
    exclusive("ldaxr and stlxr w in block", 0x885F_FC44, 0x8805_FC43, 4, 9)
    exclusive("ldxrh and stxrh in block", 0x485F_7C44, 0x4805_7C43, 2, 5)
    exclusive("ldxp and stxp w in block", 0x887F_1844, 0x8825_1C43, 4, 4, pair=True)

    def fault(name, word, size, offset, access, native=()):
        case = add(Case(name, [word, SVC], offset, size, access, list(native), True))
        case.registers = {3: X3, 7: X7}

    fault("ldxr x across a block", 0xC85F_7C44, 8, 12, "read")
    fault("stxr x across a block", 0xC805_7C43, 8, 12, "write")
    fault("ldaxr w across a block", 0x885F_FC44, 4, 13, "read")
    fault("stlxr w across a block", 0x8805_FC43, 4, 13, "write")
    fault("ldxp w across a block", 0x887F_1844, 8, 12, "read")
    fault("stxp w across a block", 0x8825_1C43, 8, 12, "write")
    fault("ldxp x misaligned", 0xC87F_1844, 16, 8, "read")

    def load(name, word, size, offset, native, displacement=0, sign=False,
             fault=False):
        case = add(Case(name, [word, SVC], offset, size, "read", native, fault))
        case.base_adjust = displacement

        def apply(memory, registers, vectors):
            value = int.from_bytes(memory[:size], "little")
            if sign:
                value = signed(value, size) & mask(8)
            registers[word & 31] = value
        case.apply = apply

    def store(name, word, size, offset, native, displacement=0, fault=False):
        case = add(Case(name, [word, SVC], offset, size, "write", native, fault))
        case.registers = {3: X3}
        case.base_adjust = displacement

        def apply(memory, registers, vectors):
            memory[:size] = (X3 & mask(size)).to_bytes(size, "little")
        case.apply = apply

    load("ldar x in block", 0xC8DF_FC44, 8, 4, [])
    load("ldar w in block", 0x88DF_FC44, 4, 2, [])
    load("ldar x across a block", 0xC8DF_FC44, 8, 12, [], fault=True)
    load("ldarh across a block", 0x48DF_FC44, 2, 15, [], fault=True)
    store("stlr x in block", 0xC89F_FC43, 8, 4, [])
    store("stlrh in block", 0x489F_FC43, 2, 9, [])
    store("stlr w across a block", 0x889F_FC43, 4, 13, [], fault=True)
    load("ldapr x in block", 0xF8BF_C044, 8, 4, ["lrcpc"])
    load("ldaprh in block", 0x78BF_C044, 2, 7, ["lrcpc"])
    load("ldapr w across a block", 0xB8BF_C044, 4, 14, ["lrcpc"], fault=True)
    load("ldapur x in block", 0xD940_4044, 8, 4, ["lrcpc2"], displacement=4)
    load("ldapursw in block", 0x999F_D044, 4, 11, ["lrcpc2"], displacement=-3,
         sign=True)
    load("ldapur x across a block", 0xD940_4044, 8, 12, ["lrcpc2"],
         displacement=4, fault=True)
    store("stlur x in block", 0xD900_4043, 8, 4, ["lrcpc2"], displacement=4)
    store("stlurh in block", 0x5900_1043, 2, 3, ["lrcpc2"], displacement=1)
    store("stlurh across a block", 0x5900_1043, 2, 15, ["lrcpc2"],
          displacement=1, fault=True)
    return result


def region_blob(page_size: int, slots: list[Case]) -> bytes:
    data = bytearray((index * 0x9D + 0x3B) & 0xFF for index in range(page_size))
    for index, case in enumerate(slots):
        if case.memory_value is not None:
            start = SLOT_BASE - REGION + SLOT * index + case.offset
            data[start:start + len(case.memory_value)] = case.memory_value
    return bytes(data)


def record_for(profile: str, page_size: int) -> tuple[dict, str, bytes]:
    authored = cases()
    blob = region_blob(page_size, authored)
    digest = hashlib.sha256(blob).hexdigest()
    programs: list[tuple[int, ...]] = []
    dumped_cases = []
    for index, case in enumerate(authored):
        words = tuple(case.words)
        if words not in programs:
            programs.append(words)
        address = SLOT_BASE + SLOT * index + case.offset
        registers = {2: address - getattr(case, "base_adjust", 0)}
        registers.update(case.registers)
        vectors = dict(case.vectors)
        initial = {"gpr": list(corpus.FILL_REGISTERS),
                   "vectors": list(corpus.FILL_VECTORS)}
        for register, value in registers.items():
            initial["gpr"][register] = value
        for register, value in vectors.items():
            initial["vectors"][register] = value
        expected_gprs = list(initial["gpr"])
        expected_vectors = list(initial["vectors"])
        expected_memory = bytearray(blob)
        state = {}
        if not case.fault:
            start = address - REGION
            window = bytearray(expected_memory[start:start + case.size])
            gprs = dict(enumerate(expected_gprs))
            vector_state = dict(enumerate(expected_vectors))
            case.apply(window, gprs, vector_state)
            expected_memory[start:start + case.size] = window
            expected_gprs = [gprs[index] & mask(8) for index in range(32)]
            expected_vectors = [vector_state[index] for index in range(32)]
            state["pc"] = corpus.hex64(corpus.CODE_BASE + 4 * len(case.words))
        entry_initial = {
            f"x{index}": corpus.hex64(value)
            for index, value in enumerate(initial["gpr"])
            if index < 31 and value != corpus.FILL_REGISTERS[index]}
        entry_initial.update({
            f"v{index}": value.hex()
            for index, value in enumerate(initial["vectors"])
            if value != corpus.FILL_VECTORS[index]})
        for index in range(31):
            if expected_gprs[index] != initial["gpr"][index]:
                state[f"x{index}"] = corpus.hex64(expected_gprs[index])
        for index in range(32):
            if expected_vectors[index] != initial["vectors"][index]:
                state[f"v{index}"] = expected_vectors[index].hex()
        item = {
            "name": case.name, "program": programs.index(words),
            "event": "memory-fault" if case.fault else "system-call",
            "ordinals": [0, 0], "invalid_encoding": False,
            "minimization": "full", "native": case.native,
            "initial": entry_initial, "expected": state,
            "retired": case.retired,
            "memory": [{
                "address": corpus.hex64(REGION), "size": page_size,
                "flags": 19, "blob": digest,
                "expected": [[offset, data.hex()] for offset, data in
                             corpus.runs_between(blob, bytes(expected_memory))],
                "tags": [], "expected_tags": [],
            }],
        }
        if case.fault:
            item["fault"] = {"address": corpus.hex64(address),
                             "access": case.access}
        dumped_cases.append(item)
    record = {
        "record": f"{PRODUCER}/{profile}", "producer": PRODUCER, "site": SITE,
        "profiles": [profile], "options": [],
        "programs": [{"entry": 0, "words": [[4 * index, f"{word:08x}"]
                                            for index, word in enumerate(words)]}
                     for words in programs],
        "cases": dumped_cases,
    }
    return record, digest, blob


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    options = parser.parse_args()
    lines = options.dump.read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    blobs: list[str] = []
    known_blobs: set[str] = set()
    records: list[dict] = []
    removed = 0
    for line in lines[1:]:
        if not line.strip():
            continue
        item = json.loads(line)
        if "blob" in item:
            known_blobs.add(item["blob"])
            blobs.append(line)
            continue
        if item["producer"] == PRODUCER:
            continue
        kept = [case for case in item["cases"] if not is_superseded(item, case)]
        removed += len(item["cases"]) - len(kept)
        if not kept:
            continue
        used = sorted({case["program"] for case in kept})
        renumber = {old: new for new, old in enumerate(used)}
        item["programs"] = [item["programs"][index] for index in used]
        for case in kept:
            case["program"] = renumber[case["program"]]
        item["cases"] = kept
        records.append(item)
    added = 0
    for profile, page_size in (("4k", 4096),):
        record, digest, blob = record_for(profile, page_size)
        if digest not in known_blobs:
            known_blobs.add(digest)
            blobs.append(json.dumps({"blob": digest,
                                     "zlib": zlib.compress(blob, 9).hex()},
                                    sort_keys=True))
        records.append(record)
        added += len(record["cases"])
    # A rerun on a finished dump finds nothing left to drop; the count stays
    # that of the export's.
    removed += header["meta"].get("lse2_alignment", {}).get("removed_cases", 0)
    header["meta"]["lse2_alignment"] = {
        "script": SITE,
        "sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
        "removed_cases": removed, "added_cases": added,
    }
    with options.output.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in blobs:
            sink.write(line + "\n")
        for record in records:
            sink.write(json.dumps(record, sort_keys=True) + "\n")
    print(f"removed {removed} superseded cases, added {added} LSE2 cases")
    return 0


if __name__ == "__main__":
    sys.exit(main())
