#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pinned LLVM ARM and Thumb decoder access for the AArch32 evidence tooling.

llvm-mc decodes A32 and T32 words in the pinned release (see
a64_llvm_oracle.py). A decode witness is an MC opcode (ADDri, t2LDRi12,
VADDv8i8), which names a family of instructions; the table generator checks
that every row's words decode to an opcode of the row's mnemonic.

Thumb needs care: LLVM reads a 32-bit Thumb instruction as two halfwords
and, when an instruction does not decode, skips fewer bytes than it holds
(down to one), so everything after the first invalid item of a batch is
unreliable.
decode_words therefore records the items before the first invalid one, marks
it invalid, and decodes the rest again.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import subprocess
from pathlib import Path
from typing import Callable

from a64_llvm_oracle import (
    PINNED_LLVM_VERSION,
    find_llvm_mc,
    llvm_version,
    require_pinned_llvm,
)


__all__ = [
    "PINNED_LLVM_VERSION",
    "find_llvm_mc",
    "llvm_version",
    "require_pinned_llvm",
    "decode_words",
    "assemble",
    "MATTR",
    "OracleError",
]

# One feature set decodes everything the XML describes: Armv8.6-A with the
# optional extensions the profile has. Features the profile lacks decode too
# (the table classifies them), so no row is hidden from the oracle.
MATTR = ",".join(
    (
        "+neon", "+fp-armv8", "+crypto", "+crc", "+dotprod", "+fullfp16",
        "+fp16fml", "+bf16", "+i8mm", "+sb", "+ras", "+hwdiv", "+hwdiv-arm",
        "+mp", "+virtualization", "+trustzone", "+dsp", "+aes", "+sha2",
        "+d32", "+vfp4",
    )
)
TRIPLES = {"A32": "armv8.6a", "T32": "thumbv8.6a"}
MC_OPCODE = re.compile(r"@ <MCInst #[0-9]+ ([^\s>]+)")
INVALID = re.compile(
    r"<stdin>:(\d+):(\d+): warning: invalid instruction encoding"
)
# LLVM decodes an encoding it considers UNPREDICTABLE and says so.
SOFT_FAIL = re.compile(
    r"<stdin>:(\d+):(\d+): warning: potentially undefined instruction encoding"
)
UNEXPECTED = re.compile(r"warning:|error:")
BATCH_LIMIT = 4096
THUMB_CHUNK = 512
TIMEOUT_SECONDS = 180


class OracleError(ValueError):
    pass


def byte_line(word: int, length: int) -> str:
    return " ".join(f"0x{(word >> (8 * i)) & 0xff:02x}" for i in range(length))


def thumb_bytes(word: int, length: int) -> str:
    """Bytes of a Thumb instruction given as hw1 << 16 | hw2 (or hw1 << 16)."""
    hw1 = word >> 16
    if length == 2:
        return byte_line(hw1, 2)
    return f"{byte_line(hw1, 2)} {byte_line(word & 0xffff, 2)}"


def run_mc(
    tool: Path,
    triple: str,
    lines: list[str],
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    mattr: str = MATTR,
) -> subprocess.CompletedProcess[str]:
    completed = runner(
        [
            str(tool),
            "--disassemble",
            "--show-inst",
            f"--triple={triple}",
            f"--mattr={mattr}",
        ],
        input="\n".join(lines) + "\n",
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=TIMEOUT_SECONDS,
    )
    for line in completed.stderr.splitlines():
        if UNEXPECTED.search(line) and not (
            INVALID.fullmatch(line) or SOFT_FAIL.fullmatch(line)
        ):
            raise OracleError(f"llvm-mc emitted an unexpected diagnostic: {line}")
    return completed


def decode_words(
    tool: Path,
    isa: str,
    items: list[tuple[int, int]],
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    mattr: str = MATTR,
) -> list[tuple[str, bool] | None]:
    """(MC opcode, soft-fail) per (word, length) item, None where llvm-mc
    rejects it.

    For A32 the word is the 32-bit instruction and the length 4. For T32 the
    word is hw1 << 16 | hw2 and the length is 2 or 4 bytes. Soft-fail means
    llvm-mc decoded the word but warned that the encoding is UNPREDICTABLE.
    """
    if isa == "A32":
        return _decode_a32(tool, items, runner, mattr)
    return _decode_thumb(tool, items, runner, mattr)


