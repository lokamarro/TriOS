#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize a gadget cache census written by a UML_GADGET_CACHE_CENSUS build.

This describes the records; tests/perf/README.md says how to take a capture.
Every executor's cache appends a snapshot of itself at teardown, and one every
DARWIN_UML_GADGET_CACHE_CENSUS_SECONDS seconds when that is set, taken by its
own vCPU at its next lookup. Both gadget engines write the same records, so
one capture reads the same way for either; a field an engine has no
counterpart for is simply absent. A snapshot is:

  E  the cache: its geometry; its measured footprint, which is the committed
     slabs, every resident translation's allocations as the allocator sized
     them, the link and indirect records and target tables, and the
     per-address-space indexes the cache owns (on arm64 also the body arena's
     chunks that hold no live body); the link sweeps, the records and sites
     they released and the time they took; the process's gadget-cache budget
     (budget=), what every address space is charged against it (metered=, of
     which metered_shedding= is in address spaces asked to shed), the most
     the open ones held when a store passed the budget (metered_peak=), and
     how many reliefs asked how many address spaces to shed (reliefs=,
     shed=), with what an x86 cache's own address spaces are charged
     (charged=); and the history below.
  G  each resident address space: its place in the window, how long it has
     been idle, its link state, and alive=: whether the MMU still has the
     generation (1), has destroyed it (0) or cannot say from the thread
     taking the snapshot (negative). An x86 cache asks as its runtime does at
     the start of each run, so it cannot say before its first, and adds the
     bytes of its own translations of the space (held=).
  S  an address space the executors share, written by every cache holding
     it: its arena, link and indirect bytes, alive=, what it is charged and
     whether it is shedding.
  P  each resident translation: its key, sizes, digest and body, and its
     allocation with the header, stream and validation bytes in it.
  L  for one in every DARWIN_UML_GADGET_CACHE_CENSUS_WORDS link records, the
     guest PC it links to.
  W  for one body in as many, its stream word by word: a gadget by its
     offset in the executable's text (g), a link or indirect record by guest
     PC (l, i), or another operand (o).

DARWIN_UML_GADGET_CACHE_CENSUS_TOTALS=1 writes only the E and G records and
leaves stores unmatched against other caches; read such a capture with
--series.

When executors share an address space's translations, an E line counts only
what its cache owns (slot slabs and slot lists) and each G line names the
space its slots point into. The report counts each space and each body once,
and the history counts the misses that adopted another cache's body.

The history counts how each lookup ended, why each departing translation left
(way eviction, address-space retirement or a discard) and, for every store,
whether the cache had held the same code under the same key before and why it
was gone, and whether another executor of the address space, or another
address space, translated it first. For a translation evicted and stored again
unchanged, the evictions in between go into a power-of-two histogram
(distance=), which says how many of those misses an exclusive victim tier of a
given size would have served. Recording never fails the cache: a departure or
store the census had no memory to remember is counted as unrecorded=, and the
report then calls the repeats lower bounds. A snapshot it had no memory to
finish is left out whole.

A translation is identified by the guest code it consumed rather than by the
stream it produced, because two executors translating the same code emit
equivalent streams that embed pointers to their own link records. That is what
makes the counts here answer how much of the cache is duplicate work: how much
would collapse if executors running one address space shared their bodies, and
how much more would collapse if a fork family's identical pages did too.

The report puts every part of the cache as a share of the measured bytes:
slot tags, body headers, streams, validation units, allocator rounding, link
and indirect records, duplicate bodies, the address spaces that are resident
but not running and the bytes of destroyed ones, with the budget line. The
lookup history gives the hit rate, why each translation that had to be made
again was gone, and how many of those an exclusive victim tier of a given size
would have served.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass, field
import math
from pathlib import Path
import sys
import zlib

MIB = 2**20
FINAL = "final"
# Bucket b of the distance histogram holds distances below 2**b.
VICTIM_SIZES = (1 << 12, 1 << 14, 1 << 16, 1 << 18, 1 << 20)
DORMANT_BUCKETS = ((0.0, 1.0), (1.0, 10.0), (10.0, 60.0), (60.0, math.inf))


