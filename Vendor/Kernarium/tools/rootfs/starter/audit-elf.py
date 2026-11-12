#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit every ELF file of a root tree against the 4 KiB guest page.

A guest kernel maps each PT_LOAD segment from a file offset that must be
congruent to its virtual address modulo the page size, and cannot map two
segments of different permissions from one page. The audit fails an ELF file
that the 4 KiB guest page cannot map. It also checks that the tree is the
userspace it claims to be (ELF class, machine and, for ARM, the hard-float
EABI) and that its dynamic linking is closed: every PT_INTERP and every
DT_NEEDED resolves inside the tree.

The audit reads ELF files only, so it runs on any host and executes nothing
from the tree. It writes one TSV row per ELF file, sorted by path, and exits
nonzero when any row carries a problem.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import dataclasses
import os
import stat
import struct
import sys
from pathlib import Path

GUEST_PAGE = 4096
ELF_MAGIC = b"\x7fELF"

ELFCLASS32 = 1
ELFCLASS64 = 2
ELFDATA2LSB = 1

ET_REL = 1
ET_EXEC = 2
ET_DYN = 3
TYPE_NAMES = {0: "none", ET_REL: "rel", ET_EXEC: "exec", ET_DYN: "dyn",
              4: "core"}

EM_ARM = 40
EM_AARCH64 = 183
EM_X86_64 = 62
MACHINES = {"arm": EM_ARM, "aarch64": EM_AARCH64, "x86_64": EM_X86_64}
MACHINE_NAMES = {number: name for name, number in MACHINES.items()}
CLASS_BITS = {ELFCLASS32: 32, ELFCLASS64: 64}

EF_ARM_EABIMASK = 0xFF000000
EF_ARM_EABI_VER5 = 0x05000000
EF_ARM_ABI_FLOAT_SOFT = 0x200
EF_ARM_ABI_FLOAT_HARD = 0x400

PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PF_X = 1
PF_W = 2
PF_R = 4

DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_STRSZ = 10
DT_SONAME = 14
DT_RPATH = 15
DT_RUNPATH = 29

# The directories glibc searches without a cache, after any RPATH/RUNPATH, in
# the order it searches them, with the multiarch directory of each machine.
MULTIARCH = {
    "arm": "arm-linux-gnueabihf",
    "aarch64": "aarch64-linux-gnu",
    "x86_64": "x86_64-linux-gnu",
}


class AuditError(RuntimeError):
    """The tree cannot be audited at all, as opposed to failing the audit."""


@dataclasses.dataclass(frozen=True)
class Segment:
    kind: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    flags: int
    align: int


@dataclasses.dataclass
class Elf:
    bits: int
    machine: int
    kind: int
    flags: int
    segments: list[Segment]
    interpreter: str | None
    needed: list[str]
    runpath: list[str]


def read_exact(stream, offset: int, length: int, what: str) -> bytes:
    stream.seek(offset)
    data = stream.read(length)
    if len(data) != length:
        raise AuditError(f"truncated {what}")
    return data


def parse_segments(stream, header: bytes, bits: int) -> tuple[int, int, int,
                                                            list[Segment]]:
    if bits == 32:
        (kind, machine, _, _, phoff, _, flags, _, phentsize, phnum) = \
            struct.unpack_from("<HHIIIIIHHH", header, 16)
        layout, size = "<IIIIIIII", 32
    else:
        (kind, machine, _, _, phoff, _, flags, _, phentsize, phnum) = \
            struct.unpack_from("<HHIQQQIHHH", header, 16)
        layout, size = "<IIQQQQQQ", 56
    if phnum and phentsize != size:
        raise AuditError(f"unexpected program header size {phentsize}")
    segments = []
    if phnum:
        table = read_exact(stream, phoff, phnum * size, "program headers")
        for index in range(phnum):
            fields = struct.unpack_from(layout, table, index * size)
            if bits == 32:
                (p_type, offset, vaddr, _, filesz, memsz, p_flags,
                 align) = fields
            else:
                (p_type, p_flags, offset, vaddr, _, filesz, memsz,
                 align) = fields
            segments.append(Segment(p_type, offset, vaddr, filesz, memsz,
                                    p_flags, align))
    return kind, machine, flags, segments


