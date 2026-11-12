#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The AArch32 corpus oracle's request and response, and the cases they carry.

tools/arm64-tests/a32_oracle.cpp documents both formats. A Case is the
producer's description of one run: the program, the state it starts in and
how the data region starts. An Observation is what the oracle recorded.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import pathlib
import struct
import sys
from dataclasses import dataclass, field

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "instruction-corpus"))
import aarch32 as corpus  # noqa: E402


REQUEST_MAGIC = b"A32ORQ1\0"
RESPONSE_MAGIC = b"A32ORS1\0"


@dataclass
class Case:
    suite: str
    name: str
    form: str
    isa: str  # "A" or "T"
    program: tuple[int, ...]  # A32 words, or Thumb halfwords
    target: int  # ordinal of the instruction under test
    recipe: str
    initial: dict[str, int] = field(default_factory=dict)  # changes to the recipe
    fill: int = corpus.FILL_HASH32
    data_patches: list[tuple[int, bytes]] = field(default_factory=list)
    straight: bool = True
    retired: int | None = None  # instructions retired when the program finishes
    fault_retired: int | None = None  # instructions retired before a fault outside the program
    path: int | None = None  # fillers the program must execute (a check on the plan)
    expect: dict[str, int] | None = None  # register increments the plan predicts
    predicted: int | None = None  # the event the profile defines; no oracle runs the case
    origin: int = 0  # byte offset of the program in the code region

    def halfwords(self) -> list[int]:
        if self.isa == "A":
            return [half for word in self.program for half in (word & 0xFFFF, word >> 16)]
        return list(self.program)

    def starts(self) -> list[int]:
        """The halfword index of each instruction."""
        halves = self.halfwords()
        if self.isa == "A":
            return list(range(0, len(halves), 2))
        result = []
        index = 0
        while index < len(halves):
            result.append(index)
            index += 2 if halves[index] >> 11 in (0x1D, 0x1E, 0x1F) else 1
        return result

    def finished(self) -> int:
        return len(self.starts()) if self.retired is None else self.retired

    def retired_before(self, half: int) -> int:
        """Instructions of a straight program ahead of the one at a halfword."""
        return sum(1 for start in self.starts() if start < half)

    def program_text(self) -> str:
        if self.isa == "A":
            return "".join(f"{word:08x}" for word in self.program)
        return "".join(f"{half:04x}" for half in self.program)


@dataclass
class Observation:
    signal: int
    code: int
    address: int
    pc: int
    exit_kind: int
    digest: int
    state: dict[str, int]
    runs: list[tuple[int, bytes]]


def initial_state(case: Case, recipes: dict[str, dict[str, int]]) -> dict[str, int]:
    state = dict(recipes[case.recipe])
    state.update(case.initial)
    return state


def request(cases: list[Case], recipes: dict[str, dict[str, int]]) -> bytes:
    body = REQUEST_MAGIC + struct.pack("<II", len(cases), 0)
    for case in cases:
        halves = case.halfwords()
        program = b"".join(struct.pack("<H", half) for half in halves)
        record = struct.pack(
            "<BBHHH", 1 if case.isa == "T" else 0, case.fill, len(program),
            len(case.data_patches), case.origin)
        record += corpus.state_wire(initial_state(case, recipes))
        record += program + bytes(-len(program) % 4)
        for offset, payload in case.data_patches:
            record += struct.pack("<II", offset, len(payload)) + payload + bytes(-len(payload) % 4)
        body += struct.pack("<I", 4 + len(record)) + record
    return body


def fill_bytes(fill: int) -> bytes:
    if fill == corpus.FILL_ZERO:
        return bytes(corpus.DATA_SIZE)
    return bytes(((1 + index) * 0x9E3779B1 & 0xFFFFFFFF) >> 24 for index in range(corpus.DATA_SIZE))


def fnv32(data: bytes) -> int:
    value = 0x811C9DC5
    for byte in data:
        value = (value ^ byte) * 0x01000193 & 0xFFFFFFFF
    return value


def initial_data_digest(case: Case) -> int:
    data = bytearray(fill_bytes(case.fill))
    for offset, payload in case.data_patches:
        data[offset:offset + len(payload)] = payload
    return fnv32(bytes(data))


def parse_response(data: bytes) -> tuple[dict[str, int], list[Observation]]:
    if data[:8] != RESPONSE_MAGIC:
        raise corpus.CorpusError("not an oracle response")
    count, hwcap, hwcap2, _ = struct.unpack_from("<4I", data, 8)
    position = 24
    observations = []
    for _ in range(count):
        size, signal, code, address, pc, kind, digest, runs = struct.unpack_from("<8I", data, position)
        state = corpus.state_from_wire(data, position + 32)
        state["apsr"] &= corpus.APSR_MASK
        cursor = position + 32 + corpus.STATE_SIZE
        changes = []
        for _ in range(runs):
            offset, length = struct.unpack_from("<II", data, cursor)
            changes.append((offset, data[cursor + 8:cursor + 8 + length]))
            cursor += 8 + ((length + 3) & ~3)
        if cursor != position + size:
            raise corpus.CorpusError("oracle response record size")
        observations.append(Observation(signal, code, address, pc, kind, digest, state, changes))
        position += size
    if position != len(data):
        raise corpus.CorpusError("trailing bytes in the oracle response")
    return {"hwcap": hwcap, "hwcap2": hwcap2}, observations
