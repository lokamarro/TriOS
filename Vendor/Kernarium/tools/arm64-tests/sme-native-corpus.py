#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Author the native SME cases of the A64 instruction corpus.

The producer that exported the corpus had no SME model, so the SME cases come
from this script and from the hardware. It derives every case from the
generated SVE/SME encoding table
(darwin/cpu/arm64/detail/sme_encodings.hpp), writes them as a request for
tools/arm64-tests/sme_native_oracle.cpp, which runs each one on an SME host,
and turns the oracle's response into arm64-sme-native records appended to a
corpus dump after the absent-feature step:

    sme-native-corpus.py --request sme.request
    sme_native_oracle sme.request sme.response          (on the SME host)
    sme-native-corpus.py absent.jsonl native.jsonl --response sme.response

The cases cover, for each record:

- control: SMSTART and SMSTOP in each form from each PSTATE.SM and PSTATE.ZA
  state, MSR SVCR, MRS SVCR, and the TPIDR2_EL0 accesses;
- classify: one witness word per table row in each mode it needs and in each
  mode that lacks one of them (SM=0, ZA=0), the rows outside the profile in
  both modes, and the evidence-binding witness of each SVE or SME entry
  without a row of its own: an alias, which its base instruction's row
  decodes, or a form the architecture executes only in Debug state (MOVT
  between ZT0 and a general register), which is UNDEFINED in every mode;
- fpsimd: the witness of every FP&SIMD class in streaming mode, where
  AdvSIMD traps and the scalar FP forms execute, except the classes of
  inventory entries the virtual CPU omits;
- operands: each operand field of each profile row at the lowest and highest
  register its alignment allows, with the other operands distinct; every
  value of each short immediate or size field and both extremes of a wider
  one; every class of bitmask immediate that DecodeBitMasks reserves, for
  the logical-immediate rows; all-true, all-false, and partial predicates;
  general-register sources at both extremes; and FPCR's flush,
  default-NaN, and rounding controls for floating-point rows;
- za: ZA slice-index registers at zero and at all ones, which wraps;
- memory: each profile memory form aligned and misaligned, crossing a 16 KiB
  boundary (a guest page boundary too) with both pages mapped, with only the
  first mapped and its elements active on the first page, with either page
  unmapped, inactive over an unmapped page, and stores to a read-only page;
- fpsr: programs that start with FPSR's IXC, IOC, or QC set, in streaming
  mode throughout (instructions that raise flags, raise none, read or write
  FPSR, take a fault or trap, or leave PSTATE.SM as it is) and entering or
  leaving it, where the change of PSTATE.SM sets FPSR to 0x0800009F.

Every expectation is observed at the oracle host's streaming vector length,
64 bytes, and a record's svl says so: the runner runs these cases only where
the virtual CPU's SVL is the record's. The SME host has no FEAT_SVE, as the
virtual CPU has none, so a case outside streaming mode whose word only an SVE
profile would execute is UNDEFINED there on both. Each case starts with X18
zero, since
Darwin clears X18 whenever a thread returns to user mode, and every address
a case forms has a zero top byte, since the host ignores it and the guest's
default state does not.

The SM = 0 vector-length family (CNT[BHWD], RDVL, ADDVL, ADDPL and the
scalar INC and DEC) is the one exception to the host's results: the SME host
traps those words outside streaming mode, as the architecture says, and the
virtual CPU deliberately executes them there for a vector length of 16 bytes
(darwin/cpu/arm64/detail/sm0_vector_length.hpp). The request keeps their
cases so that the oracle still observes the host, and the findings still
check them against the table, but no record holds them:
sm0-vl-corpus.py appends the project's results as arm64-sm0-vl records.

The response must come from an SME host with FEAT_SME2. A case whose
legality differs from the table's mode requirements, or an FP&SIMD class
whose legality in streaming mode differs from the table's AdvSIMD flag, is a
finding about the table: the script reports each one and writes nothing.
The table also decides that an operand case's word is UNDEFINED in every
mode when no row decodes it or its row's bitmask immediate is one that
DecodeBitMasks reserves. Such a case is an invalid encoding, and so is a case
whose row no mode of any virtual-CPU configuration executes and a
Debug-state form; a case that is UNDEFINED only for its mode or a missing
feature is not, including a row that streaming mode executes and
PSTATE.SM = 0 does not. invalid_encoding.py decides each case's bit from the table,
and the case's own expectation stands where it does not.

Oracle request (little-endian):

    "UMLSMEQ\\0" u32 version=1, u32 svl, u32 count, count * case
    case   = u32 words, u32 entry, words * u32, state, u8 regions, region*
    state  = u32 X mask (bit i: Xi, bit 31: SP, each set one follows as u64),
             u32 nzcv, u32 fpcr, u32 fpsr, u8 svcr, u64 tpidr2,
             u32 V mask, 16 bytes per set V, [sme]   (sme when svl is not 0)
    sme    = u32 Z mask, svl bytes per Z; u16 P mask, svl/8 bytes per P;
             svl/8-byte ZA row mask, svl bytes per row; u8 ZT0 flag, 64 bytes
    region = u64 address, u32 size, u8 flags (1 readable, 2 writable,
             0x80 explicit), then size bytes if explicit, else u32 seed

A request state is encoded against the corpus base state (the pack's base
fills) and must have X18 zero. A region without explicit bytes holds byte
fill_hash(seed * 0x01000193 + offset) at each offset. Its SVCR selects the
live register files as in the pack.

Oracle response:

    "UMLSMER\\0" u32 version=2, u32 host svl, u32 host features, u32 count,
    count * (u32 event, u32 pc offset, u64 fault address, u32 ESR, state,
             per request region: u32 runs, runs * (u32 offset, u32 length,
             bytes))

A response state is encoded against its case's initial state, and holds the
parts its own SVCR makes live. Events are numbered as in the pack, with 8
for a run the oracle could not attribute. Host features are FEAT_SME,
FEAT_SME2, FEAT_SME_F64F64, FEAT_SME_I16I64, FEAT_SME_FA64, and FEAT_SME2p1
in bits 0-5.

A response's FPSR is what the case's instructions left. XNU sets FPSR to
0x0800009F on every kernel entry in streaming mode, interrupts included, so
the oracle sets and reads FPSR in user mode with no trap in between (its
header describes how). Version 1 responses carried that reset value for
every case that ended in streaming mode, so this script reads none.

A regeneration of the earlier steps that leaves this script's request
unchanged needs no SME host: --carry PACK copies the native records of the
current pack.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import dataclasses
import functools
import hashlib
import importlib.util
import json
import pathlib
import struct
import sys
import zlib
from dataclasses import dataclass, field

HERE = pathlib.Path(__file__).resolve().parent


