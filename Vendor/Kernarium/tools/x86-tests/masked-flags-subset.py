#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Select the checked-in subset of XE2's masked-flag request sets.

XE2 recorded an Intel Xeon's MXCSR after one SIMD floating-point instruction
per case, with every exception masked, over six request sets (`100`, `x`,
`fma`, `th`, `cmp`, `tiny`): `req-SET.bin` holds the native probe's
requests, `nat-SET.bin` the Xeon's responses and `names-SET.tsv` one row per
request program. The subset keeps, for every instruction name, DAZ and FTZ
setting, the first case of each distinct combination of the Xeon's flags and
the flags each engine's host instructions raise (`--raw`, responses in the
same format from probes that fold the host's flags unchanged), so that every
way an engine can diverge from the Xeon is represented. The Xeon's flags are
the expectation; the raw responses only choose cases.

Two documented inexact results are marked per case from the Xeon's own
values, so the test can tell them from a regression: bit 6 of the flags
byte marks a result that x86 rounds up to the smallest normal from below,
where the host detects tininess before rounding and raises underflow (a
product, quotient, FMA or narrowing whose computed lane holds the smallest
normal, or a dot product with such a product); bit 7 marks an FMA whose
computed lane is a NaN with no invalid operation, which an infinity times
zero plus a quiet NaN gives.

Output (little-endian): magic "MXF1", the instruction count, each
instruction as name length, name, encoding length, encoding and the bytes
kept per input register (16, or 32 for a 256-bit form), then the case count
and each case as instruction index (u16), MXCSR (u16), the flags byte (u8)
and the three input registers' kept bytes.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import fractions
import hashlib
import json
import pathlib
import struct

SETS = ("100", "x", "fma", "th", "cmp", "tiny")
REQUEST_MAGIC = 0x31504558
OUTPUT_MAGIC = b"MXF1"
CASE_INPUT = 96
RESPONSE = 56
RESPONSE_MXCSR = 32


def vex_l(encoding: bytes) -> int:
    if encoding[0] == 0xC5:
        return (encoding[1] >> 2) & 1
    if encoding[0] == 0xC4:
        return (encoding[2] >> 2) & 1
    return 0


def flags(response: bytes, case: int) -> int:
    return struct.unpack_from("<I", response, case * RESPONSE + RESPONSE_MXCSR)[0] & 0x3F


# Smallest normal and significand bits of half, single and double.
FORMATS = {16: (0x0400, 11, -14), 32: (0x00800000, 24, -126),
           64: (0x0010000000000000, 53, -1022)}
SCALAR_FMA = {0x99, 0x9B, 0x9D, 0x9F, 0xA9, 0xAB, 0xAD, 0xAF,
              0xB9, 0xBB, 0xBD, 0xBF}


def decode(encoding: bytes) -> dict:
    """Map, opcode, operand registers and the scalar and double choices of
    one register-form instruction."""
    index = 0
    legacy = set()
    while encoding[index] in (0x66, 0xF2, 0xF3):
        legacy.add(encoding[index])
        index += 1
    rex = 0
    if 0x40 <= encoding[index] <= 0x4F:
        rex = encoding[index]
        index += 1
    vex = encoding[index] in (0xC4, 0xC5)
    if encoding[index] == 0xC5:
        byte = encoding[index + 1]
        space, wide, vvvv, prefix = 1, 0, (~byte >> 3) & 0xF, byte & 3
        index += 2
    elif encoding[index] == 0xC4:
        byte = encoding[index + 2]
        space, wide = encoding[index + 1] & 0x1F, byte >> 7
        vvvv, prefix = (~byte >> 3) & 0xF, byte & 3
        index += 3
    else:
        index += 1  # 0x0f
        space = 1
        if encoding[index] in (0x38, 0x3A):
            space = 2 if encoding[index] == 0x38 else 3
            index += 1
        wide, vvvv = (rex >> 3) & 1, None
        prefix = 2 if 0xF3 in legacy else 3 if 0xF2 in legacy else 1 if 0x66 in legacy else 0
    opcode = encoding[index]
    modrm = encoding[index + 1]
    reg, rm = (modrm >> 3) & 7, modrm & 7
    scalar = (space == 1 and prefix in (2, 3) and
              (0x51 <= opcode <= 0x5F or opcode == 0xC2)) or \
        (space == 1 and opcode in (0x2E, 0x2F)) or \
        (space == 2 and opcode in SCALAR_FMA)
    sources = (vvvv, rm) if vex else (reg, rm)
    return {"space": space, "opcode": opcode, "prefix": prefix, "wide": wide,
            "scalar": scalar, "sources": sources, "immediate": encoding[-1]}