def _lines(pattern, text: str, limit: int | None = None) -> set[int]:
    return {
        int(match.group(1)) - 1
        for match in pattern.finditer(text)
        if limit is None or int(match.group(1)) - 1 < limit
    }


def _decode_a32(tool, items, runner, mattr) -> list[tuple[str, bool] | None]:
    """A32 words are fixed four-byte units, so an invalid one cannot desync."""
    result: list[tuple[str, bool] | None] = []
    for start in range(0, len(items), BATCH_LIMIT):
        chunk = items[start:start + BATCH_LIMIT]
        completed = run_mc(
            tool, TRIPLES["A32"], [byte_line(word, 4) for word, _l in chunk],
            runner, mattr,
        )
        invalid = _lines(INVALID, completed.stderr)
        soft = _lines(SOFT_FAIL, completed.stderr)
        opcodes = iter(MC_OPCODE.findall(completed.stdout))
        for index in range(len(chunk)):
            result.append(
                None if index in invalid else (next(opcodes), index in soft)
            )
        if next(opcodes, None) is not None:
            raise OracleError("llvm-mc produced extra instructions")
    return result


def _decode_thumb(tool, items, runner, mattr) -> list[tuple[str, bool] | None]:
    result: list[tuple[str, bool] | None] = []
    position = 0
    while position < len(items):
        chunk = items[position:position + THUMB_CHUNK]
        completed = run_mc(
            tool, TRIPLES["T32"],
            [thumb_bytes(word, length) for word, length in chunk],
            runner, mattr,
        )
        opcodes = MC_OPCODE.findall(completed.stdout)
        first = INVALID.search(completed.stderr)
        if first is None:
            if len(opcodes) != len(chunk):
                raise OracleError(
                    f"llvm-mc decoded {len(opcodes)} instructions from "
                    f"{len(chunk)} words"
                )
            soft = _lines(SOFT_FAIL, completed.stderr)
            result.extend((opcode, index in soft) for index, opcode in enumerate(opcodes))
            position += len(chunk)
            continue
        index = int(first.group(1)) - 1
        if int(first.group(2)) != 1 or len(opcodes) < index:
            raise OracleError("llvm-mc invalid-encoding position is unexpected")
        soft = _lines(SOFT_FAIL, completed.stderr, index)
        result.extend((opcode, i in soft) for i, opcode in enumerate(opcodes[:index]))
        result.append(None)
        position += index + 1
    return result


def assemble(
    tool: Path,
    isa: str,
    source: str,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    mattr: str = MATTR,
) -> list[tuple[int, int]]:
    """Assemble text to [(word, length)] in the decoder's word layout."""
    completed = runner(
        [
            str(tool),
            "--assemble",
            "--show-encoding",
            f"--triple={TRIPLES[isa]}",
            f"--mattr={mattr}",
        ],
        input=source + "\n",
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=TIMEOUT_SECONDS,
    )
    if completed.stderr.strip():
        raise OracleError(f"llvm-mc rejected the source: {completed.stderr.strip()}")
    result: list[tuple[int, int]] = []
    for line in completed.stdout.splitlines():
        match = re.search(r"encoding: \[([^\]]*)\]", line)
        if match is None:
            continue
        data = [int(item, 16) for item in match.group(1).split(",")]
        if isa == "A32":
            if len(data) != 4:
                raise OracleError("A32 encoding is not four bytes")
            result.append((int.from_bytes(bytes(data), "little"), 4))
        elif len(data) == 2:
            result.append(((data[0] | data[1] << 8) << 16, 2))
        elif len(data) == 4:
            hw1 = data[0] | data[1] << 8
            hw2 = data[2] | data[3] << 8
            result.append((hw1 << 16 | hw2, 4))
        else:
            raise OracleError(f"unexpected Thumb encoding length {len(data)}")
    return result
