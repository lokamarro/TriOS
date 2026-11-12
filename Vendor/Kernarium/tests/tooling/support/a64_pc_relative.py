# SPDX-License-Identifier: GPL-2.0-or-later
"""Where an A64 instruction's PC-relative operand points, for the tests that
walk the gadget regions' code."""

from __future__ import annotations


def sign(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def target(word: int, pc: int) -> int | None:
    """The address a PC-relative instruction refers to, else None."""
    if word & 0x7C000000 == 0x14000000:  # B, BL
        return pc + 4 * sign(word & 0x3FFFFFF, 26)
    if word & 0xFF000000 == 0x54000000:  # B.cond, BC.cond
        return pc + 4 * sign((word >> 5) & 0x7FFFF, 19)
    if word & 0x7E000000 in (0x34000000, 0x36000000):  # CBZ/CBNZ, TBZ/TBNZ
        if word & 0x02000000:
            return pc + 4 * sign((word >> 5) & 0x3FFF, 14)
        return pc + 4 * sign((word >> 5) & 0x7FFFF, 19)
    if word & 0x1F000000 == 0x10000000:  # ADR, ADRP
        immediate = sign(((word >> 5) & 0x7FFFF) << 2 | (word >> 29) & 3, 21)
        if word & 0x80000000:
            return (pc & ~0xFFF) + (immediate << 12)
        return pc + immediate
    if word & 0x3B000000 == 0x18000000:  # LDR, LDRSW, PRFM (literal)
        return pc + 4 * sign((word >> 5) & 0x7FFFF, 19)
    return None
