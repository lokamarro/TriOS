#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the ELF32 linker of the AArch32 engine probe.

Assembles two small ARM EABI hard-float objects with the given compiler and
links them in each layout. Checks the executable the guest kernel's loader
sees (a read-execute PT_LOAD, a read-write one congruent to its file offset,
a non-executable PT_GNU_STACK, the entry at _start) and every relocation the
linker applies: A32 and T32 calls between the objects, with the BL of one
instruction set becoming a BLX to the other; MOVW/MOVT pairs in both sets;
ABS32 data, with bit 0 set for a Thumb function; and a bss symbol. A symbol
no object defines, one two objects define and a relocation the linker does
not know each fail, naming the culprit.
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

FIRST = """
	.syntax	unified
	.text
	.arm
	.globl	_start
	.type	_start, %function
_start:
	bl	arm_helper
	bl	thumb_helper
	movw	r0, #:lower16:table
	movt	r0, #:upper16:table
	mov	r7, #1
	svc	#0
	.data
	.balign	4
	.globl	table
table:
	.word	arm_helper
	.word	thumb_helper
	.word	0x12345678
	.bss
	.balign	16
	.globl	buffer
buffer:
	.space	64
"""

SECOND = """
	.syntax	unified
	.text
	.arm
	.globl	arm_helper
	.type	arm_helper, %function
arm_helper:
	bx	lr
	.thumb
	.thumb_func
	.globl	thumb_helper
	.type	thumb_helper, %function
thumb_helper:
	bl	arm_helper
	movw	r1, #:lower16:buffer
	movt	r1, #:upper16:buffer
	bx	lr
"""

UNDEFINED = """
	.syntax	unified
	.text
	.arm
	.globl	_start
_start:
	bl	nowhere
"""

DUPLICATE = """
	.syntax	unified
	.text
	.arm
	.globl	arm_helper
arm_helper:
	bx	lr
"""

UNSUPPORTED = """
	.syntax	unified
	.text
	.arm
	.globl	_start
_start:
	adr	r0, _start
	.globl	elsewhere
elsewhere:
	adr	r1, other
	.text
	.globl	other
other:
	bx	lr
"""


def run(arguments, **keywords):
    return subprocess.run(arguments, check=True, stdin=subprocess.DEVNULL,
                          **keywords)


def decode_arm_branch(word, address):
    """(target, is_blx) of an A32 BL or BLX immediate at `address`."""
    offset = (word & 0xffffff) << 2
    if offset & 0x2000000:
        offset -= 0x4000000
    if word >> 24 == 0xfb or word >> 24 == 0xfa:
        offset += (word >> 24 & 1) * 2
        return address + 8 + offset, True
    return address + 8 + offset, False


def decode_thumb_bl(first, second, address):
    """(target, is_blx) of a T32 BL or BLX at `address`."""
    sign = first >> 10 & 1
    i1 = (~(second >> 13 ^ sign)) & 1
    i2 = (~(second >> 11 ^ sign)) & 1
    offset = (sign << 24 | i1 << 23 | i2 << 22 | (first & 0x3ff) << 12 |
              (second & 0x7ff) << 1)
    if sign:
        offset -= 1 << 25
    blx = (second >> 12 & 1) == 0
    base = (address + 4) & ~3 if blx else address + 4
    return base + offset, blx


def decode_movw_movt_arm(low, high):
    def immediate(word):
        return (word >> 4 & 0xf000) | (word & 0xfff)
    return immediate(high) << 16 | immediate(low)


