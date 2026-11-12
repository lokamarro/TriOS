#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build instruction-test assets offline from reviewable source tables.

The tables hold architectural inputs and independent golden expectations, not
compressed packs. Byte recipes expand repeated patterns and sparse changes.
Imports preserve an accepted pack's meaning; generation never runs an engine
or a hardware oracle and never reads Git history.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import copy
import functools
import hashlib
import importlib.util
import itertools
import json
import pathlib
import shutil
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import loops  # noqa: E402


def load(name: str, path: pathlib.Path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def json_line(value) -> str:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def patches(base: bytes, data: bytes) -> list:
    result = []
    cursor = 0
    while cursor < len(data):
        if base[cursor] == data[cursor]:
            cursor += 1
            continue
        start = cursor
        while cursor < len(data) and base[cursor] != data[cursor]:
            cursor += 1
        result.append([start, data[start:cursor].hex()])
    return result


@functools.lru_cache(maxsize=16384)
def byte_source(data: bytes):
    """Readable literal, repeated lane, byte ramp, or sparsely patched fill."""
    literal = data.hex()
    best = literal
    size = len(data)
    if size < 4:
        return best
    for width in (1, 2, 4, 8, 16, 32, 64, 128, 256):
        if width > size or size % width:
            continue
        units = [data[i:i + width] for i in range(0, size, width)]
        unit, frequency = collections.Counter(units).most_common(1)[0]
        if frequency * width < size * 0.8:
            continue
        base = unit * (size // width)
        candidate = {"repeat": unit.hex(), "size": size}
        changed = patches(base, data)
        if changed:
            candidate["patches"] = [[offset, byte_source(bytes.fromhex(value))]
                                    for offset, value in changed]
        if len(json_line(candidate)) < len(json_line(best)):
            best = candidate
        if base == data:
            break
    if size >= 16:
        for mask in (255, 127):
            sample_positions = range(0, size - 1, max(1, size // 512))
            step = collections.Counter((data[i + 1] - data[i]) & mask
                                       for i in sample_positions).most_common(1)[0][0]
            high = collections.Counter(data[i] & ~mask for i in sample_positions).most_common(1)[0][0]
            for shift in (None, 4, 5, 6, 7, 8, 9, 12, 14):
                block_step = 0
                if shift:
                    transitions = range((1 << shift) - 1, size - 1, 1 << shift)
                    if not transitions:
                        continue
                    block_step = collections.Counter(
                        (data[i + 1] - data[i] - step) & mask for i in transitions).most_common(1)[0][0]
                    if not block_step:
                        continue
                starts = collections.Counter((data[i] - i * step -
                                              (block_step * (i >> shift) if shift else 0)) & mask
                                             for i in sample_positions)
                start, frequency = starts.most_common(1)[0]
                if frequency * 5 < len(sample_positions) * 4:
                    continue
                base = bytes(((start + i * step + (block_step * (i >> shift) if shift else 0))
                              & mask) | high for i in range(size))
                candidate = {"ramp": [start, step], "size": size}
                if mask != 255:
                    candidate.update(mask=mask, high=high)
                if shift:
                    candidate["block_shift"] = shift
                    if block_step != 1:
                        candidate["block_step"] = block_step
                changed = patches(base, data)
                if changed:
                    candidate["patches"] = [[offset, byte_source(bytes.fromhex(value))]
                                            for offset, value in changed]
                if len(json_line(candidate)) < len(json_line(best)):
                    best = candidate
    return best


def byte_value(value) -> bytes:
    if isinstance(value, str):
        return bytes.fromhex(value)
    size = value["size"]
    if not isinstance(size, int) or not 0 <= size <= 64 * 1024 * 1024:
        raise ValueError("byte recipe size is out of range")
    if "chunks" in value:
        data = bytearray(b"".join(byte_value(chunk) for chunk in value["chunks"]))
        if len(data) != size:
            raise ValueError("byte chunk size mismatch")
    elif "loop" in value:
        data = bytearray()
        for block in loops.expand({"$table": value["loop"]}):
            data.extend(block)
        if len(data) != size:
            raise ValueError("byte loop size mismatch")
    elif "hash32" in value:
        start, multiplier = value["hash32"]
        data = bytearray(((start + i) * multiplier & 0xffffffff) >> 24
                         for i in range(size))
    elif "repeat" in value:
        unit = bytes.fromhex(value["repeat"])
        if not unit or size % len(unit):
            raise ValueError("byte recipe has an invalid repeat")
        data = bytearray(unit * (size // len(unit)))
    elif "ramp" in value:
        start, step = value["ramp"]
        mask, high = value.get("mask", 255), value.get("high", 0)
        shift = value.get("block_shift")
        block_step = value.get("block_step", 1)
        data = bytearray(((start + i * step + (block_step * (i >> shift) if shift else 0))
                          & mask) | high for i in range(size))
    else:
        raise ValueError("unknown byte recipe")
    end = 0
    for offset, text in value.get("patches", []):
        patch = byte_value(text)
        if offset < end or offset + len(patch) > size:
            raise ValueError("byte recipe patches overlap or exceed its size")
        data[offset:offset + len(patch)] = patch
        end = offset + len(patch)
    return bytes(data)


def program_source(data: bytes):
    """Factor long FEX assembler loops without requiring an assembler."""
    best = byte_source(data)
    if len(data) < 1024:
        return best
    chunks = []
    for start in range(0, len(data), 4096):
        chunk = data[start:start + 4096]
        recipe = byte_source(chunk)
        for width in (8, 32, 128, 256):
            if len(chunk) % width:
                continue
            rows = [list(chunk[i:i + width]) for i in range(0, len(chunk), width)]
            table = loops.table(rows)
            if table is not None:
                candidate = {"size": len(chunk), "loop": table["$table"]}
                if len(json_line(candidate)) < len(json_line(recipe)):
                    recipe = candidate
        chunks.append(recipe)
    candidate = {"size": len(data), "chunks": chunks}
    return candidate if len(json_line(candidate)) < len(json_line(best)) else best


def compact_bytes(value):
    """Numeric state fields use 0x; byte fields use hexadecimal byte order."""
    if isinstance(value, dict):
        return {key: compact_bytes(item) for key, item in value.items()}
    if isinstance(value, list):
        if len(value) >= 4 and all(isinstance(row, list) and len(row) == 2 and
                                    isinstance(row[0], int) and isinstance(row[1], str)
                                    for row in value):
            step = value[1][0] - value[0][0]
            if step > 0 and all(row == [value[0][0] + i * step, value[0][1]]
                                for i, row in enumerate(value)):
                return {"$runs": [value[0][0], step, len(value),
                                  byte_source(bytes.fromhex(value[0][1]))]}
        return [compact_bytes(item) for item in value]
    if isinstance(value, str) and len(value) >= 16 and not value.startswith("0x"):
        try:
            return byte_source(bytes.fromhex(value))
        except ValueError:
            pass
    return value


def expand_bytes(value):
    if isinstance(value, dict):
        if "$runs" in value:
            start, step, count, data = value["$runs"]
            return [[start + i * step, byte_value(data).hex()] for i in range(count)]
        if any(key in value for key in ("repeat", "ramp", "hash32", "chunks", "loop")):
            return byte_value(value).hex()
        result = {key: expand_bytes(item) for key, item in value.items()
                  if key not in ("$vectors", "$gprs", "$filled")}
        if "$filled" in value:
            for prefix, recipe in value["$filled"].items():
                for i in register_ids(recipe["registers"]):
                    result[f"{prefix}{i}"] = byte_value(recipe["bytes"]).hex()
        if "$vectors" in value:
            recipe = value["$vectors"]
            start, register_step, byte_step = recipe["affine"]
            for i in register_ids(recipe["registers"]):
                result[f"v{i}"] = bytes((start + i * register_step + b * byte_step) & 255
                                          for b in range(16)).hex()
        if "$gprs" in value:
            recipe = value["$gprs"]
            for i in register_ids(recipe["registers"]):
                if "add" in recipe:
                    start, step = (int(x, 16) for x in recipe["add"])
                    number = start + i * step
                elif "xor" in recipe:
                    start, step = (int(x, 16) for x in recipe["xor"])
                    number = start ^ (i * step & ((1 << 64) - 1))
                elif "pmull" in recipe:
                    seed, multiplier, stride = (int(x, 16) for x in recipe["pmull"])
                    number = (seed ^ ((i + 1) * multiplier & ((1 << 64) - 1))) + i * stride
                else:
                    mask, seed, step = (int(x, 16) for x in recipe["xor_multiply"])
                    number = mask ^ ((seed + i) * step & ((1 << 64) - 1))
                result[f"x{i}"] = f"0x{number & ((1 << 64) - 1):016x}"
        return result
    if isinstance(value, list):
        return [expand_bytes(item) for item in value]
    return value


def register_ids(value):
    if isinstance(value, list):
        return value
    mask = int(value, 16)
    return [i for i in range(mask.bit_length()) if mask >> i & 1]


def register_mask(indices):
    return hex(sum(1 << i for i in indices))


def state_source(state: dict) -> dict:
    """Generate regular register matrices; retain every irregular value."""
    result = dict(state)
    vectors = {int(key[1:]): bytes.fromhex(value) for key, value in state.items()
               if key.startswith("v") and key[1:].isdigit()}
    if len(vectors) >= 8:
        byte_steps = collections.Counter((value[1] - value[0]) & 255
                                         for value in vectors.values())
        byte_step = byte_steps.most_common(1)[0][0]
        register_steps = collections.Counter((vectors[i + 1][0] - vectors[i][0]) & 255
                                             for i in vectors if i + 1 in vectors)
        if register_steps:
            register_step = register_steps.most_common(1)[0][0]
            starts = collections.Counter((value[0] - i * register_step) & 255
                                          for i, value in vectors.items())
            start = starts.most_common(1)[0][0]
            selected = [i for i, value in vectors.items() if value == bytes(
                (start + i * register_step + b * byte_step) & 255 for b in range(16))]
            if len(selected) >= 8:
                result["$vectors"] = {"affine": [start, register_step, byte_step],
                                      "registers": register_mask(selected)}
                for i in selected:
                    del result[f"v{i}"]
    registers = {int(key[1:]): int(value, 16) for key, value in state.items()
                 if key.startswith("x") and key[1:].isdigit()}
    if len(registers) >= 8:
        mask64 = (1 << 64) - 1
        choices = []
        steps = collections.Counter((registers[i + 1] - registers[i]) & mask64
                                     for i in registers if i + 1 in registers)
        if steps:
            step = steps.most_common(1)[0][0]
            starts = collections.Counter((value - i * step) & mask64
                                          for i, value in registers.items())
            start = starts.most_common(1)[0][0]
            choices.append(({"add": [hex(start), hex(step)]},
                            lambda i: (start + i * step) & mask64))
        for xor_step in (0x101, 0x0101010101010101, 0x01010101):
            bases = collections.Counter(value ^ (i * xor_step & mask64)
                                         for i, value in registers.items())
            xor_start = bases.most_common(1)[0][0]
            choices.append(({"xor": [hex(xor_start), hex(xor_step)]},
                            lambda i, a=xor_start, b=xor_step: a ^ (i * b & mask64)))
        # The SHA3/PMULL producer's independently defined seeded register fill.
        xor_mask, multiplier = 0x1020304050607080, 0x0101010101010101
        inverse = pow(multiplier, -1, 1 << 64)
        seeds = collections.Counter(((value ^ xor_mask) * inverse - i) & mask64
                                    for i, value in registers.items())
        seed = seeds.most_common(1)[0][0]
        choices.append(({"xor_multiply": [hex(xor_mask), hex(seed), hex(multiplier)]},
                        lambda i, a=xor_mask, b=seed, c=multiplier:
                        a ^ ((b + i) * c & mask64)))
        multiplier, stride = 0x9e3779b97f4a7c15, 0x0102030405060708
        seeds = collections.Counter(((value - i * stride) & mask64) ^
                                    ((i + 1) * multiplier & mask64)
                                    for i, value in registers.items())
        seed = seeds.most_common(1)[0][0]
        choices.append(({"pmull": [hex(seed), hex(multiplier), hex(stride)]},
                        lambda i, a=seed, b=multiplier, c=stride:
                        ((a ^ ((i + 1) * b & mask64)) + i * c) & mask64))
        recipe, selected = max(((recipe, [i for i, value in registers.items()
                                         if value == calculate(i)])
                                for recipe, calculate in choices), key=lambda x: len(x[1]))
        if len(selected) >= 8:
            result["$gprs"] = dict(recipe, registers=register_mask(selected))
            for i in selected:
                del result[f"x{i}"]
    for prefix in ("v", "z", "p", "za"):
        fields = {int(key[len(prefix):]): value for key, value in result.items()
                  if key.startswith(prefix) and key[len(prefix):].isdigit()}
        if len(fields) < 4:
            continue
        common, _ = collections.Counter(fields.values()).most_common(1)[0]
        selected = [i for i, value in fields.items() if value == common]
        if len(selected) >= 4:
            result.setdefault("$filled", {})[prefix] = {
                "registers": register_mask(selected), "bytes": byte_source(bytes.fromhex(common))}
            for i in selected:
                del result[f"{prefix}{i}"]
    return compact_bytes(result)


def defaults(rows: list[dict]) -> dict:
    result = {}
    keys = dict.fromkeys(key for row in rows for key in row)
    for key in keys:
        counts = collections.Counter(json_line(row.get(key)) for row in rows)
        value, count = counts.most_common(1)[0]
        if count >= 2 and count * 2 >= len(rows) and value != "null":
            result[key] = json.loads(value)
    return result


def difference(value: dict, base: dict) -> dict:
    result = {key: item for key, item in value.items()
              if key not in base or base[key] != item}
    missing = [key for key in base if key not in value]
    if missing:
        result["$omit"] = missing
    return result


def overlay(base: dict, delta: dict) -> dict:
    result = dict(base)
    for key in delta.get("$omit", []):
        del result[key]
    result.update((key, value) for key, value in delta.items() if key != "$omit")
    return result


def source_file(root: pathlib.Path, name: str) -> pathlib.Path:
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        raise ValueError("source index contains a path outside its directory")
    return root / path


def write_index(root: pathlib.Path, index: dict) -> None:
    path = root / "index.json"
    previous = json.loads(path.read_text())["files"] if path.is_file() else []
    path.write_text(json.dumps(index, indent=2) + "\n")
    for name in set(previous) - set(index["files"]):
        source_file(root, name).unlink(missing_ok=True)


def arm_builder():
    return load("instruction_corpus_arm64", ROOT / "tools/arm64-tests/build-unit-corpus.py")


def native_memory_recipes() -> dict:
    recipes = {}
    module = load("sme_native_corpus", ROOT / "tools/arm64-tests/sme-native-corpus.py")
    regions = {(region.seed, region.size) for draft in module.drafts()
               for region in draft.regions if region.data is None}
    for seed, size in sorted(regions):
        start = seed * 0x01000193 & 0xffffffff
        recipe = {"hash32": [start, 0x9e3779b1], "size": size}
        recipes[hashlib.sha256(byte_value(recipe)).hexdigest()] = recipe
    return recipes


@functools.cache
def native_templates() -> dict:
    """Reuse maintained allocation loops; observations are separate inputs."""
    records = {}
    module = load("sme_native_corpus", ROOT / "tools/arm64-tests/sme-native-corpus.py")
    items = []
    for draft in module.drafts():
        case = draft.wire()
        items.append((draft, case.words, module.case_skeleton(draft, case, 0)))
    for record in module.assemble(items):
        records[record["record"]] = record
    return records


def import_arm64(pack: pathlib.Path, root: pathlib.Path) -> dict:
    builder = arm_builder()
    corpus, tokens = builder.decode_pack(pack.read_bytes())
    root.mkdir(parents=True, exist_ok=True)
    index = {"schema": 1, "kind": "arm64", "feature_tokens": tokens,
             "meta": corpus.meta,
             "files": ["memory.jsonl"], "records": []}
    recipes = native_memory_recipes()
    loops.write(root / "memory.jsonl", [
        {"sha256": digest, "bytes": recipes[digest] if digest in recipes else byte_source(data)}
        for digest, data in corpus.blobs.items()])
    generated = native_templates()
    producers = collections.defaultdict(list)
    for record in corpus.records:
        producers[record.producer].append(record)
    for producer, records in producers.items():
        name = producer + ".jsonl"
        index["files"].append(name)
        source_rows = []
        for record in records:
            # dump_corpus's first row is metadata; the record follows it.
            dumped = json.loads(list(builder.dump_corpus(
                builder.Corpus(meta={}, blobs={}, records=[record]), tokens))[1])
            template = generated.get(record.name)
            if template is not None:
                if dumped["programs"] != template["programs"] or \
                        len(dumped["cases"]) != len(template["cases"]):
                    raise ValueError("native generator programs or case count differ")
                golden = []
                for ordinal, (case, base) in enumerate(zip(dumped["cases"], template["cases"])):
                    if case["name"] != base["name"]:
                        raise ValueError("native generator case order differs")
                    delta = difference(case, base)
                    if "memory" in delta:
                        delta["memory"] = [difference(region, reference) for region, reference
                                           in zip(case["memory"], base["memory"])]
                    if "initial" in delta or "program" in delta or "$omit" in delta:
                        raise ValueError("native generator input differs from the accepted case")
                    golden.append(compact_bytes({k: v for k, v in delta.items() if k != "expected"}) |
                                  {"expected": state_source(delta["expected"]), "case": ordinal})
                base = defaults(golden)
                source_rows.append({"record": record.name, "generator": producer,
                                    "defaults": base})
                source_rows.extend(difference(case, base) for case in golden)
                index["records"].append({"name": record.name, "file": name})
                continue
            rows = []
            for case in dumped.pop("cases"):
                case = compact_bytes({k: v for k, v in case.items()
                                      if k not in ("initial", "expected")}) | {
                    key: state_source(case[key]) for key in ("initial", "expected")}
                rows.append(case)
            programs = dumped.pop("programs")
            state_defaults = {key: defaults([row[key] for row in rows])
                              for key in ("initial", "expected")}
            memory_count = max((len(row.get("memory", [])) for row in rows), default=0)
            memory_defaults = [defaults([row["memory"][i] for row in rows
                                        if len(row.get("memory", [])) > i])
                               for i in range(memory_count)]
            base = defaults([{k: v for k, v in row.items()
                              if k not in state_defaults and k != "name"}
                             for row in rows])
            dumped["defaults"] = base
            dumped["state_defaults"] = state_defaults
            if memory_defaults:
                dumped["memory_defaults"] = memory_defaults
            source_rows.append(dumped)
            for i, program in enumerate(programs):
                source_rows.append(dict(program, program=i))
            for row in rows:
                item = difference({k: v for k, v in row.items()
                                   if k not in state_defaults}, base)
                for key, value in state_defaults.items():
                    delta = difference(row[key], value)
                    if delta:
                        item[key] = delta
                if "memory" in item:
                    item["memory"] = [difference(region, memory_defaults[i])
                                      for i, region in enumerate(item["memory"])]
                source_rows.append(dict(item, case=item.pop("name")))
            index["records"].append({"name": record.name, "file": name})
        loops.write(source_file(root, name), source_rows)
    order = {record.name: i for i, record in enumerate(corpus.records)}
    index["records"].sort(key=lambda item: order[item["name"]])
    write_index(root, index)
    return index


def generate_arm64(root: pathlib.Path, index: dict) -> bytes:
    builder = arm_builder()
    blobs = {}
    records = {}
    for name in index["files"]:
        record = None
        for item in loops.read(source_file(root, name)):
            if "sha256" in item:
                data = byte_value(item["bytes"])
                if hashlib.sha256(data).hexdigest() != item["sha256"]:
                    raise ValueError("memory recipe digest mismatch")
                blobs[item["sha256"]] = data
            elif "record" in item:
                if "generator" in item:
                    record = copy.deepcopy(native_templates()[item["record"]])
                    record.update(generator=item["generator"], defaults=item["defaults"], golden=0)
                else:
                    record = dict(item, programs=[], cases=[])
                records[item["record"]] = record
            elif "program" in item and "case" not in item:
                if record is None or item.pop("program") != len(record["programs"]):
                    raise ValueError("program is out of order")
                record["programs"].append(item)
            else:
                if record is None:
                    raise ValueError("case precedes its record")
                if "generator" in record:
                    delta = expand_bytes(overlay(record["defaults"], item))
                    ordinal = delta.pop("case")
                    if ordinal != record["golden"]:
                        raise ValueError("native golden observation is out of order")
                    base = record["cases"][ordinal]
                    if "memory" in delta:
                        delta["memory"] = [overlay(region, patch) for region, patch
                                           in zip(base["memory"], delta["memory"])]
                    record["cases"][ordinal] = overlay(base, delta)
                    record["golden"] += 1
                    continue
                states = record["state_defaults"]
                case = overlay(record["defaults"],
                               {k: v for k, v in item.items() if k not in states})
                case["name"] = case.pop("case")
                for key, base in states.items():
                    case[key] = overlay(base, item.get(key, {}))
                if "memory" in item:
                    case["memory"] = [overlay(record["memory_defaults"][i], region)
                                      for i, region in enumerate(item["memory"])]
                record["cases"].append(expand_bytes(case))
    corpus = builder.Corpus(meta=index["meta"], blobs=blobs, records=[])
    for entry in index["records"]:
        item = records[entry["name"]]
        if "generator" in item and item.pop("golden") != len(item["cases"]):
            raise ValueError("native golden observation count differs from its generator")
        item.pop("defaults", None)
        item.pop("state_defaults", None)
        item.pop("memory_defaults", None)
        record = builder.Record(
            name=item["record"], producer=item["producer"], site=item["site"],
            options=sum(1 << builder.OPTION_NAMES.index(x) for x in item["options"]),
            profiles=sum({"16k": 1, "4k": 2}[x] for x in item["profiles"]),
            svl=item.get("svl", 0))
        record.programs = [(p["entry"], tuple((o, int(w, 16)) for o, w in p["words"]))
                           for p in item["programs"]]
        for case in item["cases"]:
            entry_offset, words = record.programs[case["program"]]
            record.cases.append((case["program"], builder.load_case(
                case, entry_offset, words, blobs)))
        corpus.records.append(record)
    return builder.encode_pack(corpus, tuple(index["feature_tokens"]))


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.offset = 0

    def take(self, size: int) -> bytes:
        if size < 0 or self.offset + size > len(self.data):
            raise ValueError("truncated corpus input")
        result = self.data[self.offset:self.offset + size]
        self.offset += size
        return result

    def unpack(self, fmt: str):
        return struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))

    def finish(self):
        if self.offset != len(self.data):
            raise ValueError("unexpected trailing corpus input")


def x86_builder():
    return load("instruction_corpus_x86", ROOT / "tools/x86-tests/build-unit-corpus.py")


def configuration_source(data: bytes) -> dict:
    reader = Reader(data)
    header = reader.unpack("QQQQIII8I")
    result = {"header": list(header[:7]), "regions": [], "registers": [],
              "memory": [], "environment": []}
    names = ["RIP", *x86_builder().GPR_NAMES,
             *(f"XMM{i}" for i in range(16))]
    for section, offset, count in zip(
            ("regions", "registers", "memory", "environment"),
            header[7::2], header[8::2]):
        if reader.offset != offset:
            raise ValueError("noncanonical configuration section offset")
        for _ in range(count):
            if section == "regions":
                address, size = reader.unpack("QQ")
                item = [hex(address), size]
            elif section == "registers":
                size, key = reader.unpack("IQ")
                bit = key.bit_length() - 1
                name = names[bit] if key == 1 << bit and bit < len(names) else hex(key)
                item = [name, [hex(x) for x in reader.unpack(f"{size}Q")]]
            elif section == "memory":
                address, size = reader.unpack("QI")
                item = [hex(address), byte_source(reader.take(size))]
            else:
                item = []
                for _field in range(2):
                    end = data.index(b"\0", reader.offset)
                    item.append(reader.take(end - reader.offset).decode())
                    reader.take(1)
            result[section].append(item)
    reader.finish()
    return result


def configuration_value(item: dict) -> bytes:
    builder = x86_builder()
    regions = b"".join(struct.pack("<QQ", int(a, 16), size)
                       for a, size in item["regions"])
    registers = b""
    for name, values in item["registers"]:
        key = int(name, 16) if name.startswith("0x") else builder.register_key(name)
        registers += struct.pack("<IQ", len(values), key)
        registers += struct.pack(f"<{len(values)}Q", *(int(x, 16) for x in values))
    memory = b""
    for address, value in item["memory"]:
        data = byte_value(value)
        memory += struct.pack("<QI", int(address, 16), len(data)) + data
    environment = b"".join(k.encode() + b"\0" + v.encode() + b"\0"
                           for k, v in item["environment"])
    sections = (regions, registers, memory, environment)
    header = list(item["header"])
    offset = 76
    for section, name in zip(sections, ("regions", "registers", "memory", "environment")):
        header.extend((offset, len(item[name])))
        offset += len(section)
    return struct.pack("<QQQQIII8I", *header) + b"".join(sections)


def trace_source(data: bytes) -> tuple[list[int], list[dict]]:
    reader = Reader(data)
    instructions, count, offsets = reader.unpack("III")
    positions = list(reader.unpack(f"{offsets}I"))
    if instructions != offsets - 1:
        raise ValueError("invalid instruction offset count")
    states = []
    names = x86_builder().GPR_NAMES
    for _ in range(count):
        rip, gv, gc, vv, vc, fv, fc, flags, event = reader.unpack("IHHHHBBBB")
        registers = {name: hex(reader.unpack("Q")[0]) for i, name in enumerate(names)
                     if gc >> i & 1}
        vectors = {f"YMM{i}": byte_source(reader.take(32)) for i in range(16)
                   if vc >> i & 1}
        states.append({"rip": rip, "valid": [gv, vv, fv], "flags": [fc, flags],
                       "event": event, "registers": registers, "vectors": vectors})
    reader.finish()
    return positions, states


def trace_value(offsets: list[int], states: list[dict]) -> bytes:
    data = bytearray(struct.pack("<III", len(offsets) - 1, len(states), len(offsets)))
    data += struct.pack(f"<{len(offsets)}I", *offsets)
    names = x86_builder().GPR_NAMES
    for state in states:
        gv, vv, fv = state["valid"]
        fc, flags = state["flags"]
        gc = sum((name in state["registers"]) << i for i, name in enumerate(names))
        vc = sum((f"YMM{i}" in state["vectors"]) << i for i in range(16))
        data += struct.pack("<IHHHHBBBB", state["rip"], gv, gc, vv, vc,
                            fv, fc, flags, state["event"])
        for name in names:
            if name in state["registers"]:
                data += struct.pack("<Q", int(state["registers"][name], 16))
        for i in range(16):
            if f"YMM{i}" in state["vectors"]:
                vector = byte_value(state["vectors"][f"YMM{i}"])
                if len(vector) != 32:
                    raise ValueError("trace vector is not 32 bytes")
                data += vector
    return bytes(data)


def import_x86(pack: pathlib.Path, root: pathlib.Path) -> dict:
    builder = x86_builder()
    reader = Reader(pack.read_bytes())
    if reader.take(8) != builder.PACK_MAGIC:
        raise ValueError("not an x86 corpus pack")
    version, count = reader.unpack("II")
    if version != builder.PACK_VERSION:
        raise ValueError("unsupported x86 corpus version")
    root.mkdir(parents=True, exist_ok=True)
    index = {"schema": 1, "kind": "x86", "files": [], "records": []}
    files = collections.defaultdict(list)
    for _ in range(count):
        kind, flags, reserved, nl, sl, cl, pl, cases = reader.unpack("BBHHHIII")
        if reserved:
            raise ValueError("reserved x86 record flags")
        name, source = reader.take(nl).decode(), reader.take(sl).decode()
        code, payload = reader.take(cl), reader.take(pl)
        origin = next(k for k, value in builder.PACK_KINDS.items() if value == kind)
        item = {"record": name, "source": source, "origin": origin, "flags": flags,
                "cases": cases, "code": program_source(code)}
        parent = pathlib.PurePosixPath(source).parent.as_posix().replace("/", "-")
        file = f"{origin}-{parent}.jsonl"
        rows = []
        if origin == "k":
            offsets, states = trace_source(payload)
            item["instruction_offsets"] = offsets
            item["defaults"] = defaults(states)
            rows = [dict(difference(state, item["defaults"]), step=i)
                    for i, state in enumerate(states)]
        else:
            item["configuration"] = configuration_source(payload)
        files[file].extend([item, *rows])
        index["records"].append({"name": name, "file": file})
    reader.finish()
    for file, rows in files.items():
        index["files"].append(file)
        loops.write(source_file(root, file), rows)
    write_index(root, index)
    return index


def generate_x86(root: pathlib.Path, index: dict) -> bytes:
    builder = x86_builder()
    records = {}
    for file in index["files"]:
        current = None
        for item in loops.read(source_file(root, file)):
            if "record" in item:
                current = item
                item["states"] = []
                records[item["record"]] = item
            else:
                if current is None or item.pop("step") != len(current["states"]):
                    raise ValueError("trace step is out of order")
                current["states"].append(overlay(current["defaults"], item))
    result = []
    for row in index["records"]:
        item = records[row["name"]]
        payload = trace_value(item["instruction_offsets"], item["states"]) \
            if item["origin"] == "k" else configuration_value(item["configuration"])
        result.append(builder.Record(
            name=item["record"], source=item["source"], origin=item["origin"],
            code=byte_value(item["code"]), payload=payload, case_count=item["cases"],
            forms=(), oracle="", upstream_disabled=bool(item["flags"] & 1),
            nan_class_equivalence=bool(item["flags"] & 2)))
    return builder.encode_pack(result)


def import_masked(pack: pathlib.Path, root: pathlib.Path) -> dict:
    reader = Reader(pack.read_bytes())
    if reader.take(4) != b"MXF1":
        raise ValueError("not a masked-flags dataset")
    count, = reader.unpack("I")
    instructions = []
    for _ in range(count):
        size, = reader.unpack("B")
        name = reader.take(size).decode()
        size, = reader.unpack("B")
        code = reader.take(size).hex()
        kept, = reader.unpack("B")
        instructions.append({"name": name, "encoding": code, "register_bytes": kept,
                             "cases": []})
    count, = reader.unpack("I")
    order = []
    for _ in range(count):
        instruction, mxcsr, flags = reader.unpack("HHB")
        row = instructions[instruction]
        inputs = [byte_source(reader.take(row["register_bytes"])) for _ in range(3)]
        order.append([instruction, len(row["cases"])])
        row["cases"].append({"mxcsr": hex(mxcsr), "flags": hex(flags), "inputs": inputs})
    reader.finish()
    root.mkdir(parents=True, exist_ok=True)
    order_recipe = loops.table(order) or order
    runs = []
    for instruction, group in itertools.groupby(order, key=lambda row: row[0]):
        cases = [row[1] for row in group]
        runs.append([len(cases), [instruction, loops.series(cases)]])
    grouped = {"$table": {"count": len(order), "row": {"$segments": runs}}}
    if order and len(json_line(grouped)) < len(json_line(order_recipe)):
        order_recipe = grouped
    index = {"schema": 1, "kind": "masked-flags", "files": [],
             "order": order_recipe}
    for i, row in enumerate(instructions):
        name = f"{i:04d}-{row['name']}.jsonl"
        index["files"].append(name)
        cases = row.pop("cases")
        loops.write(source_file(root, name), [row, *cases])
    write_index(root, index)
    return index


def generate_masked(root: pathlib.Path, index: dict) -> bytes:
    instructions = []
    data = bytearray(b"MXF1" + struct.pack("<I", len(index["files"])))
    for file in index["files"]:
        source = iter(loops.read(source_file(root, file)))
        row = next(source)
        name = row["name"].encode()
        code = bytes.fromhex(row["encoding"])
        kept = row["register_bytes"]
        if kept not in (16, 32):
            raise ValueError("invalid masked-flags register width")
        data += bytes([len(name)]) + name + bytes([len(code)]) + code + bytes([kept])
        instructions.append((kept, list(source)))
    order = list(loops.expand(index["order"])) if isinstance(index["order"], dict) else index["order"]
    data += struct.pack("<I", len(order))
    for instruction, case in order:
        kept, rows = instructions[instruction]
        row = rows[case]
        data += struct.pack("<HHB", instruction, int(row["mxcsr"], 16), int(row["flags"], 16))
        for value in row["inputs"]:
            inputs = byte_value(value)
            if len(inputs) != kept:
                raise ValueError("invalid masked-flags input width")
            data += inputs
    return bytes(data)


def generate_aarch32(root: pathlib.Path, index: dict) -> bytes:
    return load("aarch32", pathlib.Path(__file__).with_name("aarch32.py")).generate(root, index)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--import-pack", type=pathlib.Path,
                        help="replace source tables from an accepted pack")
    parser.add_argument("--kind", choices=("arm64", "x86", "masked-flags"))
    parser.add_argument("--output", type=pathlib.Path,
                        help="generated asset directory; never the source directory")
    args = parser.parse_args()
    if args.import_pack:
        if args.kind is None:
            parser.error("an import requires --kind")
        receipt = json.loads(args.import_pack.with_name("provenance.json").read_text())
        expected = receipt.get("pack_sha256", receipt.get("subset_sha256"))
        if hashlib.sha256(args.import_pack.read_bytes()).hexdigest() != expected:
            raise ValueError("import differs from its accepted provenance")
        {"arm64": import_arm64, "x86": import_x86,
         "masked-flags": import_masked}[args.kind](args.import_pack, args.source)
        for name in ("provenance.json", "manifest.tsv", "coverage.tsv", "excluded.tsv"):
            path = args.import_pack.with_name(name)
            destination = args.source.parent / name
            if path.is_file() and path.resolve() != destination.resolve():
                shutil.copyfile(path, destination)
        return 0
    if args.output is None:
        parser.error("generation requires --output")
    source_directory = args.source.parent.resolve()
    if args.output.resolve() == source_directory or \
            source_directory in args.output.resolve().parents:
        parser.error("generated output must be outside the corpus source directory")
    index = json.loads((args.source / "index.json").read_text())
    if index["schema"] != 1:
        parser.error("unsupported source index")
    provenance = args.source.parent / "provenance.json"
    metadata = json.loads(provenance.read_text())
    data = {"arm64": generate_arm64, "x86": generate_x86,
            "masked-flags": generate_masked,
            "aarch32": generate_aarch32}[index["kind"]](args.source, index)
    if hashlib.sha256(data).hexdigest() != metadata.get("pack_sha256", metadata.get("subset_sha256")):
        raise ValueError("generated corpus differs from its accepted provenance")
    args.output.mkdir(parents=True, exist_ok=True)
    for name in ("provenance.json", "manifest.tsv", "coverage.tsv", "excluded.tsv"):
        path = args.source.parent / name
        if path.is_file():
            shutil.copyfile(path, args.output / name)
    destination = args.output / {"arm64": "a64_instruction_corpus.pack",
        "x86": "x86_64_v3.pack", "masked-flags": "xeon-flags.bin",
        "aarch32": "a32_instruction_corpus.pack"}[index["kind"]]
    temporary = destination.with_suffix(".tmp")
    temporary.write_bytes(data)
    temporary.replace(destination)
    print(f"generated {metadata['cases']} cases: {destination}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
