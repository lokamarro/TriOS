#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the ELF32 wrapper of the AArch32 guest test programs.

Assembles a small ARM EABI hard-float object with the given compiler, wraps it
in each layout, and checks the executable the guest kernel's loader sees: the
header, one read-execute PT_LOAD whose offset and address are congruent modulo
64 KiB (64k) or only modulo 4 KiB (4k), a non-executable PT_GNU_STACK, and the
bl of the object resolved to its target.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile

PROGRAM = """
	.text
	.globl	_start
	.type	_start, %function
_start:
	bl	helper
	mov	r7, #1
	svc	#0
	.type	helper, %function
helper:
	bx	lr
"""


def run(arguments, **keywords):
    return subprocess.run(arguments, check=True, stdin=subprocess.DEVNULL,
                          **keywords)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--wrapper", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    failures = []

    def check(name, passed, detail=""):
        if not passed:
            failures.append(f"{name}: {detail}")

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        source = root / "program.S"
        source.write_text(PROGRAM)
        obj = root / "program.o"
        run([arguments.compiler, "--target=armv7-unknown-linux-gnueabihf",
             "-mfpu=neon-vfpv4", "-mfloat-abi=hard", "-c", str(source),
             "-o", str(obj)])

        for layout, base, align in (("64k", 0x10000, 0x10000),
                                    ("4k", 0x11000, 0x1000)):
            output = root / f"program-{layout}"
            run([sys.executable, "-I", str(arguments.wrapper),
                 *(["--layout", layout] if layout != "64k" else []),
                 str(obj), str(output)])
            data = output.read_bytes()
            (ident, e_type, machine, _, entry, phoff, _, flags, ehsize,
             phentsize, phnum) = struct.unpack_from("<16sHHIIIIIHHH", data, 0)
            check(f"{layout}: ELF32 little endian",
                  ident[:6] == b"\x7fELF\x01\x01", ident.hex())
            check(f"{layout}: executable for ARM", (e_type, machine) == (2, 40),
                  f"{e_type} {machine}")
            check(f"{layout}: EABI5 hard-float", flags == 0x05000400,
                  hex(flags))
            programs = [struct.unpack_from("<8I", data, phoff + 32 * index)
                        for index in range(phnum)]
            loads = [p for p in programs if p[0] == 1]
            check(f"{layout}: one PT_LOAD", len(loads) == 1, str(programs))
            if len(loads) == 1:
                _, offset, vaddr, _, filesz, memsz, pflags, palign = loads[0]
                check(f"{layout}: segment at {base:#x}",
                      (vaddr, palign) == (base, align),
                      f"{vaddr:#x} {palign:#x}")
                check(f"{layout}: read-execute", pflags == 5, hex(pflags))
                check(f"{layout}: whole file mapped",
                      offset == 0 and filesz == len(data) and memsz >= filesz)
                congruent = (vaddr - offset) % 0x10000 == 0
                check(f"{layout}: 64 KiB congruence is {layout == '64k'}",
                      congruent == (layout == "64k"))
                check(f"{layout}: entry is in the segment",
                      vaddr <= entry < vaddr + filesz, hex(entry))
                # bl helper at the entry: A32 BL, branch offset in words
                # relative to pc + 8; helper follows the svc at +12.
                word = struct.unpack_from("<I", data, entry - vaddr + offset)[0]
                delta = (word & 0xffffff)
                check(f"{layout}: the bl is resolved",
                      word >> 24 == 0xeb and delta == 1, hex(word))
            stacks = [p for p in programs if p[0] == 0x6474e551]
            check(f"{layout}: non-executable stack",
                  len(stacks) == 1 and stacks[0][6] == 6, str(stacks))

        bad = subprocess.run([sys.executable, "-I", str(arguments.wrapper),
                              "--layout", "16k", str(obj), str(root / "bad")],
                             stdin=subprocess.DEVNULL, capture_output=True)
        check("an unknown layout is refused", bad.returncode != 0
              and not (root / "bad").exists())

    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