@dataclass
class Block:
    """One cache's records for one snapshot."""
    snapshot: int | str
    tag: str
    fields: dict[str, object]
    start: int
    end: int

    def get(self, name: str, default: int = 0) -> int:
        value = self.fields.get(name, default)
        return value if isinstance(value, int) else default


@dataclass
class Translation:
    generation: int
    pc: int
    instructions: int
    words: int
    units: int
    digest: int
    # Absent from captures written before the census carried the parts.
    allocated: int | None = None
    header: int | None = None
    stream: int | None = None
    source: int | None = None
    # The body, which several caches hold when they share its space.
    body: int | None = None


@dataclass
class Space:
    generation: int
    current: bool
    rank: int
    idle_switches: int
    idle_seconds: float
    links: int
    indirect: int
    targets: int
    # The shared space this cache's slots point into, if any.
    space: str | None = None
    # Whether the MMU still has the generation (1), has destroyed it (0) or
    # could not say; and the bytes of the cache's translations of it, which
    # an engine with no shared spaces (x86) reports here.
    alive: int | None = None
    held: int = 0


@dataclass
class SharedSpace:
    """One address space's bodies and records, as one cache saw them."""
    tag: str
    generation: int
    fields: dict[str, object]

    def get(self, name: str) -> int:
        value = self.fields.get(name, 0)
        return value if isinstance(value, int) else 0


@dataclass
class Contents:
    block: Block
    spaces: list[Space] = field(default_factory=list)
    shared: list[SharedSpace] = field(default_factory=list)
    translations: list[Translation] = field(default_factory=list)
    # (translation, tokens) for every sampled stream.
    words: list[tuple[Translation, list[str]]] = field(default_factory=list)
    # (address space, guest PC, record's space) of every sampled link
    # record; the space is absent from captures of private caches.
    links: list[tuple[int, int, str | None]] = field(default_factory=list)


def parse_value(text: str) -> object:
    if "," in text:
        return [int(part) for part in text.split(",")]
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text


def scan(path: Path) -> list[Block]:
    """Every cache snapshot in the capture, with where its records lie."""
    blocks: list[Block] = []
    offset = 0
    with path.open("rb") as stream:
        for line in stream:
            if line.startswith(b"E "):
                if blocks:
                    blocks[-1].end = offset
                fields: dict[str, object] = {}
                parts = line.decode().split()
                for pair in parts[2:]:
                    name, _, value = pair.partition("=")
                    fields[name] = parse_value(value)
                snapshot = fields.get("snapshot", FINAL)
                blocks.append(Block(
                    snapshot=snapshot if isinstance(snapshot, int) else FINAL,
                    tag=parts[1], fields=fields,
                    start=offset + len(line), end=-1))
            elif not blocks and line.strip():
                raise SystemExit(f"{path}: a record precedes its executor")
            offset += len(line)
    if blocks:
        blocks[-1].end = offset
    return blocks


def load(path: Path, block: Block) -> Contents:
    contents = Contents(block)
    with path.open("rb") as stream:
        stream.seek(block.start)
        text = stream.read(block.end - block.start).decode()
    previous: Translation | None = None
    for line in text.splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "P":
            numbers = [int(value, 0) for value in fields[1:12]]
            translation = Translation(*numbers[:6])
            if len(numbers) >= 10:
                (translation.allocated, translation.header,
                 translation.stream, translation.source) = numbers[6:10]
            if len(numbers) >= 11:
                translation.body = numbers[10]
            contents.translations.append(translation)
            previous = translation
        elif fields[0] == "G":
            values = {name: parse_value(value) for name, _, value in
                      (pair.partition("=") for pair in fields[2:])}
            contents.spaces.append(Space(
                generation=int(fields[1]),
                current=values.get("current") == 1,
                rank=int(values.get("rank", 0)),
                idle_switches=int(values.get("idle_switches", 0)),
                idle_seconds=float(values.get("idle_seconds", 0.0)),
                links=int(values.get("links", 0)),
                indirect=int(values.get("indirect", 0)),
                targets=int(values.get("targets", 0)),
                space=(str(values["space"]) if "space" in values
                       else None),
                alive=(int(values["alive"]) if "alive" in values else None),
                held=int(values.get("held", 0))))
        elif fields[0] == "S":
            values = {name: parse_value(value) for name, _, value in
                      (pair.partition("=") for pair in fields[2:])}
            contents.shared.append(SharedSpace(
                tag=fields[1], generation=int(values.get("generation", 0)),
                fields=values))
        elif fields[0] == "W" and previous is not None:
            contents.words.append((previous, fields[2:]))
        elif fields[0] == "L":
            contents.links.append((int(fields[1]), int(fields[2], 16),
                                   fields[3] if len(fields) > 3 else None))
    return contents


