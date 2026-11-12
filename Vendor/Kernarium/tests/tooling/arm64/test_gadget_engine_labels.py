#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check that the gadget engines' objects name no `.L` symbol.

Both engines name their private labels with the Mach-O assembler-temporary
prefix `L`, which keeps them out of the symbol table. On Mach-O a `.L` label is
an ordinary local symbol, so one would put a symbol back in the object and an
`__unwind_info` entry in every image that links it. This reads the symbol table
of each engine object given and fails on any name that starts with `.L`.

usage: test_gadget_engine_labels.py TARGET OBJECT [TARGET OBJECT ...]
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "support"))
import macho  # noqa: E402

SHOWN = 20


def symbol_names(path: Path) -> list[str]:
    """Every non-debug symbol name in a thin 64-bit Mach-O object."""
    image = macho.MachO(path.read_bytes(), str(path))
    if image.filetype != macho.MH_OBJECT:
        raise ValueError(f"{path}: not a thin 64-bit Mach-O object")
    if image.symbol_names is None:
        raise ValueError(f"{path}: no symbol table")
    return image.symbol_names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pairs", nargs="+", metavar="TARGET OBJECT")
    arguments = parser.parse_args()
    if len(arguments.pairs) % 2:
        parser.error("expected TARGET OBJECT pairs")
    failed = False
    for target, name in zip(arguments.pairs[::2], arguments.pairs[1::2]):
        path = Path(name)
        if not path.is_file():
            print(f"{target}: {path} does not exist", file=sys.stderr)
            failed = True
            continue
        try:
            names = symbol_names(path)
        except ValueError as error:
            print(f"{target}: {error}", file=sys.stderr)
            failed = True
            continue
        if not names:
            print(f"{target}: {path} has an empty symbol table",
                  file=sys.stderr)
            failed = True
            continue
        dotted = sorted(n for n in names if n.startswith(".L"))
        if dotted:
            plural = "" if len(dotted) == 1 else "s"
            print(f"{target}: {path} names {len(dotted)} .L symbol{plural}; "
                  "write private labels with the L prefix:", file=sys.stderr)
            for symbol in dotted[:SHOWN]:
                print(f"  {symbol}", file=sys.stderr)
            if len(dotted) > SHOWN:
                print(f"  ... and {len(dotted) - SHOWN} more", file=sys.stderr)
            failed = True
            continue
        print(f"{target}: {path.name}: {len(names)} symbols, none named .L")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