def decode_movw_movt_thumb(low, high):
    def immediate(first, second):
        return ((first & 0xf) << 12 | (first >> 10 & 1) << 11 |
                (second >> 12 & 7) << 8 | (second & 0xff))
    return (immediate(*high) << 16) | immediate(*low)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--linker", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    failures = []

    def check(name, passed, detail=""):
        if not passed:
            failures.append(f"{name}: {detail}")

    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)

        def assemble(name, text):
            source = root / f"{name}.S"
            source.write_text(text)
            obj = root / f"{name}.o"
            run([arguments.compiler, "--target=armv7-unknown-linux-gnueabihf",
                 "-mfpu=neon-vfpv4", "-mfloat-abi=hard", "-c", str(source),
                 "-o", str(obj)])
            return obj

        first = assemble("first", FIRST)
        second = assemble("second", SECOND)

        def link(output, *objects, layout="64k"):
            return subprocess.run(
                [sys.executable, "-I", str(arguments.linker),
                 *(["--layout", layout] if layout != "64k" else []),
                 str(output), *map(str, objects)],
                stdin=subprocess.DEVNULL, capture_output=True, text=True)

        for layout, base, align in (("64k", 0x10000, 0x10000),
                                    ("4k", 0x11000, 0x1000)):
            output = root / f"program-{layout}"
            result = link(output, first, second, layout=layout)
            check(f"{layout}: the link succeeds", result.returncode == 0,
                  result.stderr)
            if result.returncode != 0:
                continue
            data = output.read_bytes()
            (ident, e_type, machine, _, entry, phoff, _, flags, _,
             _, phnum) = struct.unpack_from("<16sHHIIIIIHHH", data, 0)
            check(f"{layout}: ELF32 little endian executable for ARM",
                  ident[:6] == b"\x7fELF\x01\x01" and (e_type, machine) == (2, 40))
            check(f"{layout}: EABI5 hard-float", flags == 0x05000400, hex(flags))
            programs = [struct.unpack_from("<8I", data, phoff + 32 * index)
                        for index in range(phnum)]
            loads = [p for p in programs if p[0] == 1]
            check(f"{layout}: two PT_LOAD segments", len(loads) == 2, str(programs))
            if len(loads) != 2:
                continue
            rx, rw = loads
            check(f"{layout}: code at {base:#x}, read-execute",
                  (rx[2], rx[6], rx[7]) == (base, 5, align),
                  f"{rx[2]:#x} {rx[6]:#x} {rx[7]:#x}")
            check(f"{layout}: data is read-write, congruent to its offset",
                  rw[6] == 6 and (rw[2] - rw[1]) % rw[7] == 0
                  and rw[7] == align, str(rw))
            check(f"{layout}: bss follows the data in memory only",
                  rw[5] > rw[4], f"filesz {rw[4]} memsz {rw[5]}")
            stacks = [p for p in programs if p[0] == 0x6474e551]
            check(f"{layout}: non-executable stack",
                  len(stacks) == 1 and stacks[0][6] == 6, str(stacks))
            check(f"{layout}: entry is _start, the first code",
                  base < entry < base + rx[4], hex(entry))

            def word_at(address):
                return struct.unpack_from("<I", data, address - rx[2] + rx[1])[0]

            def half_at(address):
                return struct.unpack_from("<H", data, address - rx[2] + rx[1])[0]

            arm_target, arm_blx = decode_arm_branch(word_at(entry), entry)
            check(f"{layout}: A32 bl reaches the ARM function in the other object",
                  not arm_blx and base <= arm_target < base + rx[4],
                  f"{arm_target:#x}")
            thumb_target, thumb_blx = decode_arm_branch(word_at(entry + 4), entry + 4)
            check(f"{layout}: A32 bl to a Thumb function becomes blx",
                  thumb_blx and base <= thumb_target < base + rx[4]
                  and (thumb_target & 1) == 0, f"{thumb_target:#x}")
            table = decode_movw_movt_arm(word_at(entry + 8), word_at(entry + 12))
            check(f"{layout}: movw/movt name the data symbol", table == rw[2],
                  f"{table:#x} vs {rw[2]:#x}")
            words = struct.unpack_from("<3I", data, rw[1])
            check(f"{layout}: ABS32 data holds the function addresses, Thumb with bit 0",
                  words[0] == arm_target and words[1] == thumb_target | 1
                  and words[2] == 0x12345678, str([hex(w) for w in words]))
            # thumb_helper: bl arm_helper (a blx), then movw/movt buffer.
            blx_target, blx_flag = decode_thumb_bl(
                half_at(thumb_target), half_at(thumb_target + 2), thumb_target)
            check(f"{layout}: T32 bl to an ARM function becomes blx to it",
                  blx_flag and blx_target == arm_target, f"{blx_target:#x}")
            buffer = decode_movw_movt_thumb(
                (half_at(thumb_target + 4), half_at(thumb_target + 6)),
                (half_at(thumb_target + 8), half_at(thumb_target + 10)))
            check(f"{layout}: T32 movw/movt name the bss symbol",
                  buffer == rw[2] + 16, f"{buffer:#x} vs {rw[2] + 16:#x}")

        undefined = link(root / "undefined", assemble("undefined", UNDEFINED))
        check("an undefined symbol is refused, by name",
              undefined.returncode != 0 and "nowhere" in undefined.stderr,
              undefined.stderr)
        check("and no executable is left", not (root / "undefined").exists())
        duplicate = link(root / "duplicate", first, second,
                         assemble("duplicate", DUPLICATE))
        check("a global defined twice is refused, by name",
              duplicate.returncode != 0 and "arm_helper" in duplicate.stderr,
              duplicate.stderr)
        unsupported = link(root / "unsupported",
                           assemble("unsupported", UNSUPPORTED))
        check("a relocation the linker does not know is refused",
              unsupported.returncode != 0 and "relocation" in unsupported.stderr,
              unsupported.stderr)

    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
