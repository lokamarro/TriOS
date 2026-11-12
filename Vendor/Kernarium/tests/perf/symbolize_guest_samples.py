#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Symbolize a DARWIN_UML_GUEST_SAMPLES profile against guest ELF files.

Each sampled guest block carries the instruction words it was translated
from. Those words are found in the executable segments of the ELF files given
here, which places the block in a library and function without knowing where
the guest loaded it, and without any help from the guest. Pass the program's
executable and its shared libraries (for Android, the APK's lib/ directory and
/system/lib64); directories are searched recursively. Symbols come from each
file's symbol tables and, for Android's stripped libraries, from the
MiniDebugInfo ELF in its .gnu_debugdata section.

Blocks carry the guest thread pointer (TPIDR_EL0) they ran under, which
separates one busy thread from a pool. Profiles are cumulative: --baseline
subtracts an earlier dump of the same run to isolate the interval between them.

usage: symbolize_guest_samples.py SAMPLES ELF_OR_DIR... [--top N] [--min-words N]
       [--thread TPIDR] [--baseline EARLIER]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import array
import bisect
import collections
import dataclasses
import lzma
import pathlib
import struct
import sys


@dataclasses.dataclass
class Block:
    count: int
    address: int
    instructions: int
    words: tuple[int, ...]
    thread: int = 0


@dataclasses.dataclass
class Profile:
    header: str
    blocks: list[Block]
    host: list[tuple[int, str]]


def read_profile(path: pathlib.Path) -> Profile:
    header = ""
    blocks: list[Block] = []
    host: list[tuple[int, str]] = []
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("# guest code samples"):
            header = line[2:]
            continue
        fields = line.split("\t")
        if fields[0] == "guest" and len(fields) >= 5:
            words = tuple(int(word, 16) for word in fields[4].split(",") if word)
            thread = int(fields[5], 16) if len(fields) >= 6 else 0
            blocks.append(Block(int(fields[1]), int(fields[2], 16),
                                int(fields[3]), words, thread))
        elif fields[0] == "host" and len(fields) >= 3:
            host.append((int(fields[1]), fields[2]))
    return Profile(header, blocks, host)


def subtract(profile: Profile, baseline: Profile) -> Profile:
    """The samples taken after baseline was dumped from the same run."""
    earlier = collections.Counter()
    for block in baseline.blocks:
        earlier[(block.address, block.words, block.thread)] += block.count
    blocks = []
    for block in profile.blocks:
        count = block.count - earlier[(block.address, block.words, block.thread)]
        if count > 0:
            blocks.append(dataclasses.replace(block, count=count))
    earlier_host = collections.Counter()
    for count, name in baseline.host:
        earlier_host[name] += count
    host = [(count - earlier_host[name], name) for count, name in profile.host
            if count > earlier_host[name]]
    host.sort(key=lambda item: -item[0])
    blocks.sort(key=lambda block: -block.count)
    return Profile(profile.header, blocks, host)


@dataclasses.dataclass
class Elf:
    path: pathlib.Path
    segments: list[tuple[int, array.array]]  # (virtual address, words)
    starts: list[int]
    symbols: list[tuple[int, int, str]]  # (address, size, name), sorted


def is_elf64(data: bytes) -> bool:
    return (len(data) >= 64 and data[:4] == b"\x7fELF" and data[4] == 2 and
            data[5] == 1)


def elf_sections(data: bytes) -> list[tuple]:
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    return [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
            for i in range(shnum)] if shoff else []


def elf_section(data: bytes, wanted: str) -> bytes | None:
    sections = elf_sections(data)
    shstrndx, = struct.unpack_from("<H", data, 0x3E)
    if shstrndx >= len(sections):
        return None
    names = sections[shstrndx][4]
    for section in sections:
        start = names + section[0]
        if data[start:data.find(b"\0", start)].decode(errors="replace") == wanted:
            return data[section[4]:section[4] + section[5]]
    return None


def elf_symbols(data: bytes) -> list[tuple[int, int, str]]:
    symbols = []
    sections = elf_sections(data)
    for kind, link, entsize, offset, size in (
            (s[1], s[6], s[9], s[4], s[5]) for s in sections):
        if kind not in (2, 11) or entsize == 0:  # SHT_SYMTAB, SHT_DYNSYM
            continue
        strings = sections[link]
        base, limit = strings[4], strings[4] + strings[5]
        for at in range(offset, offset + size, entsize):
            name_offset, info, _, section, value, symbol_size = \
                struct.unpack_from("<IBBHQQ", data, at)
            # STT_NOTYPE or STT_FUNC, defined in a section (not SHN_ABS).
            if info & 0xF not in (0, 2) or section in (0, 0xFFF1):
                continue
            end = data.find(b"\0", base + name_offset, limit)
            name = data[base + name_offset:end].decode(errors="replace")
            if name and not name.startswith("$"):  # not a mapping symbol
                symbols.append((value, symbol_size, name))
    return symbols


