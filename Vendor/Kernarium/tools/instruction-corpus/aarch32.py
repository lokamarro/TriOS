#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The AArch32 instruction corpus: source tables to pack.

tests/unit/arm64/aarch32_corpus/README.md owns the source format and the
case model; this module owns the pack layout. It reads the source tables
listed by an index (kind "aarch32"), checks every row, and writes the pack the
unit runner reads. It runs no engine and no oracle: the tables hold the
architectural inputs and what the oracle observed.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import json
import pathlib
import struct

PACK_MAGIC = b"UMLA32C\0"
# 2: each case holds the byte offset of its program in the code region.
PACK_VERSION = 2
GPRS = 15
D_REGISTERS = 32
STATE_SIZE = 336
STATE_KEYS = (
    tuple(f"r{i}" for i in range(GPRS))
    + ("apsr", "fpscr", "tpidruro", "tpidrurw")
    + tuple(f"d{i}" for i in range(D_REGISTERS))
)
MISC_KEYS = ("apsr", "fpscr", "tpidruro", "tpidrurw")
APSR_MASK = 0xF80F0000
# The memory map every case runs in (tools/arm64-tests/a32_oracle.cpp).
CODE_BASE = 0x4F000000
CODE_SIZE = 0x4000
ARM_EXIT = CODE_BASE + 0x800
THUMB_EXIT = CODE_BASE + 0x900
# Code-region ranges the harness owns: the exit stubs, the trampolines and the launch.
RESERVED_CODE = ((0x800, 0x908), (0x3F00, 0x3F90))
# The word after a stub's load: the oracle's trampolines to its own epilogues.
ARM_EPILOGUE = CODE_BASE + 0x3F00
THUMB_EPILOGUE = CODE_BASE + 0x3F08
DATA_BASE = 0x50000000
DATA_SIZE = 0x8000
READ_ONLY_BASE = 0x50008000
READ_ONLY_SIZE = 0x4000
FILL_ZERO = 0
FILL_HASH32 = 1
PROFILE_16K = 1
PROFILE_4K = 2
FLAG_THUMB = 1
FLAG_STRAIGHT = 2
FLAG_MEMORY_UNCHECKED = 4
FLAG_EXIT_THUMB = 8
FLAG_NAMES = {"t": FLAG_THUMB, "s": FLAG_STRAIGHT, "u": FLAG_MEMORY_UNCHECKED, "x": FLAG_EXIT_THUMB}
# Events are signal numbers; 0 is a program that reached an exit stub, and
# SVC (a syscall event) is 255.
EVENTS = {0: "none", 4: "SIGILL", 5: "SIGTRAP", 7: "SIGBUS", 8: "SIGFPE", 11: "SIGSEGV", 255: "svc"}
CASE_KEYS = {"n", "f", "i", "p", "t", "s", "si", "d", "dp", "x", "e", "fi", "fa", "k", "m", "fl", "o"}


class CorpusError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CorpusError(message)


# Numbers in the tables ----------------------------------------------------------


def number(value, bits: int, what: str) -> int:
    require(isinstance(value, str) and value, f"{what} must be a hexadecimal string")
    try:
        result = int(value, 16)
    except ValueError:
        raise CorpusError(f"{what} is not hexadecimal: {value!r}") from None
    require(0 <= result < 1 << bits, f"{what} does not fit in {bits} bits")
    return result


def state_value(key: str, value) -> int:
    bits = 64 if key.startswith("d") else 32
    result = number(value, bits, key)
    if key == "apsr":
        require(result & ~APSR_MASK == 0, "apsr holds only N, Z, C, V, Q and GE")
    return result


def hex_value(key: str, value: int) -> str:
    return f"{value:016x}" if key.startswith("d") else f"{value:x}"


def complete_state(source: dict, name: str) -> dict[str, int]:
    """A recipe row as a complete state."""
    state: dict[str, int] = {}
    registers = source.get("r")
    require(isinstance(registers, list) and len(registers) == GPRS, f"{name}: r needs 15 values")
    for index, value in enumerate(registers):
        state[f"r{index}"] = number(value, 32, f"{name}.r{index}")
    for key in MISC_KEYS:
        require(key in source, f"{name}: missing {key}")
        state[key] = state_value(key, source[key])
    vectors = source.get("d")
    require(isinstance(vectors, list) and len(vectors) == D_REGISTERS, f"{name}: d needs 32 values")
    for index, value in enumerate(vectors):
        state[f"d{index}"] = number(value, 64, f"{name}.d{index}")
    return state