def snapshot_order(snapshot: int | str) -> float:
    return math.inf if snapshot == FINAL else float(snapshot)


def select(blocks: list[Block], wanted: str) -> list[Block]:
    snapshots = sorted({block.snapshot for block in blocks}, key=snapshot_order)
    if wanted == "last":
        chosen = snapshots[-1]
    elif wanted == FINAL:
        if FINAL not in snapshots:
            raise SystemExit("no teardown snapshot; pass --snapshot last")
        chosen = FINAL
    else:
        chosen = int(wanted)
    return [block for block in blocks if block.snapshot == chosen]


def engine_of(blocks: list[Block]) -> str:
    return "x86-64" if any("decoded" in b.fields for b in blocks) else "arm64"


def round_up(value: int, granule: int = 16) -> int:
    return (max(value, granule) + granule - 1) // granule * granule


def mib(value: float) -> str:
    return f"{value / MIB:8.1f} MiB"


def share(value: float, total: float) -> str:
    return f"{100.0 * value / total:5.1f}%" if total else "    -"


def slab_parts(block: Block, slot_bytes_default: int) -> tuple[int, int]:
    """The slot tags and the rest of the slab bytes (x86 successor sets and
    allocator rounding)."""
    slabs = block.get("slabs")
    slot_bytes = block.get("slot_bytes", slot_bytes_default)
    slab_slots = block.get("slab_slots", 256)
    count = block.get("slab_count", -1)
    if count < 0:
        count = slabs // (slab_slots * slot_bytes)
    tags = count * slab_slots * slot_bytes
    return tags, slabs - tags


def histories(blocks: list[Block]) -> Counter[str]:
    total: Counter[str] = Counter()
    for block in blocks:
        for name, value in block.fields.items():
            if isinstance(value, int):
                total[name] += value
    return total


def shared_spaces(contents: list[Contents]) -> dict[tuple[str, int],
                                                  SharedSpace]:
    """Each shared space once. Caches write their snapshots one after
    another, so the latest record of a space stands for it."""
    spaces: dict[tuple[str, int], SharedSpace] = {}
    for content in contents:
        for space in content.shared:
            spaces[(space.tag, space.generation)] = space
    return spaces


SPACE_PARTS = ("stream_index", "streams", "arena_empty", "links", "indirect")


def dead_spaces(contents: list[Contents]) -> tuple[int, int]:
    """How many address spaces the caches hold for a generation the MMU has
    destroyed (`alive=0`), and the bytes of their arenas, indexes and
    records. An engine whose caches keep their spaces to themselves (x86)
    says so on the `G` record, with the bytes of the translations held.
    Absent from captures written before the census said."""
    dead = [space for space in shared_spaces(contents).values()
            if space.fields.get("alive") == 0]
    count = len(dead)
    total = sum(space.get(name) for space in dead
                for name in SPACE_PARTS if name != "arena_empty")
    for content in contents:
        for space in content.spaces:
            if space.space is None and space.alive == 0:
                count += 1
                total += (space.links + space.indirect + space.targets +
                          space.held)
    return count, total


def measured(blocks: list[Block], contents: list[Contents]) -> Counter[str]:
    """The snapshot's bytes by part: what each cache owns, and each shared
    space once."""
    parts: Counter[str] = Counter()
    for block in blocks:
        for name in ("total", "slabs", "decoded", "targets") + SPACE_PARTS:
            parts[name] += block.get(name)
    for space in shared_spaces(contents).values():
        for name in SPACE_PARTS:
            parts[name] += space.get(name)
        parts["total"] += sum(space.get(name) for name in SPACE_PARTS
                              if name != "arena_empty")
    return parts