def read_c_string(blob: bytes, offset: int) -> str:
    end = blob.find(b"\0", offset)
    if offset >= len(blob) or end < 0:
        raise AuditError("unterminated dynamic string")
    return blob[offset:end].decode("utf-8", "surrogateescape")


def parse_dynamic(stream, bits: int, segments: list[Segment]
                  ) -> tuple[list[str], list[str]]:
    dynamic = next((s for s in segments if s.kind == PT_DYNAMIC), None)
    if dynamic is None:
        return [], []
    entry = 8 if bits == 32 else 16
    layout = "<iI" if bits == 32 else "<qQ"
    table = read_exact(stream, dynamic.offset, dynamic.filesz, "dynamic section")
    values: list[tuple[int, int]] = []
    for index in range(0, len(table) - entry + 1, entry):
        tag, value = struct.unpack_from(layout, table, index)
        if tag == DT_NULL:
            break
        values.append((tag, value))
    tags = dict(values)
    if DT_STRTAB not in tags or DT_STRSZ not in tags:
        return [], []
    # DT_STRTAB is a virtual address; the file offset comes from the PT_LOAD
    # segment that contains it.
    address = tags[DT_STRTAB]
    segment = next(
        (s for s in segments
         if s.kind == PT_LOAD and s.vaddr <= address < s.vaddr + s.filesz),
        None,
    )
    if segment is None:
        raise AuditError("the dynamic string table is in no loaded segment")
    strings = read_exact(stream, segment.offset + address - segment.vaddr,
                         tags[DT_STRSZ], "dynamic strings")
    needed = [read_c_string(strings, value)
              for tag, value in values if tag == DT_NEEDED]
    search = []
    for tag, value in values:
        if tag in (DT_RPATH, DT_RUNPATH):
            search += [part for part in read_c_string(strings, value).split(":")
                       if part]
    return needed, search


def parse_elf(path: Path) -> Elf | None:
    """Return the ELF description of the file, or None if it is not an ELF."""
    with path.open("rb") as stream:
        magic = stream.read(4)
        if magic != ELF_MAGIC:
            return None
        stream.seek(0)
        ident = stream.read(16)
        if len(ident) != 16:
            raise AuditError("truncated ELF identification")
        if ident[4] not in CLASS_BITS:
            raise AuditError(f"unknown ELF class {ident[4]}")
        if ident[5] != ELFDATA2LSB:
            raise AuditError("big-endian ELF")
        bits = CLASS_BITS[ident[4]]
        header_size = 52 if bits == 32 else 64
        header = read_exact(stream, 0, header_size, "ELF header")
        kind, machine, flags, segments = parse_segments(stream, header, bits)
        interpreter = None
        for segment in segments:
            if segment.kind == PT_INTERP:
                raw = read_exact(stream, segment.offset, segment.filesz,
                                 "interpreter")
                interpreter = raw.split(b"\0", 1)[0].decode(
                    "utf-8", "surrogateescape")
        needed, runpath = parse_dynamic(stream, bits, segments)
    return Elf(bits, machine, kind, flags, segments, interpreter, needed,
               runpath)