def recipe_row(name: str, state: dict[str, int]) -> dict:
    return {
        "state": name,
        "r": [f"{state[f'r{i}']:x}" for i in range(GPRS)],
        **{key: f"{state[key]:x}" for key in MISC_KEYS},
        "d": [f"{state[f'd{i}']:016x}" for i in range(D_REGISTERS)],
    }


def apply_delta(state: dict[str, int], delta: dict, what: str) -> dict[str, int]:
    result = dict(state)
    for key, value in delta.items():
        require(key in STATE_KEYS, f"{what}: unknown state key {key!r}")
        result[key] = state_value(key, value)
    return result


def difference(before: dict[str, int], after: dict[str, int]) -> dict[str, str]:
    return {key: hex_value(key, after[key]) for key in STATE_KEYS if before[key] != after[key]}


# The wire form --------------------------------------------------------------------


def str16(text: str) -> bytes:
    data = text.encode("utf-8")
    require(len(data) < 1 << 16, "string too long")
    return struct.pack("<H", len(data)) + data


def str32(text: str) -> bytes:
    data = text.encode("utf-8")
    return struct.pack("<I", len(data)) + data


def state_wire(state: dict[str, int]) -> bytes:
    """The oracle's State layout: r0-r14, APSR, FPSCR, TPIDRURO, TPIDRURW, a
    reserved word, D0-D31."""
    values = [state[f"r{i}"] for i in range(GPRS)]
    values += [state[key] for key in MISC_KEYS] + [0]
    data = struct.pack("<20I", *values) + struct.pack(
        "<32Q", *[state[f"d{i}"] for i in range(D_REGISTERS)]
    )
    require(len(data) == STATE_SIZE, "state layout")
    return data


def state_from_wire(data: bytes, offset: int = 0) -> dict[str, int]:
    words = struct.unpack_from("<20I", data, offset)
    vectors = struct.unpack_from("<32Q", data, offset + 80)
    state = {f"r{i}": words[i] for i in range(GPRS)}
    state.update(zip(MISC_KEYS, words[GPRS:GPRS + 4]))
    state.update({f"d{i}": vectors[i] for i in range(D_REGISTERS)})
    return state


def delta_wire(delta: dict[str, int]) -> bytes:
    registers = 0
    misc = 0
    vectors = 0
    body = b""
    for index in range(GPRS):
        key = f"r{index}"
        if key in delta:
            registers |= 1 << index
            body += struct.pack("<I", delta[key])
    for index, key in enumerate(MISC_KEYS):
        if key in delta:
            misc |= 1 << index
            body += struct.pack("<I", delta[key])
    for index in range(D_REGISTERS):
        key = f"d{index}"
        if key in delta:
            vectors |= 1 << index
            body += struct.pack("<Q", delta[key])
    return struct.pack("<HBI", registers, misc, vectors) + body


def patches_wire(patches: list[tuple[int, bytes]]) -> bytes:
    data = struct.pack("<H", len(patches))
    for offset, payload in patches:
        data += struct.pack("<IH", offset, len(payload)) + payload
    return data


# Cases ----------------------------------------------------------------------------


def program_halfwords(row: dict) -> tuple[bool, list[int]]:
    """(Thumb entry, program as halfwords in memory order)."""
    isa = row["i"]
    require(isa in ("A", "T"), f"{row['n']}: instruction set must be A or T")
    text = row["p"]
    require(isinstance(text, str) and text, f"{row['n']}: empty program")
    if isa == "A":
        require(len(text) % 8 == 0, f"{row['n']}: an A32 program is whole words")
        halves = []
        for position in range(0, len(text), 8):
            word = number(text[position:position + 8], 32, f"{row['n']}.p")
            halves += [word & 0xFFFF, word >> 16]
        return False, halves
    require(len(text) % 4 == 0, f"{row['n']}: a Thumb program is whole halfwords")
    return True, [number(text[position:position + 4], 16, f"{row['n']}.p")
                  for position in range(0, len(text), 4)]


def parse_patches(value, what: str) -> list[tuple[int, bytes]]:
    result = []
    end = 0
    for item in value or []:
        require(isinstance(item, list) and len(item) == 2, f"{what}: a patch is [offset, hex]")
        offset = item[0]
        require(isinstance(offset, int) and offset >= end, f"{what}: patches are ordered and disjoint")
        try:
            payload = bytes.fromhex(item[1])
        except ValueError:
            raise CorpusError(f"{what}: patch bytes are not hexadecimal") from None
        require(payload and offset + len(payload) <= DATA_SIZE, f"{what}: patch outside the data")
        result.append((offset, payload))
        end = offset + len(payload)
    return result


