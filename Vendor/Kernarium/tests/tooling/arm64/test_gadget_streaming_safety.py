#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that no AdvSIMD instruction runs in streaming mode in linked images.

A program the arm64 gadget engine translates for SVCR.SM = 1 runs with the
host in streaming mode, where an AdvSIMD instruction traps, and may select
only the entries darwin/cpu/arm64/gadgets/engine/sme.inc lists: base rows
whose gadgets are safe there, the SME part's streaming half, and the
generated register-only and vector memory gadgets. In every image given
this walks all code reachable from each of those entries, and from the
streaming entry point, tracking whether the host is in streaming mode
(SMSTART and SMSTOP, and the suspend and resume routines that leave and
re-enter it around a helper), and fails where code reached in streaming
mode

- executes an AdvSIMD word, as the shared SVE/SME encoding table classifies
  FP&SIMD words (cpu/arm64/detail/sme_encodings.hpp);
- calls a helper outside the region, through its veneer, or branches to a
  register other than a return or a gadget dispatch; or
- leaves the region up to the SME part's end, so reaches the SVE part,
  which follows it and runs only outside streaming mode, or reaches a zero
  word.

usage: test_gadget_streaming_safety.py IMAGE...
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SME_PART = ROOT / "darwin/cpu/arm64/gadgets/engine/sme.inc"


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


layout = load("gadget_region_layout", Path(__file__).with_name(
    "test_gadget_region_layout.py"))
encodings = load("sme_encoding_table",
                 ROOT / "tools/arm64-tests/sme_encoding_table.py")

PREFIX = "uml_arm64_gadget_"
SMSTART_SM = {0xD503437F, 0xD503477F}  # SMSTART SM, SMSTART
SMSTOP_SM = {0xD503427F, 0xD503467F}   # SMSTOP SM, SMSTOP
ZERO_STRIDE = 16
GADGET_ALIGNMENT = 16


def streaming_rows(source: Path) -> list[tuple[str, str | None]]:
    """The rows sme.inc lists, as (begin, end) names; end is None for a
    one-entry row. Rows inside #if blocks are listed too, and a row whose
    symbol an image lacks is skipped there."""
    body = source.read_text().split("_streaming_rows_begin):", 1)[1]
    body = body.split("_streaming_rows_end):", 1)[0]
    rows: list[tuple[str, str | None]] = []
    for begin, end in re.findall(
            r"^\s*streaming_row (\w+), (\w+)$", body, re.M):
        rows.append((begin, end))
    for name in re.findall(r"^\s*streaming_entry (\w+)$", body, re.M):
        rows.append((name, None))
    if len(rows) < 2:
        raise ValueError(f"{source}: cannot read the streaming rows")
    return rows


