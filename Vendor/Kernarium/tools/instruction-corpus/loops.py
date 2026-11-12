# SPDX-License-Identifier: GPL-2.0-or-later
"""Readable table loops: constants, counters, bit fields and golden columns.

Expressions generate test data only. Irregular observations remain literal
values; no expression evaluates an instruction or calls an execution engine.
"""

from __future__ import annotations

import copy
import json
import re

MISSING = object()
OPERATORS = frozenset({"$values", "$cycle", "$counter", "$hex", "$format", "$bits",
                       "$pool", "$when", "$segments", "$literal", "$table"})


def contains_expression(value):
    if isinstance(value, dict):
        return bool(value.keys() & OPERATORS) or any(contains_expression(v) for v in value.values())
    return isinstance(value, list) and any(contains_expression(v) for v in value)


def text(value) -> str:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def period(values: list) -> int:
    """Shortest repeating prefix, including a partially repeated last cycle."""
    lengths = [0] * len(values)
    for i in range(1, len(values)):
        length = lengths[i - 1]
        while length and values[i] != values[length]:
            length = lengths[length - 1]
        if values[i] == values[length]:
            length += 1
        lengths[i] = length
    return len(values) - lengths[-1]


def numbers(values: list[int], allow_bits: bool = True):
    candidates = [{"$values": values}]
    step = values[1] - values[0]
    if all(value == values[0] + i * step for i, value in enumerate(values)):
        candidates.append({"$counter": [values[0], step]})
    for bits in (8, 16, 32, 64):
        mask = (1 << bits) - 1
        if min(values) < 0 or max(values) > mask:
            continue
        step = (values[1] - values[0]) & mask
        if all(value == (values[0] + i * step) & mask for i, value in enumerate(values)):
            candidates.append({"$counter": [values[0], step, bits]})
            break
    divisor = next((i for i, value in enumerate(values) if value != values[0]), len(values))
    reduced = values[::divisor]
    if len(reduced) >= 2:
        length = period(reduced)
        step = reduced[1] - reduced[0]
        if all(value == values[0] + step * ((i // divisor) % length)
               for i, value in enumerate(values)):
            candidates.append({"$counter": [values[0], step, 0, divisor, length]})
    if allow_bits and min(values) >= 0 and max(values) < 1 << 64:
        varying = 0
        for value in values:
            varying |= value ^ values[0]
        fields = []
        shift = 0
        while varying:
            if not varying & 1:
                varying >>= 1
                shift += 1
                continue
            width = 0
            while varying & 1:
                varying >>= 1
                width += 1
            mask = (1 << width) - 1
            column = [(value >> shift) & mask for value in values]
            fields.append([shift, width, series(column, allow_bits=False)])
            shift += width
        if len(fields) > 1:
            mask = sum(((1 << width) - 1) << shift for shift, width, _ in fields)
            candidates.append({"$bits": [values[0] & ~mask, fields]})
        # Adjacent A64 operands can make one continuous varying bit mask.
        # Split at architectural register-field boundaries, or at single
        # bits for masks and counters, and retain the shortest exact recipe.
        for boundaries in ((0, 5, 10, 16, 20, 22, 24, 26, 28, 30, 32, 64),
                           tuple(range(65))):
            fields = []
            for left, right in zip(boundaries, boundaries[1:]):
                if left >= max(values).bit_length():
                    break
                mask = (1 << (right - left)) - 1
                column = [(value >> left) & mask for value in values]
                if min(column) != max(column):
                    fields.append([left, right - left, series(column, allow_bits=False,
                                                             allow_segments=False)])
            if fields:
                mask = sum(((1 << width) - 1) << shift for shift, width, _ in fields)
                candidates.append({"$bits": [values[0] & ~mask, fields]})
    return min(candidates, key=lambda value: len(text(value)))


def strings(values: list[str]):
    candidates = [{"$values": values}]
    if max(map(len, values)) <= 18 and all(
            re.fullmatch(r"(?:0x)?[0-9a-f]+", value) for value in values):
        prefix = "0x" if values[0].startswith("0x") else ""
        width = len(values[0]) - len(prefix)
        if all(value.startswith(prefix) and len(value) == width + len(prefix)
               for value in values):
            integers = [int(value.removeprefix(prefix), 16) for value in values]
            expression = series(integers)
            if not isinstance(expression, dict) or "$values" not in expression:
                candidates.append({"$hex": [prefix, width, expression]})
    # Case names describe allocation dimensions. Keep their text once and
    # generate decimal fields with the same table counters as the operands.
    if max(map(len, values)) > 512:
        return min(candidates, key=lambda value: len(text(value)))
    tokens = [re.split(r"(\d+)", value) for value in values]
    if len(tokens[0]) > 1 and all(len(row) == len(tokens[0]) for row in tokens):
        if all(all(row[i] == tokens[0][i] for row in tokens)
               for i in range(0, len(tokens[0]), 2)):
            pieces = []
            for i in range(len(tokens[0])):
                column = [row[i] for row in tokens]
                if i % 2:
                    if all(str(int(value)) == value for value in column):
                        pieces.append(series([int(value) for value in column]))
                    else:
                        pieces.append({"$values": column})
                else:
                    pieces.append(column[0])
            candidates.append({"$format": pieces})
    return min(candidates, key=lambda value: len(text(value)))


def series(values: list, allow_bits: bool = True, allow_segments: bool = True):
    if not values:
        raise ValueError("empty loop column")
    if all(value == values[0] for value in values):
        return {"$literal": values[0]} if contains_expression(values[0]) else values[0]
    candidates = [{"$values": values}]
    length = period(values)
    if length < len(values):
        candidates.append({"$cycle": values[:length]})
    if all(type(value) is int for value in values):
        candidates.append(numbers(values, allow_bits))
    elif all(isinstance(value, str) for value in values):
        candidates.append(strings(values))
    elif all(isinstance(value, dict) and not value.keys() & OPERATORS for value in values):
        keys = dict.fromkeys(key for value in values for key in value)
        body = {}
        for key in keys:
            present = [key in value for value in values]
            column = [value[key] for value in values if key in value]
            expression = series(column)
            body[key] = expression if all(present) else {
                "$when": [series(present), expression]}
        candidates.append(body)
    elif all(isinstance(value, list) and len(value) == len(values[0]) for value in values):
        candidates.append([series([value[i] for value in values])
                           for i in range(len(values[0]))])
    elif all(isinstance(value, list) for value in values):
        # Keep allocation matrices together when their final program or
        # memory run is shorter than the preceding full-sized batches.
        groups = []
        for value in values:
            if not groups or len(value) != len(groups[-1][0]):
                groups.append([])
            groups[-1].append(value)
        if len(groups) < len(values) // 2:
            candidates.append({"$segments": [[len(group), series(group)] for group in groups]})
    if allow_segments and len(values) > 512 and all(
            isinstance(value, (int, str, bool)) for value in values):
        candidates.append({"$segments": [[len(values[i:i + 256]),
            series(values[i:i + 256], allow_bits, allow_segments=False)]
            for i in range(0, len(values), 256)]})
    if len(values) >= 4:
        # A shared operand pool is useful for irregular hardware inputs and
        # observations, whose values must not be guessed by a semantic model.
        pool, positions, seen = [], [], {}
        for value in values:
            key = text(value)
            if key not in seen:
                seen[key] = len(pool)
                pool.append(value)
            positions.append(seen[key])
        if len(pool) * 2 < len(values):
            candidates.append({"$pool": [pool, numbers(positions, allow_bits=False)]})
    return min(candidates, key=lambda value: len(text(value)))


def compile_expression(expression, count: int):
    """Compile once per table so conditional columns use linear-time ranks."""
    if isinstance(expression, dict):
        if "$literal" in expression:
            return lambda i: copy.deepcopy(expression["$literal"])
        if "$segments" in expression:
            positions = []
            for size, body in expression["$segments"]:
                if not isinstance(size, int) or size <= 0:
                    raise ValueError("invalid loop segment size")
                evaluate = compile_expression(body, size)
                positions.extend((evaluate, i) for i in range(size))
            if len(positions) != count:
                raise ValueError("loop segment length mismatch")
            return lambda i: positions[i][0](positions[i][1])
        if "$values" in expression or "$cycle" in expression:
            cycling = "$cycle" in expression
            values = expression["$cycle" if cycling else "$values"]
            if not values or not cycling and len(values) != count:
                raise ValueError("loop column length mismatch")
            return lambda i: copy.deepcopy(values[i % len(values)])
        if "$counter" in expression:
            fields = expression["$counter"] + [0] * (5 - len(expression["$counter"]))
            start, step, bits, divisor, modulus = fields
            if divisor < 0 or modulus < 0 or not 0 <= bits <= 256:
                raise ValueError("invalid loop counter")
            def counter(i):
                index = i // (divisor or 1)
                if modulus:
                    index %= modulus
                value = start + step * index
                return value & ((1 << bits) - 1) if bits else value
            return counter
        if "$hex" in expression:
            prefix, width, column = expression["$hex"]
            evaluate = compile_expression(column, count)
            return lambda i: prefix + format(evaluate(i), f"0{width}x")
        if "$format" in expression:
            parts = [compile_expression(part, count) for part in expression["$format"]]
            return lambda i: "".join(str(part(i)) for part in parts)
        if "$bits" in expression:
            base, fields = expression["$bits"]
            parts = [(shift, width, compile_expression(column, count))
                     for shift, width, column in fields]
            def bits(i):
                value = base
                for shift, width, part in parts:
                    field = part(i)
                    if not 0 <= field < 1 << width:
                        raise ValueError("loop value exceeds its bit field")
                    value |= field << shift
                return value
            return bits
        if "$pool" in expression:
            pool, column = expression["$pool"]
            evaluate = compile_expression(column, count)
            return lambda i: copy.deepcopy(pool[evaluate(i)])
        if "$when" in expression:
            presence, column = expression["$when"]
            present = compile_expression(presence, count)
            ranks, rank = [], 0
            for i in range(count):
                active = present(i)
                if type(active) is not bool:
                    raise ValueError("loop condition is not boolean")
                ranks.append(rank if active else -1)
                rank += active
            values = compile_expression(column, rank)
            return lambda i: values(ranks[i]) if ranks[i] >= 0 else MISSING
        fields = {key: compile_expression(value, count) for key, value in expression.items()}
        def object_row(i):
            result = {}
            for key, value in fields.items():
                item = value(i)
                if item is not MISSING:
                    result[key] = item
            return result
        return object_row
    if isinstance(expression, list):
        items = [compile_expression(item, count) for item in expression]
        return lambda i: [item(i) for item in items]
    return lambda i: expression


def table(rows: list):
    if len(rows) < 4:
        return None
    expression = series(rows)
    result = {"$table": {"count": len(rows), "row": expression}}
    return result if len(text(result)) < sum(len(text(row)) + 1 for row in rows) else None


def expand(value):
    if not isinstance(value, dict) or "$table" not in value:
        yield value
        return
    count = value["$table"]["count"]
    if not isinstance(count, int) or not 0 < count <= 1_000_000:
        raise ValueError("table loop count is out of range")
    evaluate = compile_expression(value["$table"]["row"], count)
    for i in range(count):
        yield evaluate(i)


def read(path):
    with path.open() as source:
        for line in source:
            yield from expand(json.loads(line))


def category(row: dict) -> str:
    return next((key for key in ("record", "sha256", "case", "step", "program")
                 if key in row), "operand")


def write(path, rows):
    """Keep record boundaries visible; replace matrices with bounded loops."""
    group = []
    with path.open("w") as sink:
        def flush():
            if not group:
                return
            recipe = table(group)
            for row in [recipe] if recipe else group:
                sink.write(text(row) + "\n")
            group.clear()
        for row in rows:
            if group and category(row) != category(group[0]):
                flush()
            group.append(row)
        flush()