def case_wire(row: dict, recipes: dict[str, dict[str, int]], recipe_ids: dict[str, int],
              defaults: dict) -> bytes:
    unknown = set(row) - CASE_KEYS
    require(not unknown, f"{row.get('n')}: unknown keys {sorted(unknown)}")
    row = {**defaults, **row}
    name = row["n"]
    thumb, halves = program_halfwords(row)
    require(len(halves) <= 0x380, f"{name}: program too long")
    recipe = row["s"]
    require(recipe in recipes, f"{name}: unknown state recipe {recipe!r}")
    initial = apply_delta(recipes[recipe], row.get("si", {}), f"{name}.si")
    final = apply_delta(initial, row.get("x", {}), f"{name}.x")
    flags = 0
    for letter in row.get("fl", ""):
        require(letter in FLAG_NAMES, f"{name}: unknown flag {letter!r}")
        flags |= FLAG_NAMES[letter]
    require(bool(flags & FLAG_THUMB) == thumb or not flags & FLAG_THUMB, f"{name}: flag t disagrees with i")
    flags = (flags & ~FLAG_THUMB) | (FLAG_THUMB if thumb else 0)
    event = row.get("e", 0)
    require(event in EVENTS, f"{name}: unknown event {event}")
    target = row["t"]
    require(isinstance(target, int) and 0 <= target < len(halves), f"{name}: target out of range")
    origin = row.get("o", 0)
    require(isinstance(origin, int) and origin % (2 if thumb else 4) == 0
            and 0 <= origin and origin + 2 * len(halves) <= CODE_SIZE, f"{name}: program origin")
    if origin:  # the exit stubs, their trampolines and the launch are the harness's
        for low, high in RESERVED_CODE:
            require(origin + 2 * len(halves) <= low or origin >= high, f"{name}: program overlaps {low:#x}")
    fault_instruction = row.get("fi", 0)
    require(isinstance(fault_instruction, int) and 0 <= fault_instruction < len(halves),
            f"{name}: fault instruction out of range")
    retired = row.get("k")
    require(isinstance(retired, int) and 0 <= retired < 1 << 16, f"{name}: retired count")
    fault_address = number(row.get("fa", "0"), 32, f"{name}.fa")
    fill = row.get("d", FILL_HASH32)
    require(fill in (FILL_ZERO, FILL_HASH32), f"{name}: unknown data fill")
    data = str16(name) + str16(row["f"]) + struct.pack("<BHH", flags, target, origin)
    data += struct.pack("<H", len(halves)) + b"".join(struct.pack("<H", half) for half in halves)
    data += struct.pack("<H", recipe_ids[recipe]) + delta_wire(
        {key: value for key, value in initial.items() if value != recipes[recipe][key]})
    data += struct.pack("<B", fill) + patches_wire(parse_patches(row.get("dp"), f"{name}.dp"))
    data += struct.pack("<BHHI", event, fault_instruction, retired, fault_address)
    data += delta_wire({key: value for key, value in final.items() if value != initial[key]})
    data += patches_wire(parse_patches(row.get("m"), f"{name}.m"))
    return data


def read_jsonl(path: pathlib.Path) -> list[dict]:
    rows = []
    with path.open(encoding="utf-8") as source:
        for number_, line in enumerate(source, 1):
            if line.strip():
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as error:
                    raise CorpusError(f"{path.name}:{number_}: {error}") from None
    return rows


def source_file(root: pathlib.Path, name: str) -> pathlib.Path:
    path = pathlib.PurePosixPath(name)
    require(not path.is_absolute() and ".." not in path.parts,
            "source index contains a path outside its directory")
    return root / path