class Walker:
    def __init__(self, image, path: Path):
        self.image = image
        self.path = path
        self.text = image.sections[("__TEXT", "__text")]
        self.region = (image.symbol("uml_arm64_gadget_region_bank"),
                       image.symbol("uml_arm64_gadget_sme_section_end"))
        self.veneers = (image.symbol("uml_arm64_gadget_veneer_section_begin"),
                        image.symbol("uml_arm64_gadget_veneer_section_end"))
        self.suspend = image.symbols["uml_arm64_gadget_sme_suspend"]
        self.resume = image.symbols["uml_arm64_gadget_sme_resume"]
        table = encodings.load()
        self.fp_simd = [(row.mask, row.value, row.advsimd)
                        for row in table.fp_simd]
        self.names: dict[int, str] = {}
        for name, value in image.symbols.items():
            self.names.setdefault(value, name)
        self.seen: set[tuple[int, bool]] = set()

    def word(self, address: int) -> int:
        return self.image.words(self.text, address, address + 4)[0]

    def advsimd(self, word: int) -> bool:
        for mask, value, advsimd in self.fp_simd:
            if word & mask == value:
                return advsimd
        return False

    def where(self, address: int) -> str:
        best = max((value for value in self.names
                    if value <= address), default=None)
        if best is None:
            return f"0x{address:x}"
        return f"{self.names[best]}+0x{address - best:x}"

    def walk(self, entry: int, streaming: bool) -> list[str]:
        """Every error on a path from `entry`, entered with the host in
        streaming mode or not."""
        errors = []
        work = [(entry, streaming)]
        while work:
            pc, state = work.pop()
            while True:
                if (pc, state) in self.seen:
                    break
                self.seen.add((pc, state))
                if not self.region[0] <= pc < self.region[1]:
                    errors.append(f"{self.where(pc)}: leaves the region")
                    break
                word = self.word(pc)
                if word == 0:
                    errors.append(f"{self.where(pc)}: zero word")
                    break
                if word in SMSTART_SM:
                    state = True
                elif word in SMSTOP_SM:
                    state = False
                elif state and self.advsimd(word):
                    errors.append(f"{self.where(pc)}: AdvSIMD word "
                                  f"{word:08x} in streaming mode")
                if word & 0xFC000000 == 0x94000000:  # BL
                    destination = layout.target(word, pc)
                    if destination == self.suspend:
                        state = False
                    elif destination == self.resume:
                        state = True
                    elif self.veneers[0] <= destination < self.veneers[1]:
                        if state:
                            errors.append(f"{self.where(pc)}: helper call "
                                          "in streaming mode")
                    else:
                        # A routine of the region returns in the mode it
                        # was called in.
                        work.append((destination, state))
                    pc += 4
                    continue
                if word & 0xFC000000 == 0x14000000:  # B
                    pc = layout.target(word, pc)
                    continue
                if word & 0xFF000010 == 0x54000000 or \
                        word & 0x7E000000 in (0x34000000, 0x36000000):
                    work.append((layout.target(word, pc), state))
                    pc += 4
                    continue
                if word & 0xFFFFFC1F in (0xD61F0000, 0xD65F0000):  # BR, RET
                    break
                if word & 0xFFFFFC1F == 0xD63F0000:  # BLR
                    errors.append(f"{self.where(pc)}: indirect call")
                    break
                pc += 4
        return errors


def entries(image, rows: list[tuple[str, str | None]]) -> list[int]:
    """Every entry a streaming program may select."""
    symbols = sorted({value for name, value in image.symbols.items()
                      if name.startswith("_" + PREFIX)})
    found = []
    for begin_name, end_name in rows:
        if "_" + PREFIX + begin_name not in image.symbols:
            continue
        begin = image.symbol(PREFIX + begin_name)
        end = image.symbol(PREFIX + end_name) if end_name else begin + 4
        found.extend(value for value in symbols if begin <= value < end)
    table = image.symbol(PREFIX + "sm_za_zero_table")
    table_end = image.symbol(PREFIX + "sm_za_zero_table_end")
    if (table_end - table) != 256 * ZERO_STRIDE:
        raise ValueError("the ZERO { mask } table is not 256 entries of "
                         f"{ZERO_STRIDE} bytes")
    found.extend(range(table, table_end, ZERO_STRIDE))
    # The generated gadgets carry no symbols: every 16-byte boundary of
    # their range, which includes every entry.
    generated = image.symbol(PREFIX + "sm_generated_begin")
    generated_end = image.symbol(PREFIX + "sm_generated_end")
    if generated_end <= generated:
        raise ValueError("the generated streaming gadgets are empty")
    found.extend(range(generated, generated_end, GADGET_ALIGNMENT))
    return sorted(set(found))


def check(path: Path, rows: list[tuple[str, str | None]]) -> list[str]:
    image = layout.Image(path)
    walker = Walker(image, path)
    selected = entries(image, rows)
    errors = []
    for entry in selected:
        errors.extend(walker.walk(entry, True))
    errors.extend(walker.walk(
        image.symbol(PREFIX + "enter_streaming"), False))
    # The two routines themselves: nothing they run in streaming mode is
    # AdvSIMD, whichever mode they start in.
    for routine in (walker.suspend, walker.resume):
        errors.extend(walker.walk(routine, True))
        errors.extend(walker.walk(routine, False))
    print(f"{path.name}: {len(selected)} streaming entries, "
          f"{len(walker.seen)} instruction states walked")
    return sorted(set(errors))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("images", nargs="+", type=Path)
    arguments = parser.parse_args()
    rows = streaming_rows(SME_PART)
    failed = False
    for path in arguments.images:
        errors = check(path, rows)
        for error in errors[:40]:
            print(f"{path}: {error}", file=sys.stderr)
        if len(errors) > 40:
            print(f"{path}: {len(errors) - 40} more", file=sys.stderr)
        failed = failed or bool(errors)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