def _load(name: str, path: pathlib.Path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


corpus = _load("build_unit_corpus", HERE / "build-unit-corpus.py")
sme_table = _load("sme_encoding_table", HERE / "sme_encoding_table.py")
invalid_encoding = _load("invalid_encoding", HERE / "invalid_encoding.py")

SITE = "tools/arm64-tests/sme-native-corpus.py"
PRODUCER = "arm64-sme-native"
SVL = 64
REQUEST_MAGIC = b"UMLSMEQ\0"
RESPONSE_MAGIC = b"UMLSMER\0"
REQUEST_VERSION = 1
RESPONSE_VERSION = 2
SVC = 0xD400_0001
SENTINEL = 0xD420_00E0  # BRK #7: the oracle runs each SVC as this
SM = corpus.SVCR_SM
ZA = corpus.SVCR_ZA
REGION_READABLE = 1
REGION_WRITABLE = 2
REGION_EXPLICIT = 0x80
EVENTS = {0: "system-call", 1: "memory-fault", 3: "illegal-instruction",
          4: "breakpoint", 5: "floating-point-trap", 8: "fatal"}
HOST_FEATURES = ("FEAT_SME", "FEAT_SME2", "FEAT_SME_F64F64",
                 "FEAT_SME_I16I64", "FEAT_SME_FA64", "FEAT_SME2p1")
# Data regions sit above every mapping XNU places itself, and below 2^48.
DATA = 0x100_0000_0000
PAGE = 0x4000
WIDE = 0x10000
MIDDLE = DATA + WIDE // 2
BOUNDARY = DATA + PAGE
# Corpus region flags: readable, writable, and guest writable; readable.
READ_WRITE = 0x13
READ_ONLY = 0x01
# DN, FZ, round towards zero, FZ16.
FLUSH_FPCR = 0x03C8_0000
GROUPS = ("control", "classify", "fpsimd", "operands", "za", "memory",
          "fpsr")


def fill_hash(index: int) -> int:
    return ((index * 0x9E3779B1) & 0xFFFFFFFF) >> 24


@functools.cache
def region_fill(seed: int, size: int) -> bytes:
    start = (seed * 0x01000193) & 0xFFFFFFFF
    return bytes(fill_hash((start + offset) & 0xFFFFFFFF)
                 for offset in range(size))


# --------------------------------------------------------------------------
# Wire format


@dataclass
class WireState:
    """A sparse state: the entries a case sets over its reference."""
    x: dict[int, int] = field(default_factory=dict)
    nzcv: int = 0
    fpcr: int = 0
    fpsr: int = 0
    svcr: int = 0
    tpidr2: int = 0
    v: dict[int, bytes] = field(default_factory=dict)
    z: dict[int, bytes] = field(default_factory=dict)
    p: dict[int, bytes] = field(default_factory=dict)
    za: dict[int, bytes] = field(default_factory=dict)
    zt0: bytes | None = None


@dataclass(frozen=True)
class WireRegion:
    address: int
    size: int
    flags: int
    seed: int = 0
    data: bytes | None = None

    def initial(self) -> bytes:
        return self.data if self.data is not None else \
            region_fill(self.seed, self.size)


@dataclass
class WireCase:
    words: tuple[int, ...]
    entry: int
    state: WireState
    regions: tuple[WireRegion, ...] = ()


@dataclass
class WireResult:
    event: str
    pc_offset: int
    fault_address: int
    esr: int
    state: WireState
    runs: tuple[tuple[tuple[int, bytes], ...], ...]


def _put_state(out: bytearray, state: WireState, svl: int) -> None:
    out += struct.pack("<I", sum(1 << i for i in state.x))
    for i in sorted(state.x):
        out += struct.pack("<Q", state.x[i])
    out += struct.pack("<IIIBQ", state.nzcv, state.fpcr, state.fpsr,
                       state.svcr, state.tpidr2)
    out += struct.pack("<I", sum(1 << i for i in state.v))
    for i in sorted(state.v):
        out += state.v[i]
    if not svl:
        return
    out += struct.pack("<I", sum(1 << i for i in state.z))
    for i in sorted(state.z):
        out += state.z[i]
    out += struct.pack("<H", sum(1 << i for i in state.p))
    for i in sorted(state.p):
        out += state.p[i]
    out += sum(1 << r for r in state.za).to_bytes(svl // 8, "little")
    for r in sorted(state.za):
        out += state.za[r]
    out += b"\0" if state.zt0 is None else b"\1" + state.zt0


def write_request(cases: list[WireCase], svl: int) -> bytes:
    out = bytearray(REQUEST_MAGIC)
    out += struct.pack("<III", REQUEST_VERSION, svl, len(cases))
    for case in cases:
        out += struct.pack("<II", len(case.words), case.entry)
        out += struct.pack(f"<{len(case.words)}I", *case.words)
        _put_state(out, case.state, svl)
        out += struct.pack("<B", len(case.regions))
        for region in case.regions:
            explicit = region.data is not None
            out += struct.pack("<QIB", region.address, region.size,
                               region.flags | (REGION_EXPLICIT if explicit
                                               else 0))
            out += region.data if explicit else struct.pack("<I", region.seed)
    return bytes(out)


class _Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def take(self, size: int) -> bytes:
        if self.offset + size > len(self.data):
            raise ValueError("truncated oracle response")
        chunk = self.data[self.offset:self.offset + size]
        self.offset += size
        return chunk

    def unpack(self, form: str):
        values = struct.unpack("<" + form,
                               self.take(struct.calcsize("<" + form)))
        return values if len(values) > 1 else values[0]


def _get_state(reader: _Reader, svl: int) -> WireState:
    state = WireState()
    mask = reader.unpack("I")
    for i in range(32):
        if mask >> i & 1:
            state.x[i] = reader.unpack("Q")
    (state.nzcv, state.fpcr, state.fpsr, state.svcr,
     state.tpidr2) = reader.unpack("IIIBQ")
    mask = reader.unpack("I")
    state.v = {i: reader.take(16) for i in range(32) if mask >> i & 1}
    if not svl:
        return state
    mask = reader.unpack("I")
    state.z = {i: reader.take(svl) for i in range(32) if mask >> i & 1}
    mask = reader.unpack("H")
    state.p = {i: reader.take(svl // 8) for i in range(16) if mask >> i & 1}
    rows = int.from_bytes(reader.take(svl // 8), "little")
    state.za = {r: reader.take(svl) for r in range(svl) if rows >> r & 1}
    if reader.unpack("B"):
        state.zt0 = reader.take(64)
    return state


def read_response(data: bytes, cases: list[WireCase], svl: int
                  ) -> tuple[dict, list[WireResult]]:
    reader = _Reader(data)
    if reader.take(8) != RESPONSE_MAGIC:
        raise ValueError("not an oracle response")
    version, host_svl, features, count = reader.unpack("IIII")
    if version != RESPONSE_VERSION or count != len(cases):
        raise ValueError("the oracle response does not answer this request")
    host = {"svl": host_svl,
            "features": [name for bit, name in enumerate(HOST_FEATURES)
                         if features >> bit & 1]}
    results = []
    for case in cases:
        event, pc_offset, fault_address, esr = reader.unpack("IIQI")
        state = _get_state(reader, svl)
        runs = []
        for _region in case.regions:
            region_runs = []
            for _ in range(reader.unpack("I")):
                offset, length = reader.unpack("II")
                region_runs.append((offset, reader.take(length)))
            runs.append(tuple(region_runs))
        results.append(WireResult(EVENTS.get(event, f"event-{event}"),
                                  pc_offset, fault_address, esr, state,
                                  tuple(runs)))
    if reader.offset != len(data):
        raise ValueError("trailing bytes in the oracle response")
    return host, results


# --------------------------------------------------------------------------
# Dense states


@dataclass
class Dense:
    x: list[int]
    nzcv: int
    fpcr: int
    fpsr: int
    svcr: int
    tpidr2: int
    v: list[bytes]
    z: list[bytes]
    p: list[bytes]
    za: list[bytes]
    zt0: bytes

    @classmethod
    def base(cls, svl: int) -> Dense:
        return _base_state(svl)

    def apply(self, state: WireState) -> Dense:
        x = list(self.x)
        for i, value in state.x.items():
            x[i] = value
        v, z, p, za = list(self.v), list(self.z), list(self.p), list(self.za)
        for target, items in ((v, state.v), (z, state.z), (p, state.p),
                              (za, state.za)):
            for i, value in items.items():
                target[i] = value
        return Dense(x=x, nzcv=state.nzcv, fpcr=state.fpcr, fpsr=state.fpsr,
                     svcr=state.svcr, tpidr2=state.tpidr2, v=v, z=z, p=p,
                     za=za, zt0=state.zt0 if state.zt0 is not None
                     else self.zt0)


@functools.cache
def _base_state(svl: int) -> Dense:
    return Dense(x=list(corpus.FILL_REGISTERS), nzcv=0, fpcr=0, fpsr=0,
                 svcr=0, tpidr2=0, v=list(corpus.FILL_VECTORS),
                 z=[corpus.fill_z(i, svl) for i in range(32)],
                 p=[corpus.fill_p(i, svl) for i in range(16)],
                 za=[corpus.fill_za(r, svl) for r in range(svl)],
                 zt0=corpus.fill_zt0())


def hex64(value: int) -> str:
    return corpus.hex64(value)


def expected_fields(initial: Dense, final: Dense, pc: int,
                    entry_pc: int) -> dict:
    """The expected state against the initial one, as the pack encodes it:
    a part the initial state does not hold is against the base fill, and
    the V registers of a state in streaming mode are its Z registers' low
    bytes."""
    out: dict[str, str] = {}
    if pc != entry_pc:
        out["pc"] = hex64(pc)
    for i in range(32):
        if final.x[i] != initial.x[i]:
            out["sp" if i == 31 else f"x{i}"] = hex64(final.x[i])
    for name in ("nzcv", "fpcr", "fpsr", "svcr", "tpidr2"):
        if getattr(final, name) != getattr(initial, name):
            out[name] = hex64(getattr(final, name))
    if final.svcr & SM:
        for i in range(32):
            if final.z[i] != initial.z[i]:
                out[f"z{i}"] = final.z[i].hex()
        for i in range(16):
            if final.p[i] != initial.p[i]:
                out[f"p{i}"] = final.p[i].hex()
    else:
        view = [initial.z[i][:16] for i in range(32)] \
            if initial.svcr & SM else initial.v
        for i in range(32):
            if final.v[i] != view[i]:
                out[f"v{i}"] = final.v[i].hex()
    if final.svcr & ZA:
        for r in range(len(final.za)):
            if final.za[r] != initial.za[r]:
                out[f"za{r}"] = final.za[r].hex()
        if final.zt0 != initial.zt0:
            out["zt0"] = final.zt0.hex()
    return out


# --------------------------------------------------------------------------
# Drafts


@dataclass
class Draft:
    group: str
    name: str
    words: tuple[int, ...]
    svcr: int = 0
    x: dict[int, int] = field(default_factory=dict)
    nzcv: int = 0
    fpcr: int = 0
    fpsr: int = 0
    tpidr2: int = 0
    v: dict[int, bytes] = field(default_factory=dict)
    z: dict[int, bytes] = field(default_factory=dict)
    p: dict[int, bytes] = field(default_factory=dict)
    za: dict[int, bytes] = field(default_factory=dict)
    zt0: bytes | None = None
    regions: tuple[WireRegion, ...] = ()
    native: tuple[str, ...] = ("sme",)
    store: bool = False
    # "legal" or "illegal" where the table decides; "" where only the
    # hardware does.
    expect: str = ""
    invalid: bool = False

    def wire(self) -> WireCase:
        x = dict(self.x)
        x[18] = 0
        if (not self.svcr & SM and (self.z or self.p)) or \
                (self.svcr & SM and self.v) or \
                (not self.svcr & ZA and (self.za or self.zt0 is not None)):
            raise SystemExit(f"{self.name}: state outside its SVCR")
        state = WireState(x=x, nzcv=self.nzcv, fpcr=self.fpcr,
                          fpsr=self.fpsr, svcr=self.svcr,
                          tpidr2=self.tpidr2, v=self.v,
                          z=self.z, p=self.p, za=self.za, zt0=self.zt0)
        return WireCase(words=self.words + (SVC,), entry=0, state=state,
                        regions=self.regions)


def svcr_name(svcr: int) -> str:
    return f"SM={svcr & 1} ZA={svcr >> 1 & 1}"


# --------------------------------------------------------------------------
# Operands


def encode(word: int, operand, number: int) -> int | None:
    """The word with the operand naming register (or tile) number, or None
    where a constant segment forbids it."""
    total = sum(segment.width for segment in operand.segments)
    if number >> total:
        return None
    shift = total
    for segment in operand.segments:
        shift -= segment.width
        part = number >> shift & ((1 << segment.width) - 1)
        if segment.field:
            mask = ((1 << segment.width) - 1) << segment.low
            word = (word & ~mask) | part << segment.low
        elif part != segment.constant:
            return None
    return word


@functools.cache
def numbers(operand) -> tuple[int, ...]:
    total = sum(segment.width for segment in operand.segments)
    return tuple(number for number in range(1 << total)
                 if encode(0, operand, number) is not None)


def field_bits(operand) -> int:
    bits = 0
    for segment in operand.fields():
        bits |= ((1 << segment.width) - 1) << segment.low
    return bits


def occupied(operand, number: int) -> set[tuple[str, int]]:
    kind = operand.kind
    if kind in ("z", "v"):
        return {("z", (number + k * operand.stride) % 32)
                for k in range(operand.count)}
    if kind == "p":
        return {("p", (number + k * operand.stride) % 16)
                for k in range(operand.count)}
    if kind in ("gpr", "gpr_or_sp"):
        return {("x", number)}
    return {(kind, number)}


def is_counter(operand) -> bool:
    """A predicate-as-counter operand (PN8-PN15)."""
    segments = operand.segments
    return operand.kind == "p" and len(segments) == 2 and \
        not segments[0].field and segments[0].width == 1 and \
        segments[0].constant == 1


def is_slice_index(row, index: int) -> bool:
    """A ZA slice-index register: W8-W11 or W12-W15."""
    operand = row.operands[index]
    if operand.kind != "gpr" or not row.za:
        return False
    choices = numbers(operand)
    return len(choices) == 4 and choices[0] in (8, 12) and \
        choices[-1] == choices[0] + 3


def memory_registers(row) -> set[int]:
    if row.memory is None:
        return set()
    indices = {row.memory.base_operand}
    if row.memory.index_operand is not None:
        indices.add(row.memory.index_operand)
    return indices


def allocate(row, word: int, order: list[int], high: bool) -> int:
    """Gives the operands, in order, the lowest (or highest) register each
    allows that no earlier operand of the same register file uses."""
    used: set[tuple[str, int]] = set()
    fixed = 0
    for index in order:
        operand = row.operands[index]
        bits = field_bits(operand)
        if bits & fixed:
            continue
        candidates = [number for number in numbers(operand)
                      if not (operand.kind in ("gpr", "gpr_or_sp") and
                              number == 18)]
        if operand.kind in ("gpr", "gpr_or_sp") and not high:
            candidates = [number for number in candidates if number != 31]
        if high:
            candidates.reverse()
        choice = next((number for number in candidates
                       if not occupied(operand, number) & used),
                      candidates[0] if candidates else None)
        if choice is None:
            continue
        word = encode(word, operand, choice)
        used |= occupied(operand, choice)
        fixed |= bits
    return word


def operand_value(row, word: int, index: int) -> int:
    return row.operands[index].number(word)


def free_runs(row) -> list[tuple[int, int]]:
    """The maximal runs of bits the row leaves free that no register operand
    names: immediates, element sizes, and other controls."""
    free = ~row.mask & 0xFFFF_FFFF
    for operand in row.operands:
        free &= ~field_bits(operand)
    runs = []
    bit = 0
    while bit < 32:
        if free >> bit & 1:
            start = bit
            while bit < 32 and free >> bit & 1:
                bit += 1
            runs.append((start, bit - start))
        else:
            bit += 1
    return runs


def with_run(word: int, run: tuple[int, int], value: int) -> int:
    low, width = run
    mask = ((1 << width) - 1) << low
    return (word & ~mask) | (value << low & mask)


# --------------------------------------------------------------------------
# Memory


def element_bytes(row, word: int) -> tuple[int, int]:
    """(bytes per element in memory, elements per vector)."""
    memory = row.memory
    esize = memory.element_bits(word)
    return memory.msize // 8, SVL * 8 // esize


def footprint(row, word: int) -> int:
    memory = row.memory
    shape = memory.shape
    if shape in ("fill_z", "za_vector"):
        return SVL
    if shape == "fill_p":
        return SVL // 8
    if shape == "zt0":
        return 64
    if shape == "replicate":
        return memory.msize // 8
    if shape == "replicate_quad":
        return 16
    if shape == "prefetch":
        return 0
    mbytes, elements = element_bytes(row, word)
    return mbytes * elements * memory.registers


def predicated(row) -> bool:
    return any(operand.kind == "p" for operand in row.operands)


def first_half_predicate(row, word: int) -> bytes:
    """Elements below half the vector active, for the element size."""
    mbytes, elements = element_bytes(row, word)
    esize = SVL // elements
    bits = 0
    for element in range(elements // 2):
        bits |= 1 << (element * esize)
    return bits.to_bytes(SVL // 8, "little")


def set_predicates(row, word: int, pattern: bytes,
                   counter: bytes | None = None) -> dict[int, bytes]:
    result = {}
    for operand in row.operands:
        if operand.kind != "p":
            continue
        value = counter if is_counter(operand) and counter is not None \
            else pattern
        for _kind, number in occupied(operand, operand.number(word)):
            result[number] = value
    return result


def address_registers(row, word: int, base: int, index: int = 2
                      ) -> dict[int, int]:
    """X (or SP, as 31) values that make the row's base register the
    address and its index register the index."""
    memory = row.memory
    values = {}
    number = operand_value(row, word, memory.base_operand)
    values[number] = base
    if memory.index_operand is not None:
        index_number = operand_value(row, word, memory.index_operand)
        if index_number != 31 and index_number != number:
            values[index_number] = index
    return values


def zero_offset(row, word: int) -> int:
    """The word with its memory immediate zero."""
    for segment in row.memory.immediate:
        if segment.field:
            word = with_run(word, (segment.low, segment.width), 0)
    return word


# --------------------------------------------------------------------------
# Case generation


def mode_svcr(row) -> int:
    return (SM if row.streaming else 0) | (ZA if row.za or row.zt0 else 0)


def row_name(row, word: int) -> str:
    return f"{row.stable_id} {row.encoding} {word:08x}"


def native_of(row) -> tuple[str, ...]:
    return tuple(row.requirement) or ("sme",)


def base_draft(group: str, row, word: int, label: str, svcr: int,
               expect: str = "") -> Draft:
    draft = Draft(group=group, name=f"{row_name(row, word)} {label}, "
                  f"{svcr_name(svcr)}", words=(word,), svcr=svcr,
                  native=native_of(row), expect=expect,
                  store=row.memory is not None and
                  row.memory.direction == "store")
    if row.memory is not None and row.memory.shape != "prefetch":
        draft.regions = (WireRegion(DATA, WIDE, REGION_READABLE |
                                    REGION_WRITABLE, seed=1),)
        draft.x.update(address_registers(row, word, MIDDLE))
    elif row.memory is not None:
        draft.x.update(address_registers(row, word, MIDDLE))
    return draft


def control_drafts() -> list[Draft]:
    """SMSTART, SMSTOP, the SVCR accesses, and TPIDR2_EL0."""
    msr = {"SMSTART": 0xD503477F, "SMSTART SM": 0xD503437F,
           "SMSTART ZA": 0xD503457F, "SMSTOP": 0xD503467F,
           "SMSTOP SM": 0xD503427F, "SMSTOP ZA": 0xD503447F}
    drafts = []
    for svcr in range(4):
        for name, word in msr.items():
            drafts.append(Draft(group="control",
                                name=f"{name} from {svcr_name(svcr)}",
                                words=(word,), svcr=svcr, expect="legal"))
        drafts.append(Draft(group="control",
                            name=f"MRS X0, SVCR in {svcr_name(svcr)}",
                            words=(0xD53B4240,), svcr=svcr, expect="legal"))
        for value in (0, 1, 2, 3, 0xFFFF_FFFF_FFFF_FFFC,
                      0xFFFF_FFFF_FFFF_FFFF):
            drafts.append(Draft(
                group="control",
                name=f"MSR SVCR, X1 = {value:#x} in {svcr_name(svcr)}",
                words=(0xD51B4241,), svcr=svcr, x={1: value},
                expect="legal"))
        drafts.append(Draft(group="control",
                            name=f"MRS X2, TPIDR2_EL0 in {svcr_name(svcr)}",
                            words=(0xD53BD0A2,), svcr=svcr,
                            tpidr2=0x0123_4567_89AB_CDEF, expect="legal"))
        drafts.append(Draft(group="control",
                            name=f"MSR TPIDR2_EL0, X3 in {svcr_name(svcr)}",
                            words=(0xD51BD0A3,), svcr=svcr,
                            x={3: 0xFEDC_BA98_7654_3210}, expect="legal"))
    return drafts


def alias_witnesses(table) -> list[tuple[str, str, int]]:
    """The evidence-binding witnesses of the SVE and SME inventory entries
    without a table row of their own: aliases, whose words their base
    instruction's rows decode, and Debug-state-only forms, whose words no
    row decodes."""
    rows = {row.stable_id for row in table.rows}
    with corpus.BINDINGS.open(encoding="utf-8", newline="") as source:
        return [(row["stable_id"], row["architectural_form"],
                 int(row["encoding"], 16))
                for row in csv.DictReader(source, delimiter="\t")
                if row["section"] in ("SVE", "SME") and
                row["stable_id"] not in rows]


def classify_drafts(table) -> list[Draft]:
    drafts = []
    index = RowIndex(table)
    for stable_id, form, word in alias_witnesses(table):
        row = index.row_for(word)
        if row is None:
            for svcr in (0, SM | ZA):
                drafts.append(Draft(
                    group="classify",
                    name=f"{stable_id} {form} {word:08x} outside Debug "
                         f"state, {svcr_name(svcr)}",
                    words=(word,), svcr=svcr, expect="illegal",
                    invalid=True))
            continue
        if not row.in_profile:
            raise SystemExit(f"{stable_id} {word:08x}: an alias witness "
                             "outside the profile's rows")
        need = mode_svcr(row)
        for label, svcr, expect in (("witness", need | SM, "legal"),
                                    ("needs SM", need & ~SM, "illegal")):
            if label == "needs SM" and not need & SM:
                continue
            draft = base_draft("classify", row, word, label, svcr, expect)
            draft.name = f"{stable_id} {form} {word:08x} {label}, " \
                f"{svcr_name(svcr)}"
            drafts.append(draft)
    for row in table.rows:
        word = row.witness
        if not row.in_profile:
            for svcr in (0, SM | ZA):
                drafts.append(Draft(
                    group="classify",
                    name=f"{row_name(row, word)} outside the profile, "
                         f"{svcr_name(svcr)}",
                    words=(word,), svcr=svcr, expect="illegal"))
            continue
        need = mode_svcr(row)
        modes = [need] if need & SM else [need, need | SM]
        for svcr in modes:
            drafts.append(base_draft("classify", row, word, "witness", svcr,
                                     "legal"))
        if need & SM:
            drafts.append(base_draft("classify", row, word, "needs SM",
                                     need & ~SM, "illegal"))
        if need & ZA:
            drafts.append(base_draft("classify", row, word, "needs ZA",
                                     need & ~ZA, "illegal"))
    return drafts


def literal_load(word: int) -> bool:
    return (word & 0x3F00_0000) == 0x1C00_0000


def load_store(word: int) -> bool:
    return (word >> 25 & 5) == 4


def absent_entries() -> set[str]:
    """The inventory entries whose features the virtual CPU omits; the host
    may implement them, but the guest's result is UNDEFINED, which the
    absent-feature records already expect."""
    with corpus.INVENTORY.open(encoding="utf-8", newline="") as source:
        return {row["stable_id"]
                for row in csv.DictReader(source, delimiter="\t")
                if row["engine_status"] == "absent"}


def fpsimd_drafts(table) -> list[Draft]:
    drafts = []
    absent = absent_entries()
    for row in table.fp_simd:
        if row.stable_id in absent:
            continue
        word = row.witness
        if load_store(word) and word & 0x3C40_0000 == 0x2C40_0000 and \
                word >> 10 & 31 == word & 31:
            # A SIMD&FP load pair with Rt == Rt2 is CONSTRAINED
            # UNPREDICTABLE, and the host makes it UNDEFINED.
            word = word & ~(31 << 10) | ((word & 31) + 1) % 32 << 10
        words: tuple[int, ...] = (word,)
        draft = Draft(group="fpsimd",
                      name=f"{row.stable_id} {row.encoding} {word:08x} "
                           f"{'AdvSIMD' if row.advsimd else 'FP'}, "
                           f"{svcr_name(SM)}",
                      words=words, svcr=SM,
                      expect="illegal" if row.advsimd else "legal")
        if literal_load(word):
            # PC-relative: load the program's own words.
            draft.words = (word & ~(0x7FFFF << 5), SVC, SVC)
        elif load_store(word):
            base = word >> 5 & 31
            draft.x[base] = MIDDLE
            index = word >> 16 & 31
            if index not in (base, 31, 18):
                draft.x[index] = 2
            draft.regions = (WireRegion(DATA, WIDE, REGION_READABLE |
                                        REGION_WRITABLE, seed=1),)
            draft.store = not word >> 22 & 1
        drafts.append(draft)
    return drafts


class RowIndex:
    """Table.row_for(), with the rows indexed by the top encoding byte."""

    def __init__(self, table) -> None:
        self.by_top = [[row for row in table.rows
                        if (top << 24 ^ row.value) & row.mask >> 24 << 24
                        == 0] for top in range(256)]

    def row_for(self, word: int):
        return next((row for row in self.by_top[word >> 24]
                     if row.matches(word)), None)


BITMASK_ROWS = ("and_z_zi_", "eor_z_zi_", "orr_z_zi_", "dupm_z_i_")


def reserved_bitmask(word: int) -> bool:
    """Whether DecodeBitMasks reserves a logical-immediate word's imm13 (N
    at bit 17, imms at bits 10:5): N:NOT(imms) selects no element size, or
    imms sets every bit of its element size."""
    n, imms = word >> 17 & 1, word >> 5 & 0x3F
    size = (n << 6 | ~imms & 0x3F).bit_length() - 1
    if size < 1:
        return True
    ones = (1 << size) - 1
    return imms & ones == ones


def undefined_everywhere(index: RowIndex, word: int) -> bool:
    """Whether the table makes a word UNDEFINED in every mode: no row
    decodes it, or its row's bitmask immediate is reserved."""
    row = index.row_for(word)
    return row is None or \
        row.encoding in BITMASK_ROWS and reserved_bitmask(word)


def operand_drafts(table) -> list[Draft]:
    drafts = []
    index = RowIndex(table)
    for row in table.rows:
        if not row.in_profile:
            continue
        svcr = mode_svcr(row)
        count = len(row.operands)
        seen = {row.witness}
        low_word = allocate(row, row.witness, list(range(count)), False)

        def add(word: int, label: str, **changes) -> Draft | None:
            if word in seen and not changes:
                return None
            seen.add(word)
            draft = base_draft("operands", row, word, label, svcr)
            if undefined_everywhere(index, word):
                draft.expect, draft.invalid = "illegal", True
            for key, value in changes.items():
                if key == "x":
                    draft.x.update(value)
                else:
                    setattr(draft, key, value)
            drafts.append(draft)
            return draft

        for rotation in range(max(count, 1)):
            order = list(range(rotation, count)) + list(range(rotation))
            for high in (False, True):
                word = allocate(row, row.witness, order, high)
                add(word, f"registers {'high' if high else 'low'} from "
                          f"operand {rotation}")
        for run in free_runs(row):
            low, width = run
            values = range(1 << width) if width <= 2 else \
                (0, (1 << width) - 1)
            for value in values:
                word = with_run(low_word, run, value)
                found = index.row_for(word)
                if found is not None and found is not row:
                    continue
                add(word, f"bits {low + width - 1}:{low} = {value:#x}")
        if row.encoding in BITMASK_ROWS:
            # Every class of bitmask immediate that DecodeBitMasks
            # reserves: N = 1 with imms all ones, N = 0 with imms 11111x,
            # and each smaller element size with every element bit set.
            for value in (0x103F, 0x003E, 0x003F, 0x003D, 0x003B, 0x0037,
                          0x002F, 0x001F):
                add(with_run(low_word, (5, 13), value),
                    f"reserved bitmask immediate {value:#06x}")
        if predicated(row) and svcr & SM:
            patterns = (("all-true", b"\xff" * (SVL // 8)),
                        ("all-false", bytes(SVL // 8)),
                        ("partial", b"\xff" * 3 + bytes(SVL // 8 - 3)))
            for label, pattern in patterns:
                add(low_word, f"predicates {label}",
                    p=set_predicates(row, low_word, pattern))
        sources = [index for index, operand in enumerate(row.operands)
                   if operand.kind == "gpr" and
                   index not in memory_registers(row) and
                   not is_slice_index(row, index)]
        if sources:
            for value in (0, 0xFFFF_FFFF_FFFF_FFFF):
                registers = {operand_value(row, low_word, index): value
                             for index in sources}
                registers.pop(31, None)
                registers.pop(18, None)
                if registers:
                    add(low_word, f"general sources = {value:#x}",
                        x=registers)
        if row.encoding.startswith(("f", "bf")):
            add(low_word, "FPCR DN FZ FZ16 RZ", fpcr=FLUSH_FPCR)
    return drafts


def za_drafts(table) -> list[Draft]:
    drafts = []
    for row in table.rows:
        if not row.in_profile:
            continue
        slices = [index for index in range(len(row.operands))
                  if is_slice_index(row, index)]
        if not slices:
            continue
        word = allocate(row, row.witness, list(range(len(row.operands))),
                        False)
        for value in (0, 0xFFFF_FFFF):
            draft = base_draft("za", row, word, f"slice index {value:#x}",
                               mode_svcr(row))
            for index in slices:
                draft.x[operand_value(row, word, index)] = value
            drafts.append(draft)
    return drafts


def memory_drafts(table) -> list[Draft]:
    drafts = []
    wide = WireRegion(DATA, WIDE, REGION_READABLE | REGION_WRITABLE, seed=1)
    first = WireRegion(DATA, PAGE, REGION_READABLE | REGION_WRITABLE, seed=2)
    second = WireRegion(BOUNDARY, PAGE, REGION_READABLE | REGION_WRITABLE,
                        seed=3)
    for row in table.rows:
        if not row.in_profile or row.memory is None or \
                row.memory.shape == "prefetch":
            continue
        svcr = mode_svcr(row)
        word = zero_offset(row, allocate(row, row.witness,
                                         list(range(len(row.operands))),
                                         False))
        size = footprint(row, word)
        all_true = b"\xff" * (SVL // 8)
        counter = None

        def add(label: str, base: int, regions: tuple[WireRegion, ...],
                pattern: bytes | None = all_true) -> None:
            draft = base_draft("memory", row, word, label, svcr)
            draft.regions = regions
            draft.x.update(address_registers(row, word, base, index=0))
            if pattern is not None and predicated(row):
                draft.p = set_predicates(row, word, pattern, counter)
            drafts.append(draft)

        add("aligned", MIDDLE, (wide,))
        add("misaligned by 3", MIDDLE + 3, (wide,))
        crossing = BOUNDARY - size // 2
        add("crossing a page", crossing, (first, second))
        add("crossing a page misaligned by 1", crossing + 1, (first, second))
        add("crossing into an unmapped page", crossing, (first,))
        add("crossing from an unmapped page", crossing, (second,))
        shape = row.memory.shape
        if predicated(row) and not any(map(is_counter, row.operands)) and \
                (shape in ("structure", "za_slice") or
                 shape == "contiguous" and row.memory.registers == 1):
            add("active only below an unmapped page", crossing, (first,),
                first_half_predicate(row, word))
        if predicated(row):
            add("inactive over unmapped memory", crossing, (),
                bytes(SVL // 8))
        if row.memory.direction == "store":
            add("into a read-only page", MIDDLE,
                (WireRegion(DATA, WIDE, REGION_READABLE, seed=1),))
    return drafts


def fpsr_drafts() -> list[Draft]:
    """Programs that start with cumulative FPSR flags set: in streaming
    mode throughout, where the flags must reach every instruction and
    survive the ones that raise none, and across SMSTART and SMSTOP, whose
    change of PSTATE.SM replaces them with 0x0800009F."""
    lanes = SVL // 4

    def words32(value: int) -> bytes:
        return value.to_bytes(4, "little") * lanes

    one, tiny, snan = words32(0x3F80_0000), words32(0x3080_0000), \
        words32(0x7FA0_0000)
    fadd = 0x1E21_2802                  # FADD S2, S0, S1
    inexact = {0: one, 1: tiny}         # 1 + 2^-30 is inexact
    streaming = (
        ("ADD X0, X1, X2", SM, (0x8B02_0020,), {}, {}),
        ("MRS X3, FPSR", SM, (0xD53B_4423,), {}, {}),
        ("MRS X3, FPSR", SM | ZA, (0xD53B_4423,), {}, {}),
        ("FADD S2, S0, S1", SM, (fadd,), inexact, {}),
        ("FADD Z2.S, Z0.S, Z1.S", SM | ZA, (0x6581_0002,), inexact, {}),
        ("FCMP S0, S1 with a signalling NaN", SM, (0x1E21_2000,),
         {0: one, 1: snan}, {}),
        ("SQADD Z2.S, Z0.S, Z1.S saturating", SM, (0x04A1_1002,),
         {0: words32(0x7FFF_FFFF), 1: words32(1)}, {}),
        ("FMOPA ZA0.S, P0/M, P1/M, Z0.S, Z1.S", SM | ZA, (0x8081_2000,),
         {0: one, 1: snan}, {}),
        ("MSR FPSR, X1 (0); FADD S2, S0, S1", SM,
         (0xD51B_4421, fadd), inexact, {1: 0}),
        ("FADD S2, S0, S1; ADD V0.4S, V1.4S, V2.4S", SM,
         (fadd, 0x4EA2_8420), inexact, {}),
        ("FADD S2, S0, S1; PTRUE P0.S; LD1W {Z0.S}, P0/Z, [X0] unmapped",
         SM, (fadd, 0x2598_E3E0, 0xA540_A000), inexact, {0: DATA}),
        ("SMSTART ZA keeps PSTATE.SM", SM, (0xD503_457F,), {}, {}),
        ("SMSTART SM keeps PSTATE.SM", SM, (0xD503_437F,), {}, {}),
        ("SMSTOP ZA keeps PSTATE.SM", SM | ZA, (0xD503_447F,), {}, {}),
    )
    # SMSTART SM resets FPSR and zeroes Z, so FDIV 0/0 raises IOC.
    entering = (
        ("SMSTART SM (resets FPSR); MRS X3, FPSR", 0,
         (0xD503_437F, 0xD53B_4423), {}),
        ("SMSTART (resets FPSR); ADD X0, X1, X2", 0,
         (0xD503_477F, 0x8B02_0020), {}),
        ("SMSTART SM (resets FPSR); MSR FPSR, X1 (IXC); FDIV S2, S0, S1",
         0, (0xD503_437F, 0xD51B_4421, 0x1E21_1802), {1: 0x10}),
        ("SMSTOP SM (resets FPSR)", SM, (0xD503_427F,), {}),
        ("SMSTART SM; SMSTOP SM; MRS X3, FPSR", 0,
         (0xD503_437F, 0xD503_427F, 0xD53B_4423), {}),
    )
    drafts = []
    # IXC, IOC, QC, and QC with IOC and IXC. No case in streaming mode
    # throughout ends at 0x0800009F, which only a change of PSTATE.SM sets.
    for fpsr in (0x10, 0x01, 0x0800_0000, 0x0800_0011):
        for name, svcr, words, z, x in streaming:
            drafts.append(Draft(
                group="fpsr", name=f"FPSR {fpsr:#x} at {svcr_name(svcr)}: "
                f"{name}", words=words, svcr=svcr, fpsr=fpsr, z=dict(z),
                x=dict(x)))
        for name, svcr, words, x in entering:
            drafts.append(Draft(
                group="fpsr", name=f"FPSR {fpsr:#x} at {svcr_name(svcr)}: "
                f"{name}", words=words, svcr=svcr, fpsr=fpsr, x=dict(x)))
    return drafts


@functools.cache
def drafts() -> tuple[Draft, ...]:
    table = sme_table.load()
    result = (control_drafts() + classify_drafts(table) +
              fpsimd_drafts(table) + operand_drafts(table) +
              za_drafts(table) + memory_drafts(table) + fpsr_drafts())
    return tuple(result)


def request_bytes() -> bytes:
    return write_request([draft.wire() for draft in drafts()], SVL)


# --------------------------------------------------------------------------
# Records


def corpus_region(region: WireRegion, runs) -> dict:
    flags = READ_WRITE if region.flags & REGION_WRITABLE else READ_ONLY
    return {"address": hex64(region.address), "size": region.size,
            "flags": flags,
            "blob": hashlib.sha256(region.initial()).hexdigest(),
            "expected": [[offset, data.hex()] for offset, data in runs],
            "tags": [], "expected_tags": []}


def initial_fields(draft: Draft, initial: Dense) -> dict:
    base = Dense.base(SVL)
    out: dict[str, str] = {}
    for i in range(32):
        if initial.x[i] != base.x[i]:
            out["sp" if i == 31 else f"x{i}"] = hex64(initial.x[i])
    for name in ("nzcv", "fpcr", "fpsr", "svcr", "tpidr2"):
        if getattr(initial, name):
            out[name] = hex64(getattr(initial, name))
    for prefix, items, reference in (("v", draft.v, base.v),
                                     ("z", draft.z, base.z),
                                     ("p", draft.p, base.p),
                                     ("za", draft.za, base.za)):
        for i, value in items.items():
            if value != reference[i]:
                out[f"{prefix}{i}"] = value.hex()
    if draft.zt0 is not None and draft.zt0 != base.zt0:
        out["zt0"] = draft.zt0.hex()
    return out


class Finding(Exception):
    pass


# Whether an exit retires the instruction it stops at, as
# terminal_does_not_retire() in darwin/cpu/execution_types.hpp classifies
# the arm64 exits: an SVC or a BRK retires, leaving the PC after the SVC
# and at the BRK, and a memory fault, an illegal instruction, or a
# floating-point trap leaves the PC at an instruction that did not retire.
# The oracles' programs do not branch, so they raise no other exit.
RETIRES = {"system-call": True, "breakpoint": True, "memory-fault": False,
           "illegal-instruction": False, "floating-point-trap": False}


def retired_count(name: str, event: str, pc_offset: int) -> int:
    """The instructions a program retires when the oracle stops it with
    `event` at the word at `pc_offset` (for a system call, its SVC)."""
    if event not in RETIRES:
        raise Finding(f"{name}: {event} at offset {pc_offset:#x}, which a "
                      "program without branches cannot raise")
    return pc_offset // 4 + RETIRES[event]


def invalid_flag(draft: Draft) -> bool:
    """The case's invalid-encoding bit: the shared classifier's for an
    illegal-instruction case whose first word the tables settle, else the
    draft's."""
    if draft.expect != "illegal":
        return draft.invalid
    decision = invalid_encoding.classifier().decide(draft.words[0], ())
    return draft.invalid if decision is None else decision.invalid


def case_skeleton(draft: Draft, case: WireCase, program: int) -> dict:
    """The record case without what the hardware decides: its event,
    expected state, retired count, fault, and expected bytes."""
    initial = Dense.base(SVL).apply(case.state)
    item = {
        "name": draft.name, "program": program, "ordinals": [0, 0],
        "invalid_encoding": invalid_flag(draft), "minimization": "full",
        "native": list(draft.native),
        "initial": initial_fields(draft, initial),
    }
    if case.regions:
        item["memory"] = [corpus_region(region, ()) for region in
                          case.regions]
    return item


def sentinel_reads(case: WireCase, final: Dense) -> Dense:
    """The oracle runs each SVC of a program as BRK #7, so a literal load
    that reads an SVC word of its own program reads BRK #7's encoding
    there; an engine, and Linux, read the SVC. Put the SVC back in the
    loaded register."""
    word = case.words[case.entry // 4]
    if not literal_load(word):
        return final
    size = 4 << (word >> 30)
    offset = (word >> 5 & 0x7FFFF) << 2
    if offset & 1 << 20:
        offset -= 1 << 21
    rt = word & 31
    streaming = bool(final.svcr & SM)
    data = bytearray(final.z[rt] if streaming else final.v[rt])
    for slot in range(size // 4):
        index = (case.entry + offset) // 4 + slot
        if 0 <= index < len(case.words) and case.words[index] == SVC and \
                data[slot * 4:slot * 4 + 4] == SENTINEL.to_bytes(4, "little"):
            data[slot * 4:slot * 4 + 4] = SVC.to_bytes(4, "little")
    registers = list(final.z if streaming else final.v)
    registers[rt] = bytes(data)
    if streaming:
        return dataclasses.replace(final, z=registers)
    return dataclasses.replace(final, v=registers)


def record_case(draft: Draft, case: WireCase, result: WireResult,
                program: int) -> dict:
    if result.event == "fatal" or result.event.startswith("event-"):
        raise Finding(f"{draft.name}: the oracle could not attribute the run "
                      f"(pc offset {result.pc_offset:#x})")
    legal = result.event != "illegal-instruction"
    if draft.expect == "legal" and not legal or \
            draft.expect == "illegal" and legal:
        raise Finding(f"{draft.name}: {result.event} (ESR "
                      f"{result.esr:#010x}), the table expects "
                      f"{draft.expect}")
    words = len(case.words)
    if result.pc_offset % 4 or result.pc_offset // 4 >= words:
        raise Finding(f"{draft.name}: stopped outside its program")
    completed = result.event == "system-call"
    if completed and case.words[result.pc_offset // 4] != SVC:
        raise Finding(f"{draft.name}: completed at a word that is not SVC")
    initial = Dense.base(SVL).apply(case.state)
    final = sentinel_reads(case, initial.apply(result.state))
    item = case_skeleton(draft, case, program)
    item["event"] = result.event
    # The oracle stops at the SVC's sentinel; a completed system call
    # leaves the PC after the SVC, as Linux reports it.
    pc_offset = result.pc_offset + (4 if completed else 0)
    item["expected"] = expected_fields(
        initial, final, corpus.CODE_BASE + pc_offset,
        corpus.CODE_BASE + case.entry)
    item["retired"] = retired_count(draft.name, result.event,
                                    result.pc_offset)
    if result.event == "memory-fault":
        if result.esr >> 26 == 0x26:
            access = "write" if draft.store else "read"
        else:
            access = "write" if result.esr >> 6 & 1 else "read"
        item["fault"] = {"address": hex64(result.fault_address),
                         "access": access}
    if case.regions:
        item["memory"] = [corpus_region(region, runs)
                          for region, runs in zip(case.regions, result.runs)]
    return item


def project_defined(draft: Draft) -> bool:
    """Whether the virtual CPU's result for the case is the project's, not
    the SME host's: a word of the SM = 0 family (the vector-length words
    and the Z and P spill words) outside streaming mode, which the host
    traps and the virtual CPU executes."""
    return not draft.svcr & SM and \
        sme_table.sm0_family_word(draft.words[0])


def assemble(items: list[tuple[Draft, tuple[int, ...], dict]]) -> list[dict]:
    """The records holding the cases, each given with its draft and its
    program's words, in group order. The project-defined cases belong to no
    record."""
    records: dict[str, dict] = {}
    programs: dict[str, dict[tuple, int]] = {}
    for draft, words, item in items:
        if project_defined(draft):
            continue
        group = draft.group
        record = records.setdefault(group, {
            "record": f"{PRODUCER}/{group}", "producer": PRODUCER,
            "site": SITE, "profiles": ["4k"], "options": [],
            "svl": SVL, "programs": [], "cases": []})
        index = programs.setdefault(group, {})
        if words not in index:
            index[words] = len(record["programs"])
            record["programs"].append({
                "entry": 0,
                "words": [[4 * i, f"{word:08x}"]
                          for i, word in enumerate(words)]})
        record["cases"].append(dict(item, program=index[words]))
    return [records[group] for group in GROUPS if group in records]


def build_records(host: dict, results: list[WireResult]
                  ) -> tuple[list[dict], dict[str, bytes], dict]:
    items = drafts()
    cases = [draft.wire() for draft in items]
    if host["svl"] != SVL or not {"FEAT_SME", "FEAT_SME2"} <= \
            set(host["features"]):
        raise SystemExit("the response is not from an SME2 host with a "
                         f"{SVL}-byte SVL")
    blobs: dict[str, bytes] = {}
    findings = []
    counts = {"cases": len(items), "events": {}}
    recorded = []
    for draft, case, result in zip(items, cases, results):
        try:
            item = record_case(draft, case, result, 0)
        except Finding as finding:
            findings.append(str(finding))
            continue
        recorded.append((draft, case.words, item))
        counts["events"][item["event"]] = \
            counts["events"].get(item["event"], 0) + 1
        for region in case.regions:
            data = region.initial()
            blobs[hashlib.sha256(data).hexdigest()] = data
    if findings:
        for finding in findings:
            print(f"sme-native-corpus: {finding}", file=sys.stderr)
        raise SystemExit(f"sme-native-corpus: {len(findings)} cases disagree "
                         "with the encoding table")
    return assemble(recorded), blobs, counts


def script_hash() -> str:
    return hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()


def native_meta(request: bytes, host: dict, counts: dict) -> dict:
    return {
        "script": SITE, "sha256": script_hash(),
        "table": {"file": sme_table.TABLE_PATH,
                  "sha256": sme_table.load().sha256},
        "request_sha256": hashlib.sha256(request).hexdigest(),
        "svl": SVL, "host_features": host["features"],
        "cases": counts["cases"],
        "events": dict(sorted(counts["events"].items())),
    }


def read_dump(path: pathlib.Path) -> tuple[dict, list[str]]:
    with path.open(encoding="utf-8") as source:
        header = json.loads(next(source))
        lines = [line.rstrip("\n") for line in source if line.strip()]
    return header, lines


def is_blob(line: str) -> bool:
    return line.startswith('{"blob"')


def write_dump(path: pathlib.Path, header: dict, lines: list[str],
               blobs: dict[str, bytes], records: list[dict],
               position: int | None = None) -> None:
    """Writes the dump with the native records after its other records, or,
    given the position of the records a rewrite stripped (among the lines
    that are not blobs), where they were."""
    known = {json.loads(line)["blob"] for line in lines if is_blob(line)}
    # A dump exported before the SME profile tokens lacks those the native
    # records name; they go after its own, which keep their indices.
    platform = corpus.feature_tokens()
    used = {token for record in records for case in record["cases"]
            for token in case["native"]}
    if not used <= set(platform):
        raise SystemExit("sme-native-corpus: unknown native features " +
                         ", ".join(sorted(used - set(platform))))
    tokens = list(header["feature_tokens"])
    header["feature_tokens"] = tokens + [token for token in platform
                                         if token in used and
                                         token not in tokens]
    other = [line for line in lines if not is_blob(line)]
    if position is None:
        position = len(other)
    with path.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in lines:
            if is_blob(line):
                sink.write(line + "\n")
        for digest, data in blobs.items():
            if digest not in known:
                sink.write(json.dumps({"blob": digest,
                                       "zlib": zlib.compress(data, 9).hex()},
                                      sort_keys=True) + "\n")
        for line in other[:position]:
            sink.write(line + "\n")
        for record in records:
            sink.write(json.dumps(record, sort_keys=True) + "\n")
        for line in other[position:]:
            sink.write(line + "\n")


def strip_native(lines: list[str]) -> tuple[list[str], int | None]:
    """The lines without the native records, and the position (among those
    that are not blobs) of the first one removed."""
    kept = []
    position = None
    for line in lines:
        if not is_blob(line) and json.loads(line).get("producer") == PRODUCER:
            if position is None:
                position = sum(not is_blob(item) for item in kept)
            continue
        kept.append(line)
    return kept, position


def append(options: argparse.Namespace) -> int:
    header, lines = read_dump(options.input)
    lines, position = strip_native(lines)
    request = request_bytes()
    cases = [draft.wire() for draft in drafts()]
    responses = [path.read_bytes() for path in options.response]
    if any(response != responses[0] for response in responses[1:]):
        first = read_response(responses[0], cases, SVL)[1]
        for path, response in zip(options.response[1:], responses[1:]):
            other = read_response(response, cases, SVL)[1]
            for draft, one, two in zip(drafts(), first, other):
                if one != two:
                    print(f"sme-native-corpus: {path}: {draft.name} differs",
                          file=sys.stderr)
        raise SystemExit("sme-native-corpus: the responses differ")
    host, results = read_response(responses[0], cases, SVL)
    records, blobs, counts = build_records(host, results)
    header["meta"]["sme_native"] = native_meta(request, host, counts)
    write_dump(options.output, header, lines, blobs, records, position)
    print(f"appended {counts['cases']} native SME cases in {len(records)} "
          f"records: " + ", ".join(f"{name} {count}" for name, count in
                                   sorted(counts["events"].items())))
    return 0


def carry(options: argparse.Namespace) -> int:
    header, lines = read_dump(options.input)
    lines, position = strip_native(lines)
    pack, tokens = corpus.decode_pack(options.carry.read_bytes())
    meta = pack.meta.get("sme_native")
    request = request_bytes()
    if meta is None or \
            meta["request_sha256"] != hashlib.sha256(request).hexdigest():
        raise SystemExit("sme-native-corpus: the request changed since the "
                         "pack's native records; run the oracle again")
    native = [record for record in pack.records
              if record.producer == PRODUCER]
    dumped = [json.loads(line) for line in corpus.dump_corpus(
        corpus.Corpus(meta=pack.meta, blobs=pack.blobs, records=native),
        tokens)]
    blobs = {item["blob"]: zlib.decompress(bytes.fromhex(item["zlib"]))
             for item in dumped if "blob" in item}
    used = {region["blob"] for item in dumped if "cases" in item
            for case in item["cases"] for region in case.get("memory", [])}
    # The request is unchanged, so each group's cases keep their drafts' order
    # (names repeat where the table splits an encoding into rows); a pack that
    # split a group across several records is read from each in turn. They go
    # into the records this script now gives them.
    queues: dict[str, list[list]] = {}
    for item in dumped:
        if "cases" not in item:
            continue
        group = item["record"].split("/")[1]
        queues.setdefault(group, []).append([
            (tuple(int(word, 16) for _offset, word in
                   item["programs"][case["program"]]["words"]), case)
            for case in reversed(item["cases"])
            # A pack from before the family became the project's holds the
            # host's results for it.
            if not (not int(case["initial"].get("svcr", "0x0"), 16) & SM and
                    sme_table.sm0_family_word(int(item["programs"][
                        case["program"]]["words"][0][1], 16)))])
    carried = []
    for draft in drafts():
        if project_defined(draft):
            carried.append((draft, draft.wire().words, {}))
            continue
        for queue in queues.get(draft.group, ()):
            words, case = queue[-1] if queue else ((), {})
            if case.get("name") == draft.name and \
                    words == draft.wire().words:
                queue.pop()
                break
        else:
            raise SystemExit(f"sme-native-corpus: {draft.name}: the pack's "
                             "native cases do not answer the request")
        carried.append((draft, words, case))
    if any(queue for group in queues.values() for queue in group):
        raise SystemExit("sme-native-corpus: the pack holds native cases "
                         "the request lacks")
    records = assemble(carried)
    header["meta"]["sme_native"] = dict(meta, sha256=script_hash())
    write_dump(options.output, header, lines,
               {digest: blobs[digest] for digest in sorted(used)}, records,
               position)
    print(f"carried {sum(len(item['cases']) for item in records)} native SME "
          f"cases in {len(records)} records")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("input", type=pathlib.Path, nargs="?")
    parser.add_argument("output", type=pathlib.Path, nargs="?")
    parser.add_argument("--request", type=pathlib.Path,
                        help="write the oracle request and exit")
    parser.add_argument("--response", type=pathlib.Path, action="append",
                        default=[], help="an oracle response to the request; "
                        "several must be identical")
    parser.add_argument("--carry", type=pathlib.Path,
                        help="copy the native records of this pack instead")
    options = parser.parse_args()
    if options.request is not None:
        data = request_bytes()
        options.request.write_bytes(data)
        print(f"wrote {len(drafts())} cases, {len(data)} bytes, sha256 "
              f"{hashlib.sha256(data).hexdigest()}")
        return 0
    if options.input is None or options.output is None or \
            bool(options.response) == (options.carry is not None):
        parser.error("give INPUT OUTPUT and either --response or --carry")
    return carry(options) if options.carry is not None else append(options)


if __name__ == "__main__":
    sys.exit(main())