def generate(root: pathlib.Path, index: dict) -> bytes:
    """The pack of an index whose files are state recipes and record tables."""
    require(index.get("kind") == "aarch32", "not an AArch32 corpus index")
    recipes: dict[str, dict[str, int]] = {}
    recipe_ids: dict[str, int] = {}
    records = []
    for name in index["files"]:
        rows = read_jsonl(source_file(root, name))
        if rows and "state" in rows[0]:
            for row in rows:
                require(set(row) == {"state", "r", "d", *MISC_KEYS}, f"{name}: recipe keys")
                require(row["state"] not in recipes, f"duplicate recipe {row['state']}")
                recipe_ids[row["state"]] = len(recipes)
                recipes[row["state"]] = complete_state(row, row["state"])
            continue
        require(rows and "record" in rows[0], f"{name}: no record header")
        header = rows[0]
        require(set(header) <= {"record", "oracle", "profiles", "defaults", "environment"}, f"{name}: header keys")
        profiles = header.get("profiles", PROFILE_16K | PROFILE_4K)
        require(profiles in (1, 2, 3), f"{name}: profiles")
        defaults = header.get("defaults", {})
        cases = [case_wire(row, recipes, recipe_ids, defaults) for row in rows[1:]]
        records.append(
            str16(header["record"]) + str16(header["oracle"]) + struct.pack("<BI", profiles, len(cases))
            + b"".join(cases))
    names = [record_name for record_name in index["records"]]
    meta = {"schema": 1, "records": names, "oracle_memory_map": {
        "code": [CODE_BASE, CODE_SIZE], "arm_exit": ARM_EXIT, "thumb_exit": THUMB_EXIT,
        "arm_epilogue": ARM_EPILOGUE, "thumb_epilogue": THUMB_EPILOGUE,
        "data": [DATA_BASE, DATA_SIZE], "read_only": [READ_ONLY_BASE, READ_ONLY_SIZE]}}
    body = str32(json.dumps(meta, sort_keys=True, separators=(",", ":")))
    body += struct.pack("<I", len(recipes))
    for name_, state in recipes.items():
        body += str16(name_) + state_wire(state)
    body += struct.pack("<I", len(records)) + b"".join(records)
    return PACK_MAGIC + struct.pack("<IIQ", PACK_VERSION, 0, len(body)) + body


# Reading a pack (tests and tools) ---------------------------------------------------


class Reader:
    def __init__(self, data: bytes, offset: int = 0):
        self.data = data
        self.offset = offset

    def take(self, size: int) -> bytes:
        require(self.offset + size <= len(self.data), "truncated pack")
        chunk = self.data[self.offset:self.offset + size]
        self.offset += size
        return chunk

    def unpack(self, fmt: str):
        values = struct.unpack(fmt, self.take(struct.calcsize(fmt)))
        return values if len(values) > 1 else values[0]

    def str16(self) -> str:
        return self.take(self.unpack("<H")).decode("utf-8")

    def str32(self) -> str:
        return self.take(self.unpack("<I")).decode("utf-8")

    def delta(self) -> dict[str, int]:
        registers, misc, vectors = self.unpack("<HBI")
        result = {}
        for index in range(GPRS):
            if registers >> index & 1:
                result[f"r{index}"] = self.unpack("<I")
        for index, key in enumerate(MISC_KEYS):
            if misc >> index & 1:
                result[key] = self.unpack("<I")
        for index in range(D_REGISTERS):
            if vectors >> index & 1:
                result[f"d{index}"] = self.unpack("<Q")
        return result

    def patches(self) -> list[tuple[int, bytes]]:
        result = []
        for _ in range(self.unpack("<H")):
            offset, length = self.unpack("<IH")
            result.append((offset, self.take(length)))
        return result


def parse_pack(data: bytes) -> dict:
    """The pack as nested dicts: meta, recipes and records with cases. A
    case holds its program, initial state (a recipe and the changes to it),
    data fill and patches, and what the oracle observed as changes."""
    require(data[:8] == PACK_MAGIC, "not an AArch32 corpus pack")
    reader = Reader(data, 8)
    version, _flags, length = reader.unpack("<IIQ")
    require(version == PACK_VERSION and len(data) == 24 + length, "pack header")
    meta = json.loads(reader.str32())
    recipes = {}
    names = []
    for _ in range(reader.unpack("<I")):
        name = reader.str16()
        names.append(name)
        recipes[name] = state_from_wire(reader.take(STATE_SIZE))
    records = []
    for _ in range(reader.unpack("<I")):
        record = {"name": reader.str16(), "oracle": reader.str16()}
        record["profiles"], count = reader.unpack("<BI")
        record["cases"] = []
        for _ in range(count):
            case = {"name": reader.str16(), "form": reader.str16()}
            case["flags"], case["target"], case["origin"] = reader.unpack("<BHH")
            halves = reader.unpack("<H")
            case["halfwords"] = list(struct.unpack(f"<{halves}H", reader.take(2 * halves)))
            case["recipe"] = names[reader.unpack("<H")]
            case["initial"] = reader.delta()
            case["fill"] = reader.unpack("<B")
            case["data_patches"] = reader.patches()
            case["event"], case["fault_instruction"], case["retired"], case["fault_address"] = \
                reader.unpack("<BHHI")
            case["final"] = reader.delta()
            case["memory_patches"] = reader.patches()
            record["cases"].append(case)
        records.append(record)
    require(reader.offset == len(data), "trailing bytes in the pack")
    return {"meta": meta, "recipes": recipes, "records": records}