class Tree:
    """Path resolution inside a root directory, as a chroot would do it."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self._resolved: dict[str, Path | None] = {}

    def resolve(self, path: str) -> Path | None:
        """Return the real file `path` names inside the root, or None."""
        if path not in self._resolved:
            self._resolved[path] = self._walk(path)
        return self._resolved[path]

    def _walk(self, path: str, limit: int = 40) -> Path | None:
        pending = [part for part in path.split("/") if part]
        current = self.root
        walked = 0
        while pending:
            part = pending.pop(0)
            if part == ".":
                continue
            if part == "..":
                if current != self.root:
                    current = current.parent
                continue
            candidate = current / part
            try:
                mode = candidate.lstat().st_mode
            except OSError:
                return None
            if stat.S_ISLNK(mode):
                walked += 1
                if walked > limit:
                    return None
                target = os.readlink(candidate)
                if target.startswith("/"):
                    current = self.root
                pending = [p for p in target.split("/") if p] + pending
                continue
            current = candidate
        return current if current.exists() else None


def library_directories(tree: Tree, multiarch: str) -> list[str]:
    """The directories the dynamic loader searches without RPATH or cache."""
    directories: list[str] = []

    def add(value: str) -> None:
        if value.startswith("/") and value not in directories:
            directories.append(value)

    def read_configuration(path: Path, depth: int = 0) -> None:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeDecodeError):
            return
        for line in lines:
            line = line.split("#", 1)[0].strip()
            if line.startswith("include ") and depth < 4:
                pattern = line.split(None, 1)[1].strip()
                parent = tree.resolve(str(Path(pattern).parent))
                if parent is not None and parent.is_dir():
                    for item in sorted(parent.glob(Path(pattern).name)):
                        read_configuration(item, depth + 1)
            elif line:
                add(line)

    configuration = tree.resolve("/etc/ld.so.conf")
    if configuration is not None:
        read_configuration(configuration)
    for directory in (f"/lib/{multiarch}", f"/usr/lib/{multiarch}", "/lib",
                      "/usr/lib"):
        add(directory)
    return directories


@dataclasses.dataclass
class Row:
    path: str
    elf: Elf
    problems: list[str]


def expand_origin(entry: str, origin: str, multiarch: str) -> str:
    return (entry.replace("${ORIGIN}", origin).replace("$ORIGIN", origin)
            .replace("${LIB}", f"lib/{multiarch}").replace("$LIB", "lib")
            .replace("${PLATFORM}", "").replace("$PLATFORM", ""))


def segment_problems(elf: Elf) -> list[str]:
    """The reasons the guest page cannot map the file's segments."""
    loads = sorted((s for s in elf.segments if s.kind == PT_LOAD),
                   key=lambda s: s.vaddr)
    problems = []
    if any((s.vaddr - s.offset) % GUEST_PAGE for s in loads):
        problems.append("not-congruent")
    for before, after in zip(loads, loads[1:]):
        shared = ((before.vaddr + before.memsz - 1) // GUEST_PAGE
                  >= after.vaddr // GUEST_PAGE)
        if shared and before.flags != after.flags:
            problems.append("shares-page")
            break
    return problems


def audit_file(tree: Tree, relative: str, elf: Elf, options,
               directories: list[str], runpaths: list[str]) -> list[str]:
    problems: list[str] = []
    if options.machine is not None:
        if elf.machine != MACHINES[options.machine]:
            name = MACHINE_NAMES.get(elf.machine, f"em{elf.machine}")
            problems.append(f"machine-{name}")
        if elf.bits != options.bits:
            problems.append(f"class-{elf.bits}")
    if elf.machine == EM_ARM and options.machine == "arm":
        if elf.flags & EF_ARM_EABIMASK != EF_ARM_EABI_VER5:
            problems.append("not-eabi5")
        if elf.flags & EF_ARM_ABI_FLOAT_SOFT:
            problems.append("soft-float")
    problems += segment_problems(elf)
    if elf.interpreter is not None:
        if tree.resolve(elf.interpreter) is None:
            problems.append(f"interpreter-missing:{elf.interpreter}")
    if options.closed and elf.needed:
        origin = "/" + relative.rsplit("/", 1)[0] if "/" in relative else "/"
        multiarch = MULTIARCH.get(options.machine or "", "")
        # A library the loader already holds satisfies a later request for
        # its name, wherever the first request found it, so any RUNPATH of
        # the tree counts after the object's own and the default ones.
        search = [expand_origin(entry, origin, multiarch)
                  for entry in elf.runpath] + directories + runpaths
        for name in elf.needed:
            candidates = [name] if "/" in name else [
                f"{directory}/{name}" for directory in search]
            if not any(tree.resolve(candidate) is not None
                       for candidate in candidates):
                problems.append(f"needed-missing:{name}")
    return problems


def audit_tree(root: Path, options) -> list[Row]:
    tree = Tree(root)
    directories = library_directories(
        tree, MULTIARCH.get(options.machine or "", ""))
    multiarch = MULTIARCH.get(options.machine or "", "")
    found: list[tuple[str, Elf]] = []
    for directory, subdirectories, filenames in os.walk(root,
                                                        followlinks=False):
        subdirectories.sort()
        for filename in sorted(filenames):
            path = Path(directory, filename)
            try:
                mode = path.lstat().st_mode
            except FileNotFoundError:
                continue
            if not stat.S_ISREG(mode):
                continue
            relative = str(path.relative_to(root))
            if "\t" in relative or "\n" in relative:
                raise AuditError(f"path is not TSV-safe: {relative!r}")
            try:
                elf = parse_elf(path)
            except AuditError as error:
                raise AuditError(f"{relative}: {error}") from error
            if elf is not None:
                found.append((relative, elf))
    runpaths: list[str] = []
    for relative, elf in found:
        origin = "/" + relative.rsplit("/", 1)[0] if "/" in relative else "/"
        for entry in elf.runpath:
            expanded = expand_origin(entry, origin, multiarch)
            if expanded not in runpaths:
                runpaths.append(expanded)
    rows = [Row(relative, elf,
                audit_file(tree, relative, elf, options, directories, runpaths))
            for relative, elf in found]
    rows.sort(key=lambda row: row.path)
    return rows


def describe_abi(elf: Elf) -> str:
    if elf.machine != EM_ARM:
        return "-"
    if elf.flags & EF_ARM_EABIMASK != EF_ARM_EABI_VER5:
        return "other"
    if elf.flags & EF_ARM_ABI_FLOAT_HARD:
        return "eabi5-hard"
    if elf.flags & EF_ARM_ABI_FLOAT_SOFT:
        return "eabi5-soft"
    return "eabi5"


def format_row(row: Row) -> str:
    elf = row.elf
    loads = [s for s in elf.segments if s.kind == PT_LOAD]
    alignment = min((s.align for s in loads), default=0)
    fields = (
        row.path,
        str(elf.bits),
        MACHINE_NAMES.get(elf.machine, f"em{elf.machine}"),
        TYPE_NAMES.get(elf.kind, f"et{elf.kind}"),
        describe_abi(elf),
        elf.interpreter or "-",
        f"{alignment:#x}" if loads else "-",
        str(len(loads)),
        "none" if segment_problems(elf) else "4k",
        ";".join(row.problems) or "ok",
    )
    return "\t".join(fields)


def parse_arguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--machine", choices=sorted(MACHINES),
                        help="require every ELF to be for this machine")
    parser.add_argument("--bits", type=int, choices=(32, 64), default=None,
                        help="require this ELF class (default: the "
                        "machine's usual class)")
    parser.add_argument("--closed", action="store_true",
                        help="require every DT_NEEDED to resolve in the tree")
    options = parser.parse_args(argv)
    if options.bits is None:
        options.bits = 32 if options.machine == "arm" else 64
    return options


def main(argv: list[str] | None = None) -> int:
    options = parse_arguments(argv)
    root = options.root.resolve(strict=True)
    if not root.is_dir():
        raise AuditError(f"root is not a directory: {root}")
    if options.output.is_symlink():
        raise AuditError(f"refusing symlink output: {options.output}")
    rows = audit_tree(root, options)
    if not rows:
        raise AuditError(f"root contains no ELF files: {root}")
    with options.output.open("w", encoding="utf-8", newline="\n") as stream:
        for row in rows:
            stream.write(format_row(row) + "\n")
    failed = [row for row in rows if row.problems]
    print(f"audited {len(rows)} ELF files")
    for row in failed:
        print(f"audit-elf.py: {row.path}: {';'.join(row.problems)}",
              file=sys.stderr)
    if failed:
        print(f"audit-elf.py: {len(failed)} of {len(rows)} ELF files failed",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AuditError, OSError) as error:
        print(f"audit-elf.py: {error}", file=sys.stderr)
        raise SystemExit(2) from error
