#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the arm64 gadget region's pinned layout in linked Mach-O images.

darwin/cpu/arm64/gadgets/engine.S emits the region as parts in their own
sections, and UML::GadgetRegionLayout merges them into __TEXT,__text ahead of
everything else. In every image given this checks that:

- __text starts on a 16 KiB image offset and the parts are its first bytes,
  in the order engine.S and gadget_region.order name, each on its boundary
  with its phase pad and directly after the previous part;
- no part holds a zero word between its bounds, so the phase pads are the
  only unreachable bytes in the region; and
- every PC-relative reference in the code parts stays inside the region, so a
  call to a helper outside it goes through its veneer in an island, whose
  words are all branches (the first island, and the AArch32 part's own); and
- SVE and SME words appear only in the SME part, the one part that may run
  in streaming mode; the other parts and the AArch32 parts hold none.

usage: test_gadget_region_layout.py IMAGE...
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
import macho  # noqa: E402
from a64_pc_relative import target  # noqa: E402

HOST_PAGE = 16384
CPU = Path(__file__).resolve().parents[3] / "darwin/cpu"
GADGETS = CPU / "arm64/gadgets"
# The bound symbols of each part, by its gadget_region_start name.
BOUNDS = {
    "bank": "uml_arm64_gadget_bank",
    "forward": "uml_arm64_gadget_forward_section",
    "vector_forward": "uml_arm64_gadget_vector_forward_section",
    "cold": "uml_arm64_gadget_cold_section",
    "afp": "uml_arm64_gadget_afp_section",
    "veneer": "uml_arm64_gadget_veneer_section",
    "sme": "uml_arm64_gadget_sme_section",
    "a32_bank": "uml_arm64_gadget_a32_bank_section",
    "a32_cold": "uml_arm64_gadget_a32_cold_section",
    "a32_veneer": "uml_arm64_gadget_a32_veneer_section",
}
# The islands: parts whose words are all branches. The AArch32 part has its
# own so that it never adds a veneer to the first.
ISLAND = "veneer"
ISLANDS = frozenset({ISLAND, "a32_veneer"})
# The part the SME-only code goes in, after the island so that no base helper
# call changes its distance to its veneer.
STREAMING = "sme"


@dataclass(frozen=True)
class Part:
    name: str
    alignment: int
    phase: int


def engine_parts(engine: Path) -> list[Part]:
    """The parts in emission order, with their alignment and phase."""
    text = engine.read_text()
    defines = {name: int(value) for name, value in re.findall(
        r"^#define (GADGET_PHASE_\w+) (\d+)$", text, re.M)}
    body = text.split(".macro gadget_bank_bounds", 1)[1].split(".endm", 1)[0]
    parts = []
    for name, alignment, phase in re.findall(
            r"gadget_region_start (\w+), (\d+), (\w+)", body):
        parts.append(Part(name, int(alignment),
                          int(phase) if phase.isdigit() else defines[phase]))
    names = [part.name for part in parts]
    if not parts or names[0] != "bank" or ISLAND not in names or \
            STREAMING not in names or set(names) != set(BOUNDS):
        raise ValueError(f"{engine}: cannot read the region parts")
    return parts


def order_parts(order: Path) -> list[str]:
    names = []
    for line in order.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        # The x86-64 region's parts follow; its own guard checks them.
        if line.startswith("_uml_x86_64_"):
            continue
        prefix = "_uml_arm64_gadget_region_"
        if not line.startswith(prefix):
            raise ValueError(f"{order}: unexpected entry {line}")
        names.append(line[len(prefix):])
    return names


class Image(macho.MachO):
    def __init__(self, path: Path):
        super().__init__(path.read_bytes(), str(path))

    def symbol(self, name: str) -> int:
        try:
            return self.symbols["_" + name]
        except KeyError:
            raise ValueError(f"{self.name}: no symbol {name}") from None


def sve_or_sme(word: int) -> bool:
    """Whether a word lies in the SVE (op1 0010) or SME (op0 1, op1 0000)
    encoding space, as cpu/arm64/detail/sme_decode.hpp decides it."""
    return word & 0x1E000000 == 0x04000000 or word & 0x9E000000 == 0x80000000


def check(image: Image, parts: list[Part]) -> list[str]:
    errors = []
    text = image.sections.get(("__TEXT", "__text"))
    base = image.segments.get("__TEXT")
    if text is None or base is None:
        return [f"{image.name}: no __TEXT,__text"]
    if text.alignment < 14 or (text.address - base) % HOST_PAGE:
        errors.append(f"__text is at image offset "
                      f"0x{text.address - base:x} with "
                      f"alignment 2^{text.alignment}, not on a 16 KiB page")
    region = image.symbol("uml_arm64_gadget_region_bank")
    if region != text.address:
        errors.append(f"the region starts at 0x{region:x}, not at the start "
                      f"of __text 0x{text.address:x}")
    previous_end = region
    spans = []
    for part in parts:
        start = image.symbol(f"uml_arm64_gadget_region_{part.name}")
        begin = image.symbol(f"{BOUNDS[part.name]}_begin")
        end = image.symbol(f"{BOUNDS[part.name]}_end")
        boundary = 1 << part.alignment
        if start % boundary:
            errors.append(f"{part.name} starts at 0x{start:x}, off its "
                          f"2^{part.alignment} boundary")
        if not previous_end <= start < previous_end + boundary:
            errors.append(f"{part.name} starts at 0x{start:x}, not directly "
                          f"after the previous part's end 0x{previous_end:x}")
        if begin != start + part.phase:
            errors.append(f"{part.name} entries begin at 0x{begin:x}, not at "
                          f"its start 0x{start:x} plus phase {part.phase}")
        if end < begin or end > text.address + text.size:
            errors.append(f"{part.name} ends at 0x{end:x}, outside __text")
            return errors
        spans.append((part, begin, end))
        previous_end = end
    region_end = previous_end
    for part, begin, end in spans:
        words = image.words(text, begin, end)
        zeros = [begin + 4 * index for index, word in enumerate(words)
                 if word == 0]
        if zeros:
            errors.append(f"{part.name} holds {len(zeros)} zero words, the "
                          f"first at 0x{zeros[0]:x}")
        for index, word in enumerate(words):
            pc = begin + 4 * index
            if part.name in ISLANDS:
                if word & 0xFC000000 != 0x14000000:
                    errors.append(f"island word 0x{word:08x} at 0x{pc:x} is "
                                  f"not a branch")
                continue
            if part.name != STREAMING and sve_or_sme(word):
                errors.append(f"{part.name} word 0x{word:08x} at 0x{pc:x} is "
                              f"an SVE or SME instruction outside the "
                              f"{STREAMING} part")
            destination = target(word, pc)
            if destination is not None and not region <= destination < region_end:
                errors.append(f"{part.name} word 0x{word:08x} at 0x{pc:x} "
                              f"refers to 0x{destination:x} outside the "
                              f"region")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--engine", type=Path, default=GADGETS / "engine.S")
    parser.add_argument("--order", type=Path,
                        default=CPU / "gadget_region.order")
    parser.add_argument("images", type=Path, nargs="+")
    arguments = parser.parse_args()
    parts = engine_parts(arguments.engine)
    order = order_parts(arguments.order)
    failed = False
    if order != [part.name for part in parts]:
        print(f"{arguments.order} names {order}, but engine.S emits "
              f"{[part.name for part in parts]}", file=sys.stderr)
        failed = True
    for path in arguments.images:
        try:
            image = Image(path)
            errors = check(image, parts)
        except ValueError as error:
            print(error, file=sys.stderr)
            failed = True
            continue
        for error in errors[:20]:
            print(f"{path}: {error}", file=sys.stderr)
        if len(errors) > 20:
            print(f"{path}: {len(errors) - 20} more errors", file=sys.stderr)
        if errors:
            failed = True
            continue
        region = image.symbol("uml_arm64_gadget_region_bank")
        end = image.symbol(f"{BOUNDS[parts[-1].name]}_end")
        offset = region - image.segments["__TEXT"]
        print(f"{path}: region at image offset 0x{offset:x}, "
              f"{end - region} bytes, "
              f"phases {[part.phase for part in parts]}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