def read_elf(path: pathlib.Path) -> Elf | None:
    data = path.read_bytes()
    if not is_elf64(data):
        return None
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    segments = []
    for index in range(phnum):
        kind, flags, offset, vaddr, _, filesz = struct.unpack_from(
            "<IIQQQQ", data, phoff + index * phentsize)
        if kind == 1 and flags & 1 and filesz >= 4:  # PT_LOAD, PF_X
            words = array.array("I")
            words.frombytes(data[offset:offset + filesz - filesz % 4])
            segments.append((vaddr, words))
    symbols = elf_symbols(data)
    # Android strips its libraries but keeps the local function symbols in an
    # xz-compressed ELF in .gnu_debugdata (MiniDebugInfo); without them every
    # hidden function reads as a bare address.
    packed = elf_section(data, ".gnu_debugdata")
    if packed:
        try:
            inner = lzma.decompress(packed)
        except lzma.LZMAError:
            inner = b""
        if is_elf64(inner):
            symbols += elf_symbols(inner)
    symbols = sorted(set(symbols))
    return Elf(path, segments, [s[0] for s in symbols], symbols)


def elf_files(arguments: list[pathlib.Path]) -> list[pathlib.Path]:
    found = []
    for argument in arguments:
        candidates = argument.rglob("*") if argument.is_dir() else [argument]
        for candidate in candidates:
            if candidate.is_file() and not candidate.is_symlink():
                with candidate.open("rb") as handle:
                    if handle.read(4) == b"\x7fELF":
                        found.append(candidate)
    return found


def symbol_for(elf: Elf, address: int) -> str:
    index = bisect.bisect_right(elf.starts, address) - 1
    if index >= 0:
        start, size, name = elf.symbols[index]
        if size == 0 or address < start + size:
            return f"{name}+{address - start:#x}"
    return f"{address:#x}"


def locate(blocks: list[Block], elves: list[Elf], minimum: int
           ) -> dict[int, tuple[Elf, int]]:
    """Map block index to (ELF, virtual address) where its words occur."""
    wanted = collections.defaultdict(list)
    for index, block in enumerate(blocks):
        if len(block.words) >= minimum:
            wanted[block.words[0]].append(index)
    located: dict[int, tuple[Elf, int]] = {}
    for elf in elves:
        for vaddr, words in elf.segments:
            for position, word in enumerate(words):
                candidates = wanted.get(word)
                if candidates is None:
                    continue
                for index in candidates:
                    if index in located:
                        continue
                    prefix = blocks[index].words
                    if tuple(words[position:position + len(prefix)]) == prefix:
                        located[index] = (elf, vaddr + 4 * position)
    return located


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples", type=pathlib.Path)
    parser.add_argument("elves", type=pathlib.Path, nargs="+")
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--min-words", type=int, default=4)
    parser.add_argument("--thread", type=lambda value: int(value, 16),
                        help="only blocks run under this TPIDR_EL0")
    parser.add_argument("--baseline", type=pathlib.Path,
                        help="an earlier dump of the same run to subtract")
    arguments = parser.parse_args()

    profile = read_profile(arguments.samples)
    if arguments.baseline is not None:
        profile = subtract(profile, read_profile(arguments.baseline))
    by_thread: collections.Counter[int] = collections.Counter()
    for block in profile.blocks:
        by_thread[block.thread] += block.count
    all_threads = sum(by_thread.values()) or 1
    if arguments.thread is not None:
        profile.blocks = [block for block in profile.blocks
                          if block.thread == arguments.thread]
    elves = [elf for elf in map(read_elf, elf_files(arguments.elves)) if elf]
    located = locate(profile.blocks, elves, arguments.min_words)
    total = sum(block.count for block in profile.blocks) or 1
    by_symbol: collections.Counter[tuple[str, str]] = collections.Counter()
    by_library: collections.Counter[str] = collections.Counter()
    for index, block in enumerate(profile.blocks):
        if index in located:
            elf, address = located[index]
            name = symbol_for(elf, address).split("+")[0]
            by_symbol[(elf.path.name, name)] += block.count
            by_library[elf.path.name] += block.count
        else:
            by_symbol[("[unmatched]", f"{block.address:#x}")] += block.count
            by_library["[unmatched]"] += block.count

    print(f"{profile.header}")
    print(f"guest samples {total}, located in {len(elves)} ELF files: "
          f"{100 * (total - by_library['[unmatched]']) / total:.1f}%")
    print("\nby guest thread (TPIDR_EL0):")
    for thread, count in by_thread.most_common(arguments.top):
        print(f"  {100 * count / all_threads:6.2f}%  {thread:#x}")
    print("\nby library:")
    for library, count in by_library.most_common(arguments.top):
        print(f"  {100 * count / total:6.2f}%  {library}")
    print("\nby function:")
    for (library, name), count in by_symbol.most_common(arguments.top):
        print(f"  {100 * count / total:6.2f}%  {name}  [{library}]")
    host_total = sum(count for count, _ in profile.host) or 1
    print(f"\nhost samples {host_total}:")
    for count, name in profile.host[:arguments.top]:
        print(f"  {100 * count / host_total:6.2f}%  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