def distinct_bodies(contents: list[Contents]) -> list[Translation]:
    """Every resident body once: a body several caches hold is one."""
    seen: set[int] = set()
    bodies: list[Translation] = []
    for content in contents:
        for item in content.translations:
            if item.body is not None:
                if item.body in seen:
                    continue
                seen.add(item.body)
            bodies.append(item)
    return bodies


@dataclass
class Dormancy:
    spaces: int = 0
    bodies: int = 0
    state: int = 0


def dormancy(contents: list[Contents],
             buckets: tuple[tuple[float, float], ...]) -> list[Dormancy]:
    """The address spaces resident but running nowhere, by how long the
    one that ran them last has been away, with their bodies and link state
    counted once."""
    shared = shared_spaces(contents)
    # A space is its shared record where the cache names one, else the
    # cache's own state for that address space.
    idle: dict[tuple[str, int], float] = {}
    state: Counter[tuple[str, int]] = Counter()
    space_of: dict[tuple[str, int], tuple[str, int]] = {}
    for content in contents:
        for space in content.spaces:
            key = ((space.space, space.generation) if space.space
                   else (content.block.tag, space.generation))
            space_of[(content.block.tag, space.generation)] = key
            seconds = 0.0 if space.current else space.idle_seconds
            idle[key] = min(idle.get(key, math.inf), seconds)
            state[key] += space.links + space.indirect + space.targets
            if space.current:
                idle[key] = -1.0
    for key, space in shared.items():
        state[key] += space.get("links") + space.get("indirect")
    bodies: Counter[tuple[str, int]] = Counter()
    seen: set[int] = set()
    for content in contents:
        for item in content.translations:
            if item.body is not None:
                if item.body in seen:
                    continue
                seen.add(item.body)
            key = space_of.get((content.block.tag, item.generation))
            if key is not None:
                bodies[key] += item.allocated or 0
    result = [Dormancy() for _ in buckets]
    for key, seconds in idle.items():
        for index, (low, high) in enumerate(buckets):
            if seconds >= 0.0 and low <= seconds < high:
                result[index].spaces += 1
                result[index].bodies += bodies[key]
                result[index].state += state[key]
    return result


def distances(blocks: list[Block]) -> list[int]:
    merged: list[int] = []
    for block in blocks:
        value = block.fields.get("distance")
        if isinstance(value, int):
            value = [value]
        if not isinstance(value, list):
            continue
        if len(merged) < len(value):
            merged.extend([0] * (len(value) - len(merged)))
        for index, count in enumerate(value):
            merged[index] += count
    return merged


@dataclass
class Duplication:
    resident: int = 0
    bytes: int = 0
    # Copies of a translation another cache of the same address space holds.
    space_copies: int = 0
    space_bytes: int = 0
    # Beyond those, copies of the same code another address space holds.
    family_copies: int = 0
    family_bytes: int = 0
    distinct_in_space: int = 0
    distinct_code: int = 0
    # Sampled link records, and how many are distinct within their space.
    links_sampled: int = 0
    links_distinct: int = 0


def duplication(contents: list[Contents]) -> Duplication:
    """Copies of one translation, counting a body several caches share as
    one body."""
    result = Duplication()
    by_space: dict[tuple[int, int, int], list[int]] = defaultdict(list)
    seen: set[int] = set()
    for content in contents:
        for item in content.translations:
            result.resident += 1
            if item.body is not None:
                if item.body in seen:
                    continue
                seen.add(item.body)
            size = item.allocated if item.allocated is not None else 0
            by_space[(item.generation, item.pc, item.digest)].append(size)
            result.bytes += size
    by_code: dict[tuple[int, int], list[int]] = defaultdict(list)
    for (generation, pc, digest), sizes in by_space.items():
        result.space_copies += len(sizes) - 1
        result.space_bytes += sum(sizes) - max(sizes)
        by_code[(pc, digest)].append(max(sizes))
    for sizes in by_code.values():
        result.family_copies += len(sizes) - 1
        result.family_bytes += sum(sizes) - max(sizes)
    result.distinct_in_space = len(by_space)
    result.distinct_code = len(by_code)
    # A record a shared space holds is sampled once per cache holding the
    # space, but is one record.
    sampled: list[tuple[int, int]] = []
    records: set[tuple[str, int, int]] = set()
    for content in contents:
        for generation, pc, space in content.links:
            if space is not None:
                if (space, generation, pc) in records:
                    continue
                records.add((space, generation, pc))
            sampled.append((generation, pc))
    result.links_sampled = len(sampled)
    result.links_distinct = len(set(sampled))
    return result