def result_bits(name: str, decoded: dict, double: bool) -> int:
    if "cvtps2ph" in name:
        return 16
    if decoded["space"] == 1 and decoded["opcode"] == 0x5A:
        return 32 if decoded["prefix"] in (1, 3) else 64
    return 64 if double else 32


def lanes(data: bytes, bits: int) -> list[int]:
    width = bits // 8
    return [int.from_bytes(data[index:index + width], "little")
            for index in range(0, len(data), width)]


def value(bits: int, element: int) -> fractions.Fraction:
    smallest, precision, minimum = FORMATS[element]
    exponent_bits = {16: 5, 32: 8, 64: 11}[element]
    fraction_bits = precision - 1
    biased = (bits >> fraction_bits) & ((1 << exponent_bits) - 1)
    significand = bits & ((1 << fraction_bits) - 1)
    if biased == 0:
        magnitude = fractions.Fraction(significand, 1 << fraction_bits) * \
            fractions.Fraction(2) ** minimum
    else:
        magnitude = fractions.Fraction((1 << fraction_bits) | significand,
                                       1 << fraction_bits) * \
            fractions.Fraction(2) ** (biased - (1 << (exponent_bits - 1)) + 1)
    return -magnitude if bits >> (element - 1) else magnitude


def rounds_up_to_normal(exact: fractions.Fraction, element: int,
                        rounding: int) -> bool:
    """A tiny exact result that rounds, with an unbounded exponent, to the
    smallest normal: not tiny for x86, tiny for the host."""
    _smallest, precision, minimum = FORMATS[element]
    threshold = fractions.Fraction(2) ** minimum
    magnitude = abs(exact)
    if magnitude == 0 or magnitude >= threshold:
        return False
    exponent = minimum - 1
    while magnitude < fractions.Fraction(2) ** exponent:
        exponent -= 1
    unit = fractions.Fraction(2) ** (exponent - precision + 1)
    steps, remainder = divmod(magnitude, unit)
    negative = exact < 0
    if remainder:
        if rounding == 0:
            steps += remainder * 2 > unit or (remainder * 2 == unit and steps % 2)
        elif rounding == 3:
            pass
        elif (rounding == 2) != negative:
            steps += 1
    return steps * unit >= threshold


def is_nan(bits: int, element: int) -> bool:
    _smallest, precision, _minimum = FORMATS[element]
    exponent_bits = {16: 5, 32: 8, 64: 11}[element]
    fraction_bits = precision - 1
    exponent = (bits >> fraction_bits) & ((1 << exponent_bits) - 1)
    return exponent == (1 << exponent_bits) - 1 and bits & ((1 << fraction_bits) - 1) != 0


