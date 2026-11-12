#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pinned LLVM AArch64 decoder access shared by evidence tooling."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import shutil
import subprocess
from pathlib import Path
from typing import Callable


PINNED_LLVM_VERSION = "22.1.8"
MC_OPCODE = re.compile(r"<MCInst #[0-9]+ ([^\s>]+)")
INVALID_LINE = re.compile(
    r"<stdin>:(\d+):\d+: warning: invalid instruction encoding"
)
ALLOWED_WARNING = re.compile(
    r"<stdin>:\d+:\d+: warning: "
    r"(?:invalid|potentially undefined) instruction encoding"
)


def homebrew_llvm_mc_candidates() -> list[Path]:
    """Homebrew decoders, pinned keg first.

    The unversioned ``llvm`` keg tracks the newest release, so an unrelated
    upgrade would otherwise silently retire the pinned decoder.
    """

    major = PINNED_LLVM_VERSION.split(".")[0]
    return [
        Path(f"/opt/homebrew/opt/llvm@{major}/bin/llvm-mc"),
        Path("/opt/homebrew/opt/llvm/bin/llvm-mc"),
    ]


def find_llvm_mc(
    explicit: Path | None = None,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> Path:
    if explicit is not None:
        if not explicit.is_file():
            raise ValueError(f"LLVM decoder does not exist: {explicit}")
        return explicit
    candidates = []
    path_tool = shutil.which("llvm-mc")
    if path_tool is not None:
        candidates.append(Path(path_tool))
    candidates.extend(
        candidate
        for candidate in homebrew_llvm_mc_candidates()
        if candidate.is_file()
    )
    if not candidates:
        raise ValueError("LLVM llvm-mc was not found; pass --llvm-mc")
    for candidate in candidates:
        try:
            if llvm_version(candidate, runner) == PINNED_LLVM_VERSION:
                return candidate
        except (OSError, ValueError, subprocess.SubprocessError):
            continue
    # Leave the pinned-version diagnostic to require_pinned_llvm.
    return candidates[0]


def byte_line(word: int) -> str:
    return " ".join(f"0x{(word >> (byte * 8)) & 0xff:02x}" for byte in range(4))


def decoder_opcodes(
    tool: Path,
    words: list[int],
    mattr: str = "+all",
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> list[str | None]:
    if not words:
        return []
    command = [
        str(tool),
        "--disassemble",
        "--show-inst",
        "--triple=aarch64",
    ]
    if mattr:
        command.append(f"--mattr={mattr}")
    completed = runner(
        command,
        input="\n".join(byte_line(word) for word in words) + "\n",
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    invalid = {
        int(match.group(1)) - 1
        for match in INVALID_LINE.finditer(completed.stderr)
    }
    unexpected = [
        line
        for line in completed.stderr.splitlines()
        if "warning:" in line and ALLOWED_WARNING.fullmatch(line) is None
    ]
    if unexpected:
        raise ValueError(
            "llvm-mc emitted unexpected warnings: " + "; ".join(unexpected)
        )

    decoded = iter(MC_OPCODE.findall(completed.stdout))
    result: list[str | None] = []
    for index in range(len(words)):
        result.append(None if index in invalid else next(decoded, None))
    if any(opcode is None for index, opcode in enumerate(result) if index not in invalid):
        raise ValueError("llvm-mc output omitted a successful decoder opcode")
    if next(decoded, None) is not None:
        raise ValueError("llvm-mc output contains extra decoder opcodes")
    return result


def llvm_version(
    tool: Path,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> str:
    completed = runner(
        [str(tool), "--version"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    first = completed.stdout.splitlines()[0]
    match = re.search(
        r"(?:LLVM|version)\s+(?:version\s+)?([0-9]+(?:\.[0-9]+)+)", first
    )
    if match is None:
        raise ValueError(f"cannot parse LLVM version from {first!r}")
    return match.group(1)


def require_pinned_llvm(
    tool: Path,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> str:
    version = llvm_version(tool, runner)
    if version != PINNED_LLVM_VERSION:
        raise ValueError(
            f"LLVM decoder version is {version}, expected pinned "
            f"{PINNED_LLVM_VERSION}"
        )
    return f"llvm-mc-{version}"


MC_INSTRUCTION = re.compile(
    r"^\t(?P<text>[^\n]*?)\s*// <MCInst #[0-9]+ (?P<opcode>[^\s>]+)"
)
MC_OPERAND = re.compile(r"<MCOperand (?P<kind>[A-Za-z]+):(?P<value>[^>]*)>")


def decoder_instructions(
    tool: Path,
    words: list[int],
    mattr: str = "+all",
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> list[tuple[str, tuple[tuple[str, str], ...], str] | None]:
    """Decode words to (opcode, operands, assembly text), None if invalid.

    Each operand is (kind, value) as --show-inst prints it, for example
    ("Reg", "Z0_Z1") or ("Imm", "3").
    """
    if not words:
        return []
    command = [str(tool), "--disassemble", "--show-inst", "--triple=aarch64"]
    if mattr:
        command.append(f"--mattr={mattr}")
    completed = runner(
        command,
        input="\n".join(byte_line(word) for word in words) + "\n",
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    invalid = {
        int(match.group(1)) - 1
        for match in INVALID_LINE.finditer(completed.stderr)
    }
    unexpected = [
        line
        for line in completed.stderr.splitlines()
        if "warning:" in line and ALLOWED_WARNING.fullmatch(line) is None
    ]
    if unexpected:
        raise ValueError(
            "llvm-mc emitted unexpected warnings: " + "; ".join(unexpected)
        )
    blocks: list[tuple[str, list[tuple[str, str]], str]] = []
    for line in completed.stdout.splitlines():
        match = MC_INSTRUCTION.match(line)
        if match is not None:
            blocks.append((match.group("opcode"), [], match.group("text").strip()))
            line = line[match.end():]
        elif not blocks:
            continue
        for operand in MC_OPERAND.finditer(line):
            blocks[-1][1].append((operand.group("kind"), operand.group("value")))
    decoded = iter(blocks)
    result: list[tuple[str, tuple[tuple[str, str], ...], str] | None] = []
    for index in range(len(words)):
        if index in invalid:
            result.append(None)
            continue
        block = next(decoded, None)
        if block is None:
            raise ValueError("llvm-mc output omitted a decoded instruction")
        result.append((block[0], tuple(block[1]), block[2]))
    if next(decoded, None) is not None:
        raise ValueError("llvm-mc output contains extra instructions")
    return result
