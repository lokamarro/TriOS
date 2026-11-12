#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Report what each group of the x86-64 gadget engine needs from its slot.

region.inc gives every group a slot in the bank and in the cold part, and a
group that outgrows its slot fails assembly at the `.org` that closes it; every
group after the first to fail then fails too, because the location counter has
already passed its slot. This assembles engine.S once with the group macros
replaced by labels, so the groups pack with no slots, and reports each group's
code against the table:

    group  part  phase  code  slot  room  need

`need` is the slot the README's rule gives a group that no longer fits: its
phase and code plus 1 KiB, rounded up to a 128-byte line. A group's code here
can differ from the pinned region's by the alignment of its first gadget, up
to 12 bytes, which the 1 KiB absorbs.

usage: fit_slots.py --build-dir DIR [--region region.inc] [--all]
DIR is a CMake Ninja tree configured for a product that builds the engine. The
exit status is 1 when a group does not fit.
"""

from __future__ import annotations
# Standalone tools must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

GADGETS = Path(__file__).resolve().parents[1]
OBJECT = "darwin/cpu/x86_64/gadgets/CMakeFiles/uml_x86_64_gadgets.dir/engine.S.o"
LINE = 128
HEADROOM = 1024
ROW = re.compile(r"^\s*gadget_slot (\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+)\s*$",
                 re.M)
# region.inc's group macros from `.set Lx86_groups, 0` on, as labels: the first
# and last group's labels delimit the bank and the cold part without slots.
PACKED = """\t.set Lx86_groups, 0
.macro gadget_group name
\t.if Lx86_slot_\\name != Lx86_groups
\t.error "x86-64 gadget group \\name is not the slot table's next row"
\t.endif
\t.set Lx86_groups, Lx86_groups + 1
XLPb_\\name:
\t.pushsection GADGET_SECTION_COLD
XLPc_\\name:
\t.popsection
.endm
.macro gadget_region_end
XLPb_end:
\t.pushsection GADGET_SECTION_COLD
XLPc_end:
\t.popsection
.endm
"""


def compile_command(build_dir: Path, ninja: str) -> list[str]:
    """The Ninja tree's command for engine.S, as an argument list."""
    commands = subprocess.run(
        [ninja, "-C", str(build_dir), "-t", "commands", OBJECT],
        check=True, capture_output=True, text=True).stdout.splitlines()
    arguments = shlex.split(commands[-1])
    result, skip = [], False
    for argument in arguments:
        if skip:
            skip = False
        elif argument in ("-MD",):
            continue
        elif argument in ("-MT", "-MF", "-o", "-c"):
            skip = True
        else:
            result.append(argument)
    return result


def labels(path: Path) -> dict[str, int]:
    """Each label of a Mach-O object, as its offset in its section."""
    data = path.read_bytes()
    commands = struct.unpack_from("<I", data, 16)[0]
    offset, sections, symbols = 32, [], (0, 0, 0)
    for _ in range(commands):
        command, size = struct.unpack_from("<II", data, offset)
        if command == 0x19:  # LC_SEGMENT_64
            count = struct.unpack_from("<I", data, offset + 64)[0]
            for index in range(count):
                address = struct.unpack_from(
                    "<Q", data, offset + 72 + 80 * index + 32)[0]
                sections.append(address)
        elif command == 0x2:  # LC_SYMTAB
            symbols = struct.unpack_from("<III", data, offset + 8)
        offset += size
    symbol_offset, count, string_offset = symbols
    result = {}
    for index in range(count):
        string, _, section, _, value = struct.unpack_from(
            "<IBBHQ", data, symbol_offset + 16 * index)
        end = data.index(b"\0", string_offset + string)
        name = data[string_offset + string:end].decode()
        if section:
            result[name.lstrip("_")] = value - sections[section - 1]
    return result


def fit(arguments: argparse.Namespace) -> list[tuple]:
    text = arguments.region.read_text()
    rows = [(name, *(int(value, 0) for value in values))
            for name, *values in ROW.findall(text)]
    cut = text.index("\t.set Lx86_groups, 0")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        # engine/common.inc reaches ../../layout, and the frame both engines
        # share at ../../../gadget_host_frame.inc, from its own directory.
        gadgets = root / "x86_64/gadgets"
        shutil.copytree(GADGETS, gadgets, ignore=shutil.ignore_patterns(
            "*.cpp", "*.hpp", "*.md", "*.py", "CMakeLists.txt"))
        (root / "x86_64/layout").symlink_to(GADGETS.parent / "layout")
        (root / "gadget_host_frame.inc").symlink_to(
            GADGETS.parents[1] / "gadget_host_frame.inc")
        (gadgets / "engine/region.inc").write_text(text[:cut] + PACKED)
        command = compile_command(arguments.build_dir, arguments.ninja)
        source = str(GADGETS / "engine.S")
        command = [str(gadgets / "engine.S") if part == source else part
                   for part in command]
        packed = root / "engine.o"
        subprocess.run(command + ["-o", str(packed), "-c",
                                  str(gadgets / "engine.S")],
                       check=True, cwd=arguments.build_dir)
        position = labels(packed)
    names = [row[0] for row in rows] + ["end"]
    report = []
    for part, tag, phase_at in (("bank", "XLPb_", 1), ("cold", "XLPc_", 3)):
        for index, row in enumerate(rows):
            phase, slot = row[phase_at], row[phase_at + 1]
            code = position[tag + names[index + 1]] - position[tag + row[0]]
            if not code and not slot:
                continue
            need = -(-(phase + code + HEADROOM) // LINE) * LINE
            report.append((row[0], part, phase, code, slot,
                           slot - phase - code, need))
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--ninja", default="ninja")
    parser.add_argument("--region", type=Path,
                        default=GADGETS / "engine/region.inc")
    parser.add_argument("--all", action="store_true",
                        help="list every group, not only those that do not fit")
    arguments = parser.parse_args()
    report = fit(arguments)
    over = [row for row in report if row[5] < 0]
    shown = report if arguments.all else over
    if shown:
        print(f"{'group':26}{'part':5}{'phase':>6}{'code':>9}{'slot':>9}"
              f"{'room':>8}{'need':>9}")
    for group, part, phase, code, slot, room, need in shown:
        print(f"{group:26}{part:5}{phase:6}{code:9}{slot:9}{room:8}{need:9}")
    if over:
        print(f"{len(over)} slots too small: set each to its `need`")
        return 1
    print(f"every group fits its slot ({len(report)} parts)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