def candidate_bits(name: str, decoded: dict, double: bool, mxcsr: int,
                   inputs: bytes, result: bytes, expected: int) -> int:
    """Bits 6 and 7 of the flags byte, from the Xeon's result."""
    element = result_bits(name, decoded, double)
    fma = decoded["space"] == 2 and 0x96 <= decoded["opcode"] <= 0xBF
    dot = decoded["space"] == 3 and decoded["opcode"] in (0x40, 0x41)
    computed = result[:element // 8] if decoded["scalar"] else result
    marks = 0
    if dot:
        element = 64 if decoded["opcode"] == 0x41 else 32
        count = 2 if element == 64 else 4
        selected = decoded["immediate"] >> 4
        left, right = decoded["sources"]
        rounding = (mxcsr >> 13) & 3
        for half in range(len(result) // 16):
            for lane in range(count):
                if not selected >> lane & 1:
                    continue
                offset = half * 16 + lane * (element // 8)
                a = int.from_bytes(inputs[left * 32 + offset:left * 32 + offset +
                                          element // 8], "little")
                b = int.from_bytes(inputs[right * 32 + offset:right * 32 + offset +
                                          element // 8], "little")
                if is_nan(a, element) or is_nan(b, element):
                    continue
                exponent_mask = {32: 0x7F800000, 64: 0x7FF0000000000000}[element]
                if a & exponent_mask == exponent_mask or b & exponent_mask == exponent_mask:
                    continue
                if mxcsr & 0x40 and (a & exponent_mask == 0 or b & exponent_mask == 0):
                    continue
                if rounds_up_to_normal(value(a, element) * value(b, element),
                                       element, rounding):
                    marks |= 0x40
    elif decoded["space"] == 3 and decoded["opcode"] == 0x1D or fma or \
            (decoded["space"] == 1 and decoded["opcode"] in (0x59, 0x5E, 0x5A)):
        smallest = FORMATS[element][0]
        if any(lane & ~(1 << (element - 1)) == smallest
               for lane in lanes(computed, element)):
            marks |= 0x40
    if fma and not expected & 1 and any(is_nan(lane, element)
                                        for lane in lanes(computed, element)):
        marks |= 0x80
    return marks


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xe2", required=True, type=pathlib.Path,
                        help="directory with req-, nat- and names- files")
    parser.add_argument("--raw", required=True, action="append", type=pathlib.Path,
                        help="directory with one engine's raw responses, "
                        "PREFIX-SET.bin; give one per engine")
    parser.add_argument("--raw-prefix", required=True, action="append",
                        help="file prefix inside the matching --raw directory")
    parser.add_argument("--all", action="store_true",
                        help="keep every case, for a run of the whole sets")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--provenance", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    if len(arguments.raw) != len(arguments.raw_prefix):
        parser.error("give one --raw-prefix per --raw")

    instructions: dict[tuple[str, bytes], int] = {}
    instruction_rows: list[bytes] = []
    cases: list[bytes] = []
    seen: set[tuple] = set()
    hashes: dict[str, str] = {}
    for name_set in SETS:
        request_path = arguments.xe2 / f"req-{name_set}.bin"
        native_path = arguments.xe2 / f"nat-{name_set}.bin"
        names_path = arguments.xe2 / f"names-{name_set}.tsv"
        request = request_path.read_bytes()
        native = native_path.read_bytes()
        raw = [(directory / f"{prefix}-{name_set}.bin").read_bytes()
               for directory, prefix in zip(arguments.raw, arguments.raw_prefix)]
        for path, data in ((request_path, request), (native_path, native),
                           (names_path, names_path.read_bytes())):
            hashes[path.name] = hashlib.sha256(data).hexdigest()
        rows = [line.split("\t") for line in names_path.read_text().splitlines()]
        magic, programs = struct.unpack_from("<II", request, 0)
        if magic != REQUEST_MAGIC or programs != len(rows):
            raise SystemExit(f"{request_path}: not a request for {names_path}")
        offset = 8
        case = 0
        for row in rows:
            name, _shape, double, daz, ftz, _rounding, count = row
            length = request[offset]
            encoding = request[offset + 1:offset + 1 + length]
            mxcsr, program_cases = struct.unpack_from("<II", request, offset + 16)
            offset += 24
            if program_cases != int(count) or mxcsr > 0xFFFF:
                raise SystemExit(f"{request_path}: program {name} disagrees with its row")
            kept = 32 if vex_l(encoding) else 16
            key = (name, encoding)
            if key not in instructions:
                instructions[key] = len(instruction_rows)
                instruction_rows.append(
                    bytes([len(name)]) + name.encode() + bytes([length]) +
                    encoding + bytes([kept]))
            decoded = decode(encoding)
            for _ in range(program_cases):
                expected = flags(native, case)
                group = (name, daz, ftz, expected) + tuple(flags(r, case) for r in raw)
                if arguments.all or group not in seen:
                    seen.add(group)
                    data = request[offset:offset + CASE_INPUT]
                    result = native[case * RESPONSE:case * RESPONSE + 32]
                    marks = candidate_bits(name, decoded, double == "1", mxcsr,
                                           data, result[:kept], expected)
                    inputs = b"".join(data[register * 32:register * 32 + kept]
                                      for register in range(3))
                    cases.append(struct.pack("<HHB", instructions[key], mxcsr,
                                             expected | marks) + inputs)
                offset += CASE_INPUT
                case += 1
        if offset != len(request) or case * RESPONSE != len(native):
            raise SystemExit(f"{request_path}: sizes disagree with the responses")
    if len(instruction_rows) > 0xFFFF:
        raise SystemExit("too many instructions for a 16-bit index")
    output = (OUTPUT_MAGIC + struct.pack("<I", len(instruction_rows)) +
              b"".join(instruction_rows) + struct.pack("<I", len(cases)) +
              b"".join(cases))
    arguments.output.write_bytes(output)
    arguments.provenance.write_text(json.dumps({
        "cases": len(cases),
        "instructions": len(instruction_rows),
        "native_oracle": {"cpuid_signature": "0x000c06f2", "vendor": "GenuineIntel"},
        "inputs": hashes,
        "selection": [f"{prefix}-SET.bin" for prefix in arguments.raw_prefix],
        "subset_sha256": hashlib.sha256(output).hexdigest(),
    }, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