def varint(value: int) -> int:
    return max(1, (value.bit_length() + 6) // 7)


def zigzag(value: int) -> int:
    value &= (1 << 64) - 1
    if value >= 1 << 63:
        value -= 1 << 64
    return (value << 1) ^ (value >> 63)


@dataclass
class Codec:
    bodies: int = 0
    stored: int = 0
    words: int = 0
    tokens: Counter[str] = field(default_factory=Counter)
    model: float = 0.0
    deflated: int = 0


def codec(contents: list[Contents]) -> Codec:
    """Size the sampled bodies under the victim codec C2 proposes: a gadget
    by its rank among the gadgets the sample uses, a link or indirect record
    by the guest PC it names relative to the block, any other word as a
    signed or PC-relative varint, and the validation units as they are;
    zlib on the raw words is the general-purpose reference."""
    sample: list[tuple[Translation, list[str]]] = []
    seen: set[int] = set()
    for content in contents:
        for entry in content.words:
            body = entry[0].body
            if body is not None:
                if body in seen:
                    continue
                seen.add(body)
            sample.append(entry)
    result = Codec()
    if not sample:
        return result
    gadgets: Counter[str] = Counter()
    for _, tokens in sample:
        gadgets.update(token for token in tokens if token[0] == "g")
    rank = {name: index for index, (name, _) in
            enumerate(gadgets.most_common())}
    for translation, tokens in sample:
        result.bodies += 1
        result.words += len(tokens)
        if translation.allocated is not None:
            result.stored += translation.allocated
        source = translation.source or 0
        # A count byte, the instruction count and the source units.
        cost = 2.0 + source
        raw = bytearray()
        for token in tokens:
            kind, value = token[0], int(token[1:], 16)
            result.tokens[kind] += 1
            raw += value.to_bytes(8, "little")
            if kind == "g":
                cost += 1 if rank[token] < 128 else 2
            elif kind in "li":
                cost += varint(zigzag(value - translation.pc))
            else:
                cost += 0.25 + min(varint(zigzag(value)),
                                   varint(zigzag(value - translation.pc)))
        result.model += cost
        result.deflated += len(zlib.compress(bytes(raw), 9)) + 2 + source
    return result


def victim_served(histogram: list[int], entries: int) -> int:
    bound = int(math.log2(entries))
    return sum(histogram[: bound + 1])


def budget_of(blocks: list[Block]) -> dict[str, int]:
    """The process's gadget-cache budget as the snapshot's caches saw it: what
    every address space was charged, and how often the budget was passed.
    Each cache reports the whole process, so the latest view counts. Empty
    for captures written before the census said."""
    fields = ("budget", "metered", "metered_shedding", "metered_peak",
              "reliefs", "shed")
    seen = [b for b in blocks if "budget" in b.fields]
    if not seen:
        return {}
    latest = max(seen, key=lambda b: float(b.fields.get("seconds", 0.0)))
    return {name: latest.get(name) for name in fields}


def report(path: Path, blocks: list[Block], arguments: argparse.Namespace) -> None:
    contents = [load(path, block) for block in blocks]
    engine = engine_of(blocks)
    snapshot = blocks[0].snapshot
    seconds = max(float(b.fields.get("seconds", 0.0)) for b in blocks)
    resident = sum(len(c.translations) for c in contents)
    shared = shared_spaces(contents)
    print(f"{engine} snapshot {snapshot} at {seconds:.1f} s: "
          f"{len(blocks)} caches, {resident:,} resident translations")
    if shared:
        attached = sum(space.get("attached") for space in shared.values())
        print(f"  {len(shared)} address-space records, held by "
              f"{attached / len(shared):.2f} caches each")
    dead, dead_bytes = dead_spaces(contents)
    if dead:
        print(f"  {dead} address spaces held for destroyed generations, "
              f"{dead_bytes / MIB:.1f} MiB")
    budget = budget_of(blocks)
    if budget:
        print(f"  gadget-cache budget {budget['budget'] / MIB:.1f} MiB: "
              f"{budget['metered'] / MIB:.1f} MiB charged, "
              f"{budget['metered_shedding'] / MIB:.1f} MiB of it shedding; "
              f"{budget['reliefs']:,} reliefs shed "
              f"{budget['shed']:,} address spaces")
    if resident == 0:
        return

    print("\nper cache")
    for content in contents:
        block = content.block
        slots = block.get("slots", arguments.slots)
        lookups = block.get("lookups")
        rate = ((block.get("hits") + block.get("revalidated")) / lookups
                if lookups else float("nan"))
        # A cache sharing its spaces owns only its slots and slot lists.
        label = "own" if content.shared else "cache"
        print(f"  {block.tag}  spaces {block.get('generations'):>2}  "
              f"resident {len(content.translations):>8,}  "
              f"occupancy {100.0 * block.get('occupancy') / slots:5.1f}%  "
              f"{label} {block.get('total') / MIB:7.1f} MiB  "
              f"hit rate {100.0 * rate:5.1f}%")

    parts = measured(blocks, contents)
    total = parts["total"]
    tags = rest = 0
    for block in blocks:
        part_tags, part_rest = slab_parts(block, arguments.slot_bytes)
        tags += part_tags
        rest += part_rest
    translations = distinct_bodies(contents)
    have_parts = all(t.allocated is not None for t in translations)
    print(f"\nwhere the bytes are, of {total / MIB:.1f} MiB measured")
    rows: list[tuple[str, int]] = [("slot tags", tags)]
    if engine == "x86-64":
        rows.append(("successor sets and slab rounding", rest))
    rows.append(("stream lookup maps", parts["stream_index"]))
    allocated = sum(t.allocated or 0 for t in translations)
    if have_parts:
        headers = sum(t.header or 0 for t in translations)
        streams = sum(t.stream or 0 for t in translations)
        sources = sum(t.source or 0 for t in translations)
        rows += [("translation headers", headers),
                 ("streams", streams),
                 ("validation units", sources),
                 ("allocation rounding",
                  allocated - headers - streams - sources)]
        if engine == "arm64":
            arena = parts["streams"]
            if any("arena_empty" in b.fields for b in blocks):
                # Whole chunks with no live body, apart from the free
                # space between the bodies of chunks that still hold one.
                empty = parts["arena_empty"]
                rows += [("arena chunks holding no body", empty),
                         ("free space in chunks holding a body",
                          arena - allocated - empty)]
            else:
                rows.append(("arena chunks not holding a body",
                             arena - allocated))
    else:
        rows.append(("bodies and streams", parts["streams"]))
    rows += [("replay decodes", parts["decoded"]),
             ("link records" + (" and slot lists" if shared else ""),
              parts["links"]),
             ("indirect sites", parts["indirect"]),
             ("target tables", parts["targets"])]
    for name, value in rows:
        if value:
            print(f"  {name:34s} {mib(value)}  {share(value, total)}")

    dup = duplication(contents)
    print("\nduplicate translations")
    if shared:
        print(f"  bodies {len(translations):>10,}, held by "
              f"{resident / max(len(translations), 1):.3f} slots each")
    print(f"  distinct within an address space {dup.distinct_in_space:>10,}   "
          f"factor {resident / max(dup.distinct_in_space, 1):.3f}x")
    print(f"  distinct ignoring address space  {dup.distinct_code:>10,}   "
          f"factor {resident / max(dup.distinct_code, 1):.3f}x")
    if have_parts:
        print(f"  copies another cache of the same address space holds "
              f"{dup.space_copies:>9,}  {mib(dup.space_bytes)}  "
              f"{share(dup.space_bytes, total)} of the cache")
        print(f"  copies another address space holds besides          "
              f"{dup.family_copies:>9,}  {mib(dup.family_bytes)}  "
              f"{share(dup.family_bytes, total)} of the cache")
    if dup.links_sampled:
        links = parts["links"]
        spare = links * (1 - dup.links_distinct / dup.links_sampled)
        print(f"  link records another cache of the same space keeps, "
              f"from {dup.links_sampled:,} sampled: "
              f"{100.0 * (1 - dup.links_distinct / dup.links_sampled):.1f}%"
              f"  {mib(spare)}  {share(spare, total)} of the cache")

    print("\naddress spaces resident but not running")
    dormant_total = 0
    for (low, high), idle in zip(DORMANT_BUCKETS,
                                 dormancy(contents, DORMANT_BUCKETS)):
        dormant_total += idle.bodies + idle.state
        label = f"idle {low:g}-{high:g} s" if high != math.inf else f"idle >{low:g} s"
        print(f"  {label:16s} spaces {idle.spaces:>4}  "
              f"translations {mib(idle.bodies)}  "
              f"link state {mib(idle.state)}  "
              f"{share(idle.bodies + idle.state, total)}")
    print(f"  {'all':16s} {'':11s} {mib(dormant_total)} "
          f"{share(dormant_total, total)} of the cache")

    history = histories(blocks)
    lookups = history["lookups"]
    if lookups:
        hits = history["hits"] + history["revalidated"]
        print("\nlookups since the run began")
        print(f"  lookups {lookups:,}   hit rate {100.0 * hits / lookups:.2f}%"
              f"   revalidated {100.0 * history['revalidated'] / lookups:.2f}%"
              f"   misses {history['misses']:,}")
        if history["adopted"]:
            served = hits + history["adopted"]
            print(f"  adopted from another cache of the space "
                  f"{history['adopted']:,}   served without translating "
                  f"{100.0 * served / lookups:.2f}%")
        stores = history["stores"]
        print(f"  stores {stores:,}")
        for name, label in (("first", "first in this cache"),
                            ("again_evicted", "again after way eviction"),
                            ("again_retired", "again after address-space retirement"),
                            ("again_discarded", "again after a discard"),
                            ("changed", "again with different code"),
                            ("restored", "beside a copy still resident")):
            print(f"    {label:38s} {history[name]:>11,}  "
                  f"{share(history[name], stores)}")
        print(f"  translated first by another executor of the space "
              f"{history['other_executor']:>11,}  "
              f"{share(history['other_executor'], stores)}")
        print(f"  translated first in another address space          "
              f"{history['other_space']:>11,}  "
              f"{share(history['other_space'], stores)}")
        if history["unrecorded"]:
            print(f"  departures and stores the census had no memory to "
                  f"record {history['unrecorded']:,}: the repeats above "
                  f"are lower bounds")
        evictions = history["evictions"]
        mean_evicted = history["evicted_bytes"] / evictions if evictions else 0.0
        print(f"  departures: evicted {evictions:,} (mean {mean_evicted:.0f} B)"
              f"  retired {history['retirements']:,}"
              f"  discarded {history['discards']:,}")
        histogram = distances(blocks)
        if evictions:
            print("  an exclusive victim tier of V bodies would have served")
            for entries in VICTIM_SIZES:
                served = victim_served(histogram, entries)
                print(f"    V {entries:>9,}  {served:>11,} returns  "
                      f"{share(served, history['misses'])} of misses  "
                      f"hit rate {100.0 * (hits + served) / lookups:.2f}%  "
                      f"holding {mib(entries * mean_evicted)} uncompressed per cache")

    if have_parts:
        print("\nwhat digest validation (C4) would free")
        digest8 = digest16 = 0
        for t in translations:
            assert t.allocated is not None and t.header is not None
            assert t.stream is not None and t.source is not None
            if engine == "arm64":
                digest8 += t.allocated - round_up(t.header + t.stream + 8)
                digest16 += t.allocated - round_up(t.header + t.stream + 16)
            else:
                digest8 += round_up(t.source) - 8
                digest16 += round_up(t.source) - 16
        print(f"  validation units for an 8-byte digest   {mib(digest8)}  "
              f"{share(digest8, total)}")
        print(f"  validation units for a 16-byte digest   {mib(digest16)}  "
              f"{share(digest16, total)}")

    sampled = codec(contents)
    if sampled.bodies:
        print(f"\nsampled streams: {sampled.bodies:,} bodies, "
              f"{sampled.words:,} words "
              + "  ".join(f"{kind} {100.0 * count / sampled.words:.1f}%"
                          for kind, count in sorted(sampled.tokens.items())))
        if sampled.stored:
            print(f"  stored {sampled.stored:,} B   victim codec "
                  f"{sampled.model:,.0f} B ({sampled.stored / sampled.model:.2f}x)"
                  f"   zlib -9 {sampled.deflated:,} B "
                  f"({sampled.stored / sampled.deflated:.2f}x)")

    lengths = Counter(t.words for t in translations)
    print("\nstream length, in words")
    ordered = sorted(lengths.elements())
    bodies = len(ordered)
    print(f"  mean {sum(ordered) / bodies:.1f}   median {ordered[bodies // 2]}")
    previous = 0
    for bound in (4, 8, 16, 24, 32, 48, 64, 96):
        count = sum(n for length, n in lengths.items() if previous < length <= bound)
        print(f"    {previous + 1:>3}-{bound:<3} {count:>9,} {100.0 * count / bodies:5.1f}%")
        previous = bound
    count = sum(n for length, n in lengths.items() if length > previous)
    if count:
        print(f"    >{previous:<6} {count:>9,} {100.0 * count / bodies:5.1f}%")


def series(path: Path, blocks: list[Block]) -> None:
    """One line per snapshot: what the caches held, and how the lookups
    since the previous snapshot went."""
    snapshots = sorted({b.snapshot for b in blocks}, key=snapshot_order)
    print(f"{'snapshot':>8} {'s':>7} {'caches':>6} {'resident':>9} "
          f"{'MiB':>7} {'bodies':>7} {'dup/space':>9} {'dup/code':>8} "
          f"{'dormant':>7} {'dead MiB':>8} {'lookups':>11} {'hit rate':>8} "
          f"{'stores':>9} {'evicted':>8}")
    previous: dict[str, Block] = {}
    for snapshot in snapshots:
        chosen = [b for b in blocks if b.snapshot == snapshot]
        contents = [load(path, block) for block in chosen]
        dup = duplication(contents)
        total = measured(chosen, contents)["total"]
        dead = dead_spaces(contents)[1]
        dormant = sum(idle.bodies + idle.state for idle in
                      dormancy(contents, ((0.0, math.inf),)))
        delta: Counter[str] = Counter()
        for block in chosen:
            before = previous.get(block.tag)
            for name in ("lookups", "hits", "revalidated", "stores",
                         "evictions"):
                delta[name] += block.get(name) - (before.get(name) if before else 0)
            previous[block.tag] = block
        rate = ((delta["hits"] + delta["revalidated"]) / delta["lookups"]
                if delta["lookups"] else float("nan"))
        seconds = max(float(b.fields.get("seconds", 0.0)) for b in chosen)
        print(f"{snapshot!s:>8} {seconds:7.1f} {len(chosen):6d} {dup.resident:9,} "
              f"{total / MIB:7.1f} {dup.bytes / MIB:7.1f} "
              f"{share(dup.space_bytes, total):>9} {share(dup.family_bytes, total):>8} "
              f"{share(dormant, total):>7} {dead / MIB:8.1f} "
              f"{delta['lookups']:11,} {100.0 * rate:7.2f}% "
              f"{delta['stores']:9,} {delta['evictions']:8,}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("census", type=Path,
                        help="file written by DARWIN_UML_GADGET_CACHE_CENSUS")
    parser.add_argument("--snapshot", default=None,
                        help="final (the default when the capture has one), "
                             "last, or a periodic snapshot's number")
    parser.add_argument("--series", action="store_true",
                        help="one line per snapshot instead of one report")
    parser.add_argument("--slots", type=int, default=65536,
                        help="cache slots per executor, for a record that "
                             "does not carry its own (%(default)s)")
    parser.add_argument("--slot-bytes", type=int, default=40,
                        help="bytes per cache slot, for a record that does "
                             "not carry its own (%(default)s)")
    arguments = parser.parse_args()

    blocks = scan(arguments.census)
    if not blocks:
        raise SystemExit(f"{arguments.census}: no executor records")
    if arguments.series:
        series(arguments.census, blocks)
        return 0
    wanted = arguments.snapshot
    if wanted is None:
        wanted = FINAL if any(b.snapshot == FINAL for b in blocks) else "last"
    report(arguments.census, select(blocks, wanted), arguments)
    return 0


if __name__ == "__main__":
    sys.exit(main())
