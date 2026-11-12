#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Normalize pinned K and FEX x86-64 cases, and the repository's own cases,
into the generated unit corpus."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import NoReturn

sys.path.insert(
    0, str(pathlib.Path(__file__).resolve().parents[1] / "fex-tests"))
import fex_config  # noqa: E402
from pinned_source import manifest_identity, sha256_file, verify_checkout  # noqa: E402


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
CORPUS = REPOSITORY / "tests" / "unit" / "x86_64" / "corpus"
REPOSITORY_CASES = CORPUS / "cases"
CODE_ADDRESS = 0x1_0000
STACK_ADDRESS = 0x7000_0000
PAGE_SIZE = 0x1000
PACK_MAGIC = b"UMLX86T\0"
PACK_VERSION = 1
PACK_KINDS = {"fex": 1, "k": 2, "repository": 3}
CASE_REQUEST_MAGIC = b"UMLCREQ\0"
CASE_RESPONSE_MAGIC = b"UMLCRSP\0"
# The native helper records each state as an event, an instruction index or
# RIP offset, sixteen GPRs, sixteen YMM registers and RFLAGS.
NATIVE_STATE_SIZE = 4 + 4 + 16 * 8 + 16 * 32 + 8

# These K floating-point traces contain concrete finite results that disagree
# with native Intel execution. Bind the complete programs to the native oracle
# while retaining K's defined-state masks for every step. In particular, the
# F16C trace rounds a tiny positive value to zero for round-toward-+infinity;
# Intel documents and produces the least positive half subnormal instead.
NATIVE_OVERRIDE_SOURCES = frozenset(
    {
        f"tests/single-instruction-tests/{name}/test.s"
        for name in (
            "mulpdpssdsd",
            "vadd",
            "vaddsub",
            "vcvtps2ph_xmm_xmm_imm8",
            "vfmadd",
            "vfmaddsub",
            "vfmsub",
            "vfnmadd",
            "vfnmsub",
            "vsub",
        )
    }
)
EXACT_NATIVE_OVERRIDE_SOURCES = frozenset(
    {"tests/single-instruction-tests/vcvtps2ph_xmm_xmm_imm8/test.s"}
)

# K computes RCPSS and RSQRTSS as exact quotients, also for the denormal
# inputs and tiny results the SDM defines as zero and infinity. These programs
# take a native trace too: a K lane further from the native lane than the
# SDM's estimate error is disputed, and the native value replaces it. The
# remaining K quotients lie within that error.
ESTIMATE_SOURCES = frozenset(
    {
        f"tests/single-instruction-tests/{name}/test.s"
        for name in ("rcp", "rsqrt")
    }
)
ESTIMATE_RELATIVE_ERROR = 1.5 * 2.0**-12

# K chooses concrete NaN signs and payloads for these floating-point programs
# where the Intel architecture permits another quiet NaN. Keep every other K
# trace bit-exact so data movement and integer cases cannot inherit a looser
# comparison merely because an untouched vector happens to encode a NaN.
K_NAN_CLASS_SOURCES = (
    NATIVE_OVERRIDE_SOURCES - EXACT_NATIVE_OVERRIDE_SOURCES
) | frozenset(
    {
        f"tests/single-instruction-tests/{name}/test.s"
        for name in (
            "add_single",
            "cvtsd2ss2sd",
            "cvtss2sd2ss",
            "div_single",
            "divpdpssdsd",
            "mul_single",
            "rcp",
            "rsqrt",
        )
    }
)

GPR_NAMES = (
    "RAX",
    "RBX",
    "RCX",
    "RDX",
    "RSI",
    "RDI",
    "RBP",
    "RSP",
    "R8",
    "R9",
    "R10",
    "R11",
    "R12",
    "R13",
    "R14",
    "R15",
)
FLAG_NAMES = ("CF", "PF", "AF", "ZF", "SF", "DF", "OF")

# The ordinary unit profile is the portable userspace ISA through AVX2.  AES,
# PCLMUL, XSAVE, FSGSBASE, and ADX remain because they are already part of the
# product's documented userspace profile and are useful to desktop software.
# So are the AVX-VNNI dot products, which both x86 engines run and CPUID
# reports.
# ADX is what guest OpenSSL dispatches its Montgomery multiply on, so its two
# carry chains and their flags belong in the pinned corpus.
ALLOWED_FEX_FEATURES = frozenset(
    {
        "AVX",
        "AVX_VNNI",
        "SSE4.1",
        "SSSE3",
        "SSE4.2",
        "BMI1",
        "BMI2",
        "MOVBE",
        "AES",
        "PCLMUL",
        "XSAVE",
        "FSGSBASE",
        "ADX",
        "Linux",
    }
)
EXCLUDED_FEX_FEATURES = {
    "3DNOW": "obsolete AMD-only 3DNow extension",
    "SSE4A": "AMD-only extension outside x86-64-v3",
    "EMMI": "obsolete AMD multimedia extension",
    "SHA": "post-AVX2 SHA extension",
    "AES256": "post-AVX2 vector AES extension",
    "CLZERO": "AMD-only cache-zero extension",
    "RAND": "nondeterministic random-number extension outside v3",
    "AFP": "AMD floating-point extension outside v3",
    "RDPID": "post-AVX2 processor-identification extension",
    "CLFLOPT": "post-AVX2 cache-management extension",
    "CLWB": "post-AVX2 cache-management extension",
}

# FEX programs whose expected state is FEX's own emulation where x86 differs.
# FEX lists each among the programs its native-host runner cannot pass.
EXCLUDED_FEX_SOURCES = {
    "VEX/vldmxcsr.asm": "expects FEX's MXCSR, which drops the exception flags "
    "LDMXCSR loads and x86 keeps; upstream disables it on native hosts",
    "X87/X87MMXInteraction.asm": "expects FEX's tag word, which FNSAVE stores "
    "as valid for every full register where x86 tags zeros and special "
    "values; upstream lists it as a known failure on native hosts",
    "Primary/Primary_8C.asm": "expects FEX's selector loads, which accept "
    "selectors beyond the GDT or naming an absent LDT where x86 raises #GP; "
    "upstream disables it on native hosts",
    "Primary/Primary_8C_2.asm": "expects FEX's selector loads under REX.R, "
    "which accept selectors beyond the GDT or naming an absent LDT where x86 "
    "raises #GP; upstream disables it on native hosts",
    "PrimaryGroup/5_FF_05.asm": "expects AMD's REX.W far JMP, which reads "
    "m16:32, where Intel reads m16:64 and takes the null selector after it "
    "to #GP(0)",
    "PrimaryGroup/5_FF_05_03_o64.asm": "expects AMD's REX.W far CALL, which "
    "reads m16:32, where Intel reads m16:64 and takes the null selector "
    "after it to #GP(0)",
    "PrimaryGroup/5_FF_05_03_o64_imm.asm": "expects AMD's REX.W far CALL, "
    "which reads m16:32, where Intel reads m16:64 and takes the null "
    "selector after it to #GP(0)",
    "modrm_oob/DDD.asm": "runs the AMD-only 3DNow! family, which upstream "
    "does not annotate with its 3DNOW feature; an Intel core and the guest "
    "raise #UD",
}

# The programs above that cite upstream's Known_Failures_host list. A program
# on that list is excluded, and a program is excluded for that reason only
# while it stays on the list, so an upstream fix cannot leave a stale
# exclusion behind.
KNOWN_FAILURE_HOST_SOURCES = frozenset({"X87/X87MMXInteraction.asm"})

# HLT is the FEX test terminator and is not part of a case's userspace form.
# The other instructions are either privileged or belong to Intel-only
# facilities explicitly outside the product profile.
EXCLUDED_MNEMONICS = {
    "xbegin": "Intel TSX transactional-memory instruction",
    "xend": "Intel TSX transactional-memory instruction",
    "xabort": "Intel TSX transactional-memory instruction",
    "xtest": "Intel TSX transactional-memory instruction",
    "bndcl": "Intel MPX instruction",
    "bndcu": "Intel MPX instruction",
    "bndcn": "Intel MPX instruction",
    "bndmk": "Intel MPX instruction",
    "encls": "Intel enclave instruction",
    "enclu": "Intel enclave instruction",
    "vmcall": "Intel VMX instruction",
    "vmlaunch": "Intel VMX instruction",
    "vmresume": "Intel VMX instruction",
    "vmxoff": "Intel VMX instruction",
    "vmxon": "Intel VMX instruction",
    "in": "privileged port-I/O instruction",
    "ins": "privileged port-I/O instruction",
    "out": "privileged port-I/O instruction",
    "outs": "privileged port-I/O instruction",
    "cli": "privileged interrupt-control instruction",
    "sti": "privileged interrupt-control instruction",
    "lgdt": "privileged descriptor-table instruction",
    "lidt": "privileged descriptor-table instruction",
    "lldt": "privileged descriptor-table instruction",
    "ltr": "privileged task-register instruction",
    "invlpg": "privileged address-translation instruction",
    "swapgs": "privileged kernel-context instruction",
    "sysret": "privileged syscall-return instruction",
    "iret": "privileged interrupt-return instruction",
    "retf": "obsolete far control transfer",
    "lcall": "obsolete far control transfer",
    "ljmp": "obsolete far control transfer",
}

SETUP_MNEMONICS = frozenset(
    {"mov", "movabs", "lea", "push", "pop", "nop", "cld", "std"}
)


def fail(message: str) -> NoReturn:
    raise SystemExit(f"build-unit-corpus.py: {message}")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def resolve_tool(environment: str, fallback: str) -> str:
    requested = os.environ.get(environment, fallback)
    resolved = shutil.which(requested)
    if resolved is None:
        fail(f"required tool is unavailable: {requested}")
    return resolved


@dataclass(frozen=True)
class Instruction:
    address: int
    mnemonic: str
    assembly: str
    form: str


@dataclass
class State:
    rip_index: int
    gprs: list[int | None]
    vectors: list[int | None]
    flags: list[int | None]
    expected_event: int = 0


@dataclass(frozen=True)
class Record:
    origin: str
    source: str
    name: str
    code: bytes
    payload: bytes
    case_count: int
    forms: tuple[str, ...]
    oracle: str
    upstream_disabled: bool = False
    nan_class_equivalence: bool = False


@dataclass(frozen=True)
class Exclusion:
    origin: str
    source: str
    reason: str


@dataclass(frozen=True)
class KProgram:
    source: pathlib.Path
    relative: str
    code: bytes
    instructions: list[Instruction]
    offsets: list[int]
    formal_states: list[State] | None


def normalize_mnemonic(mnemonic: str) -> str:
    value = mnemonic.lower().removeprefix("lock\t")
    aliases = {
        "movabsq": "mov",
        "movabs": "mov",
        "movzbq": "movzx",
        "movzbl": "movzx",
        "movzbw": "movzx",
        "movzwl": "movzx",
        "movzwq": "movzx",
        "movsbq": "movsx",
        "movsbl": "movsx",
        "movsbw": "movsx",
        "movswl": "movsx",
        "movswq": "movsx",
        "movslq": "movsx",
    }
    if value in aliases:
        return aliases[value]
    if value.startswith("rep") and value not in {"ret", "retq", "retl"}:
        return value
    if value.endswith(("b", "w", "l", "q")):
        stem = value[:-1]
        if stem in {
            "adc",
            "add",
            "and",
            "bsf",
            "bsr",
            "bt",
            "btc",
            "btr",
            "bts",
            "call",
            "cmp",
            "cmpxchg",
            "dec",
            "div",
            "idiv",
            "imul",
            "inc",
            "jmp",
            "lea",
            "mov",
            "mul",
            "neg",
            "not",
            "or",
            "pop",
            "push",
            "rcl",
            "rcr",
            "rol",
            "ror",
            "sar",
            "sbb",
            "shl",
            "shr",
            "sub",
            "test",
            "xadd",
            "xchg",
            "xor",
        }:
            return stem
    return value


def split_operands(text: str) -> list[str]:
    operands: list[str] = []
    begin = 0
    depth = 0
    for index, character in enumerate(text):
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        elif character == "," and depth == 0:
            operands.append(text[begin:index].strip())
            begin = index + 1
    remainder = text[begin:].strip()
    if remainder:
        operands.append(remainder)
    return operands


def register_class(register: str) -> str:
    value = register.lower().lstrip("%")
    if re.fullmatch(r"[xyz]mm\d+", value):
        return value[:3]
    if re.fullmatch(r"mm\d+", value):
        return "mm"
    if value.startswith("st"):
        return "st"
    if value in {"al", "bl", "cl", "dl", "ah", "bh", "ch", "dh"} or value.endswith("b"):
        return "r8"
    if value in {"ax", "bx", "cx", "dx", "si", "di", "sp", "bp"} or value.endswith("w"):
        return "r16"
    if value.startswith("e") or value.endswith("d"):
        return "r32"
    if value.startswith("r"):
        return "r64"
    return value


def canonical_form(assembly: str) -> tuple[str, str]:
    instruction = assembly.split("#", 1)[0].strip()
    # LLVM marks the VEX encoding of an instruction that also has an EVEX one,
    # as AVX-VNNI's does, with a pseudo-prefix that is not part of its form.
    instruction = instruction.removeprefix("{vex}").strip()
    if not instruction:
        return "", ""
    fields = instruction.split(None, 1)
    raw_mnemonic = fields[0].lower()
    mnemonic = normalize_mnemonic(raw_mnemonic)
    shapes: list[str] = []
    if len(fields) == 2:
        for operand in split_operands(fields[1]):
            if operand.startswith("$"):
                shapes.append("imm")
            elif operand.startswith("%"):
                shapes.append(register_class(operand))
            elif "(" in operand or operand.startswith("0x"):
                shapes.append("mem")
            else:
                shapes.append("rel")
    width_suffix = raw_mnemonic[-1:] if raw_mnemonic[-1:] in "bwlq" else ""
    return mnemonic, f"{mnemonic}{width_suffix}:{','.join(shapes)}"


def parse_disassembly(text: str) -> list[Instruction]:
    instructions: list[Instruction] = []
    line_pattern = re.compile(r"^\s*([0-9a-fA-F]+):\s+(\S.*)$")
    for line in text.splitlines():
        match = line_pattern.match(line)
        if match is None:
            continue
        assembly = match.group(2).strip()
        mnemonic, form = canonical_form(assembly)
        if not mnemonic:
            continue
        instructions.append(
            Instruction(int(match.group(1), 16), mnemonic, assembly, form)
        )
    return instructions


def assemble_program(
    source: pathlib.Path,
    temporary: pathlib.Path,
    clang: str,
    linker: str,
    objcopy: str,
    objdump: str,
    readelf: str,
    label: str = "K",
) -> tuple[bytes, list[Instruction], list[int]]:
    stem = hashlib.sha256(str(source).encode()).hexdigest()[:16]
    obj = temporary / f"{stem}.o"
    executable = temporary / f"{stem}.elf"
    binary = temporary / f"{stem}.bin"
    subprocess.run(
        [clang, "-target", "x86_64-unknown-linux-gnu", "-c", str(source), "-o", str(obj)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    subprocess.run(
        [
            linker,
            "-m",
            "elf_x86_64",
            "--image-base=0",
            "-nostdlib",
            f"-Ttext={CODE_ADDRESS:#x}",
            "-e",
            "_start",
            "-o",
            str(executable),
            str(obj),
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    subprocess.run(
        [objcopy, "-O", "binary", str(executable), str(binary)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    disassembly = subprocess.run(
        [objdump, "-d", "--no-show-raw-insn", str(executable)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout
    instructions = parse_disassembly(disassembly)
    if not instructions or instructions[0].address != CODE_ADDRESS:
        fail(f"unexpected linked instruction layout: {source}")
    sections = subprocess.run(
        [readelf, "-S", "--wide", str(executable)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    ).stdout
    section_match = re.search(
        r"\]\s+\.text\s+PROGBITS\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)",
        sections,
    )
    if section_match is None:
        fail(f"linked {label} program has no .text section: {source}")
    text_address = int(section_match.group(1), 16)
    text_size = int(section_match.group(2), 16)
    if text_address != CODE_ADDRESS:
        fail(f"linked {label} .text has an unexpected address: {source}")
    offsets = [instruction.address - CODE_ADDRESS for instruction in instructions]
    offsets.append(text_size)
    if offsets != sorted(offsets) or len(set(offsets)) != len(offsets):
        fail(f"linked {label} instruction offsets are not strictly ordered: {source}")
    return binary.read_bytes(), instructions, offsets


def parse_integer(expression: str, expected_width: int) -> int | None:
    match = re.fullmatch(rf"{expected_width}'(-?\d+)\s*\)?", expression.strip())
    if match is None:
        return None
    return int(match.group(1)) % (1 << expected_width)


def resolve_pointer(
    expression: str, instruction_offsets: list[int]
) -> int | None:
    text_match = re.search(
        r"ptr \( symloc \( -1 , 0 , 0 , 0 , 0 \) , 64'(-?\d+) \)",
        expression,
    )
    if text_match is not None:
        index = int(text_match.group(1))
        if index < 0 or index >= len(instruction_offsets):
            return None
        return CODE_ADDRESS + instruction_offsets[index]
    stack_match = re.search(
        r"ptr \( symloc \( 1 , [^)]*\) , 64'(-?\d+) \)", expression
    )
    if stack_match is not None:
        return (STACK_ADDRESS + int(stack_match.group(1))) & ((1 << 64) - 1)
    return None


def register_operand_value(state: State, operand: str) -> tuple[int, int] | None:
    register = operand.strip().lower().lstrip("%")
    aliases: dict[str, tuple[int, int, int]] = {}
    roots = (
        ("ax", "eax", "rax"),
        ("bx", "ebx", "rbx"),
        ("cx", "ecx", "rcx"),
        ("dx", "edx", "rdx"),
        ("si", "esi", "rsi"),
        ("di", "edi", "rdi"),
        ("bp", "ebp", "rbp"),
        ("sp", "esp", "rsp"),
    )
    byte_names = ("al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl")
    for index, ((word, dword, qword), byte) in enumerate(zip(roots, byte_names)):
        aliases[byte] = (index, 8, 0)
        aliases[word] = (index, 16, 0)
        aliases[dword] = (index, 32, 0)
        aliases[qword] = (index, 64, 0)
    for index, byte in enumerate(("ah", "bh", "ch", "dh")):
        aliases[byte] = (index, 8, 8)
    for number in range(8, 16):
        index = number
        aliases[f"r{number}b"] = (index, 8, 0)
        aliases[f"r{number}w"] = (index, 16, 0)
        aliases[f"r{number}d"] = (index, 32, 0)
        aliases[f"r{number}"] = (index, 64, 0)
    location = aliases.get(register)
    if location is None:
        return None
    index, width, shift = location
    value = state.gprs[index]
    if value is None:
        return None
    return (value >> shift) & ((1 << width) - 1), width


def signed_value(value: int, width: int) -> int:
    sign = 1 << (width - 1)
    return value - (1 << width) if value & sign else value


def divide_faults(instruction: Instruction, state: State) -> bool:
    if instruction.mnemonic not in {"div", "idiv"}:
        return False
    assembly = instruction.assembly.split("#", 1)[0].strip()
    fields = assembly.split(None, 1)
    if len(fields) != 2:
        fail(f"cannot classify K divide operand: {instruction.assembly}")
    raw_mnemonic = fields[0].lower()
    widths = {"b": 8, "w": 16, "l": 32, "q": 64}
    width = widths.get(raw_mnemonic[-1:])
    operand = register_operand_value(state, fields[1])
    if width is None or operand is None or operand[1] != width:
        fail(f"cannot classify K divide form: {instruction.assembly}")
    divisor = operand[0]
    rax = state.gprs[0]
    rdx = state.gprs[3]
    if rax is None or rdx is None:
        fail(f"K divide has a non-concrete dividend: {instruction.assembly}")
    if width == 8:
        dividend = rax & 0xFFFF
    else:
        mask = (1 << width) - 1
        dividend = ((rdx & mask) << width) | (rax & mask)
    if divisor == 0:
        return True
    if instruction.mnemonic == "div":
        return dividend // divisor >= 1 << width
    signed_dividend = signed_value(dividend, width * 2)
    signed_divisor = signed_value(divisor, width)
    quotient = abs(signed_dividend) // abs(signed_divisor)
    if (signed_dividend < 0) != (signed_divisor < 0):
        quotient = -quotient
    return quotient < -(1 << (width - 1)) or quotient >= 1 << (width - 1)


def parse_k_states(
    path: pathlib.Path,
    instructions: list[Instruction],
    instruction_offsets: list[int],
) -> list[State]:
    text = path.read_text()
    try:
        queue = text.split("<regstatequeue>", 1)[1].split("</regstatequeue>", 1)[0]
    except IndexError:
        fail(f"K state has no register-state queue: {path}")
    blocks: list[list[str]] = []
    current: list[str] | None = None
    for line in queue.splitlines():
        if line.strip() == "ListItem (":
            if current is not None:
                blocks.append(current)
            current = []
        elif current is not None:
            current.append(line.strip())
    if current is not None:
        blocks.append(current)
    if not blocks:
        fail(f"K state queue is empty: {path}")

    mapping_pattern = re.compile(r'^"([A-Z0-9]+)" \|-> (.*)$')
    states: list[State] = []
    for block in blocks:
        values: dict[str, str] = {}
        for line in block:
            match = mapping_pattern.match(line)
            if match is not None:
                values[match.group(1)] = match.group(2)
        rip_expression = values.get("RIP", "")
        rip_match = re.search(
            r"ptr \( symloc \( -1 , 0 , 0 , 0 , 0 \) , 64'(-?\d+) \)",
            rip_expression,
        )
        if rip_match is None:
            plain_rip = parse_integer(rip_expression, 64)
            if plain_rip is None:
                fail(f"K trace has a non-concrete RIP: {path}")
            rip_index = plain_rip
        else:
            rip_index = int(rip_match.group(1))
        if rip_index < 0 or rip_index >= len(instruction_offsets):
            fail(f"K trace RIP is outside the linked program: {path}: {rip_index}")

        gprs: list[int | None] = []
        for name in GPR_NAMES:
            expression = values.get(name, "")
            value = parse_integer(expression, 64)
            if value is None:
                value = resolve_pointer(expression, instruction_offsets)
            gprs.append(value)
        vectors = [
            parse_integer(values.get(f"YMM{index}", ""), 256)
            for index in range(16)
        ]
        flags = [parse_integer(values.get(name, ""), 1) for name in FLAG_NAMES]
        states.append(State(rip_index, gprs, vectors, flags))
    for index in range(1, len(states)):
        previous = states[index - 1]
        if previous.rip_index >= len(instructions):
            fail(f"K trace executes beyond the linked program: {path}")
        states[index].expected_event = int(
            divide_faults(instructions[previous.rip_index], previous)
        )
    return states


def encode_state(state: State, previous: State | None) -> bytes:
    gpr_valid = sum(
        (value is not None) << index for index, value in enumerate(state.gprs)
    )
    vector_valid = sum(
        (value is not None) << index for index, value in enumerate(state.vectors)
    )
    flag_valid = sum(
        (value is not None) << index for index, value in enumerate(state.flags)
    )
    if previous is None:
        gpr_changed = gpr_valid
        vector_changed = vector_valid
        flag_changed = flag_valid
    else:
        gpr_changed = sum(
            ((value is not None) and
             (previous.gprs[index] is None or previous.gprs[index] != value))
            << index
            for index, value in enumerate(state.gprs)
        )
        vector_changed = sum(
            ((value is not None) and
             (previous.vectors[index] is None or previous.vectors[index] != value))
            << index
            for index, value in enumerate(state.vectors)
        )
        flag_changed = sum(
            ((value is not None) and
             (previous.flags[index] is None or previous.flags[index] != value))
            << index
            for index, value in enumerate(state.flags)
        )
    flag_values = sum(
        bool(value) << index for index, value in enumerate(state.flags) if value is not None
    )
    output = bytearray(
        struct.pack(
            "<IHHHHBBBB",
            state.rip_index,
            gpr_valid,
            gpr_changed,
            vector_valid,
            vector_changed,
            flag_valid,
            flag_changed,
            flag_values,
            state.expected_event,
        )
    )
    for index, value in enumerate(state.gprs):
        if gpr_changed & (1 << index):
            assert value is not None
            output.extend(struct.pack("<Q", value))
    for index, value in enumerate(state.vectors):
        if vector_changed & (1 << index):
            assert value is not None
            output.extend(value.to_bytes(32, "little"))
    return bytes(output)


def encode_k_payload(states: list[State], instruction_offsets: list[int]) -> bytes:
    output = bytearray(
        struct.pack("<III", len(instruction_offsets) - 1, len(states), len(instruction_offsets))
    )
    output.extend(struct.pack(f"<{len(instruction_offsets)}I", *instruction_offsets))
    previous: State | None = None
    for state in states:
        output.extend(encode_state(state, previous))
        previous = state
    return bytes(output)


def needs_native_trace(program: KProgram) -> bool:
    return (
        program.formal_states is None or
        program.relative in NATIVE_OVERRIDE_SOURCES or
        program.relative in ESTIMATE_SOURCES
    )


def single_value(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def within_estimate(formal: int, native: int) -> bool:
    """Whether native single `native` estimates K's exact single `formal`."""
    if formal == native:
        return True
    exponent = 0x7F80_0000
    if any(value & exponent in (0, exponent) for value in (formal, native)):
        return False
    if (formal ^ native) & 0x8000_0000:
        return False
    exact = single_value(formal)
    return abs(single_value(native) - exact) <= (
        ESTIMATE_RELATIVE_ERROR * abs(exact))


def estimate_vector(formal: int, native: int) -> int:
    merged = 0
    for lane in range(8):
        shift = 32 * lane
        formal_lane = (formal >> shift) & 0xFFFF_FFFF
        native_lane = (native >> shift) & 0xFFFF_FFFF
        both_nan = all(
            value & 0x7FFF_FFFF > 0x7F80_0000
            for value in (formal_lane, native_lane)
        )
        chosen = formal_lane if both_nan or within_estimate(
            formal_lane, native_lane) else native_lane
        merged |= chosen << shift
    return merged


def native_request(programs: list[KProgram]) -> bytes:
    records = bytearray()
    selected = [program for program in programs if needs_native_trace(program)]
    for program in selected:
        control = [
            instruction.assembly
            for instruction in program.instructions
            if instruction.mnemonic.startswith("j") or
            instruction.mnemonic.startswith("loop") or
            instruction.mnemonic in {"call", "ret"}
        ]
        if control:
            fail(
                "native-only K source needs an explicit control-flow path: "
                f"{program.relative}: {control[0]}"
            )
        name = program.relative.encode()
        path = list(range(len(program.instructions) + 1))
        records.extend(
            struct.pack(
                "<IIII",
                len(name),
                len(program.code),
                len(program.instructions),
                len(path),
            )
        )
        records.extend(struct.pack(f"<{len(program.offsets)}I", *program.offsets))
        records.extend(struct.pack(f"<{len(path)}I", *path))
        records.extend(name)
        records.extend(program.code)
    identity = hashlib.sha256(records).digest()
    return PACK_MAGIC.replace(b"UMLX86T", b"UMLKREQ") + struct.pack(
        "<II", 1, len(selected)
    ) + identity + records


def native_states(path: pathlib.Path, request: bytes) -> tuple[dict[str, list[State]], dict[str, str]]:
    data = path.read_bytes()
    cursor = 0

    def take(size: int) -> bytes:
        nonlocal cursor
        if size > len(data) - cursor:
            fail("native K response is truncated")
        value = data[cursor:cursor + size]
        cursor += size
        return value

    if take(8) != b"UMLKRSP\0":
        fail("native K response has invalid magic")
    version, record_count = struct.unpack("<II", take(8))
    if version != 1:
        fail("native K response has an unsupported version")
    vendor = take(12).decode("ascii", errors="strict")
    signature = struct.unpack("<I", take(4))[0]
    request_identity = take(32)
    if request_identity != request[16:48]:
        fail("native K response does not match the generated request")
    output: dict[str, list[State]] = {}
    flag_masks = (1 << 0, 1 << 2, 1 << 4, 1 << 6, 1 << 7, 1 << 10, 1 << 11)
    for _ in range(record_count):
        name_size, state_count = struct.unpack("<II", take(8))
        if name_size == 0 or state_count < 2:
            fail("native K response has an invalid record header")
        name = take(name_size).decode("utf-8", errors="strict")
        if name in output:
            fail(f"native K response repeats a source: {name}")
        states: list[State] = []
        for _ in range(state_count):
            event, rip_index = struct.unpack("<II", take(8))
            if event > 1:
                fail(f"native K response has an invalid event: {name}")
            gprs = list(struct.unpack("<16Q", take(16 * 8)))
            vectors = [int.from_bytes(take(32), "little") for _ in range(16)]
            rflags = struct.unpack("<Q", take(8))[0]
            flags = [int(bool(rflags & mask)) for mask in flag_masks]
            states.append(State(rip_index, gprs, vectors, flags, event))
        output[name] = states
    if cursor != len(data):
        fail("native K response has trailing bytes")
    return output, {
        "vendor": vendor,
        "cpuid_signature": f"0x{signature:08x}",
        "response_sha256": sha256_file(path),
    }


def disassemble_raw(code: bytes, llvm_mc: str) -> list[Instruction]:
    # FEX binaries can carry large data regions after their HLT marker. Feed
    # LLVM only through a candidate marker so a data-heavy case does not turn
    # corpus generation into an unbounded linear disassembly.
    candidate = code.find(b"\xf4")
    attempts = 0
    while candidate >= 0 and attempts < 32:
        attempts += 1
        prefix = code[: candidate + 1]
        tokens = " ".join(f"0x{byte:02x}" for byte in prefix) + "\n"
        result = subprocess.run(
            [llvm_mc, "--disassemble", "--triple=x86_64-unknown-linux-gnu"],
            input=tokens,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode == 0:
            instructions: list[Instruction] = []
            for index, line in enumerate(result.stdout.splitlines()):
                assembly = line.strip()
                if not assembly:
                    continue
                mnemonic, form = canonical_form(assembly)
                if not mnemonic:
                    continue
                instructions.append(
                    Instruction(index, mnemonic, assembly, form)
                )
            if instructions and instructions[-1].mnemonic == "hlt":
                return instructions
        candidate = code.find(b"\xf4", candidate + 1)
    fail("FEX binary has no linearly disassembled HLT terminator")


def profile_exclusion(instructions: list[Instruction]) -> str | None:
    for instruction in instructions:
        reason = EXCLUDED_MNEMONICS.get(instruction.mnemonic)
        if reason is not None:
            return reason
    return None


def semantic_forms(instructions: list[Instruction]) -> set[str]:
    forms = {
        instruction.form
        for instruction in instructions
        if instruction.mnemonic not in {"hlt", "nop"}
    }
    substantive = {
        form for form in forms if form.split(":", 1)[0].rstrip("bwlq") not in SETUP_MNEMONICS
    }
    return substantive or forms


def collect_k_programs(
    source_root: pathlib.Path,
    temporary: pathlib.Path,
    tools: dict[str, str],
) -> tuple[list[KProgram], list[Exclusion]]:
    programs: list[KProgram] = []
    exclusions: list[Exclusion] = []
    sources = sorted(
        list((source_root / "tests" / "single-instruction-tests").rglob("test.s"))
        + list((source_root / "tests" / "mcsema-unit-tests").rglob("test.s"))
    )
    for source in sources:
        relative = source.relative_to(source_root).as_posix()
        state_path = source.parent / "Output" / "test.kstate"
        if source.stat().st_size == 0:
            exclusions.append(
                Exclusion("k", relative, "upstream test source is empty")
            )
            continue
        try:
            code, instructions, offsets = assemble_program(
                source,
                temporary,
                tools["clang"],
                tools["linker"],
                tools["objcopy"],
                tools["objdump"],
                tools["readelf"],
            )
        except subprocess.CalledProcessError as error:
            stderr = error.stderr.decode() if isinstance(error.stderr, bytes) else error.stderr
            if not state_path.is_file() or state_path.stat().st_size == 0:
                exclusions.append(
                    Exclusion(
                        "k",
                        relative,
                        "upstream source contains invalid x86-64 operand forms",
                    )
                )
                continue
            fail(f"failed to assemble K source {relative}: {(stderr or '').strip()}")
        reason = profile_exclusion(instructions)
        if reason is not None:
            exclusions.append(Exclusion("k", relative, reason))
            continue
        states: list[State] | None = None
        if (state_path.is_file() and state_path.stat().st_size != 0 and
                "<regstatequeue>" in state_path.read_text()):
            state_text = state_path.read_text()
            next_pc_match = re.search(
                r"<nextLocPc>\s*64'(\d+)\s*</nextLocPc>", state_text
            )
            if (next_pc_match is not None and
                    int(next_pc_match.group(1)) == len(instructions)):
                states = parse_k_states(state_path, instructions, offsets)
                if len(states) < 2:
                    fail(f"K trace has no retired instruction: {relative}")
        programs.append(
            KProgram(source, relative, code, instructions, offsets, states)
        )
    return programs, exclusions


def collect_k_records(
    programs: list[KProgram], native: dict[str, list[State]]
) -> list[Record]:
    records: list[Record] = []
    required_native = {
        program.relative for program in programs if needs_native_trace(program)
    }
    if set(native) != required_native:
        missing = sorted(required_native - set(native))
        unexpected = sorted(set(native) - required_native)
        fail(
            "native K response has the wrong source inventory: "
            f"missing={missing[:1]} unexpected={unexpected[:1]}"
        )
    for program in programs:
        states = program.formal_states
        oracle = "K checked trace"
        nan_class_equivalence = program.relative in K_NAN_CLASS_SOURCES
        if states is None:
            states = native[program.relative]
            if len(states) != len(program.instructions) + 1 or any(
                state.rip_index != index for index, state in enumerate(states)
            ):
                fail(f"native K trace has an unexpected path: {program.relative}")
            for state in states:
                state.flags = [None] * len(FLAG_NAMES)
            oracle = "native Intel trace"
            nan_class_equivalence = False
        elif program.relative in NATIVE_OVERRIDE_SOURCES:
            native_trace = native[program.relative]
            if len(native_trace) != len(states) or any(
                actual.rip_index != expected.rip_index
                for actual, expected in zip(native_trace, states)
            ):
                fail(f"native K override has an unexpected path: {program.relative}")
            states = [
                State(
                    actual.rip_index,
                    [
                        actual.gprs[index] if value is not None else None
                        for index, value in enumerate(expected.gprs)
                    ],
                    [
                        actual.vectors[index] if value is not None else None
                        for index, value in enumerate(expected.vectors)
                    ],
                    [
                        actual.flags[index] if value is not None else None
                        for index, value in enumerate(expected.flags)
                    ],
                    expected.expected_event,
                )
                for actual, expected in zip(native_trace, states)
            ]
            oracle = "native Intel trace with K validity masks"
            nan_class_equivalence = (
                program.relative not in EXACT_NATIVE_OVERRIDE_SOURCES
            )
        elif program.relative in ESTIMATE_SOURCES:
            native_trace = native[program.relative]
            if len(native_trace) != len(states) or any(
                actual.rip_index != expected.rip_index
                for actual, expected in zip(native_trace, states)
            ):
                fail(f"native K estimate has an unexpected path: {program.relative}")
            states = [
                State(
                    expected.rip_index,
                    expected.gprs,
                    [
                        estimate_vector(value, actual.vectors[index])
                        if value is not None else None
                        for index, value in enumerate(expected.vectors)
                    ],
                    expected.flags,
                    expected.expected_event,
                )
                for actual, expected in zip(native_trace, states)
            ]
            oracle = "K checked trace with native Intel special estimates"
        record_forms = tuple(sorted(semantic_forms(program.instructions)))
        records.append(
            Record(
                "k",
                program.relative,
                f"k/{pathlib.Path(program.relative).parent.relative_to('tests').as_posix()}",
                program.code,
                encode_k_payload(states, program.offsets),
                len(states) - 1,
                record_forms,
                oracle,
                nan_class_equivalence=nan_class_equivalence,
            )
        )
    return records


def full_precision_config(config: bytes, source: str) -> bytes:
    """Drop FEX's reduced-precision x87 request from one FEX configuration.

    The engine has only full 80-bit x87, and FEX's expected registers are
    already native full-precision results (fex_config explains why).
    """
    try:
        rewritten, requested = fex_config.without_reduced_precision(config)
    except ValueError as error:
        fail(f"{error}: {source}")
    if any(value != b"1" for value in requested):
        fail(f"unexpected FEX reduced-precision setting: {source}")
    return rewritten


def collect_fex_records(
    corpus: pathlib.Path,
    llvm_mc: str,
    expected_commit: str,
) -> tuple[list[Record], list[Exclusion]]:
    metadata = json.loads((corpus / "corpus.json").read_text())
    if (
        metadata.get("schema") != 1
        or metadata.get("mode") != "x86-64"
        or metadata.get("commit") != expected_commit
    ):
        fail("FEX corpus identity does not match the pinned x86-64 input")
    records: list[Record] = []
    exclusions: list[Exclusion] = []
    seen: set[tuple[str, str]] = set()
    for item in metadata.get("tests", []):
        source = str(item["source"])
        if bool(item.get("disabled_upstream", False)):
            exclusions.append(
                Exclusion("fex", source, "upstream marks this source unsupported")
            )
            continue
        listed_failure = bool(item.get("known_failure_host", False))
        if listed_failure != (source in KNOWN_FAILURE_HOST_SOURCES):
            fail(
                "upstream's Known_Failures_host and the producer's "
                f"exclusions disagree about {source}"
            )
        if source in EXCLUDED_FEX_SOURCES:
            exclusions.append(Exclusion("fex", source, EXCLUDED_FEX_SOURCES[source]))
            continue
        features = set(item.get("host_features", []))
        unknown = features - ALLOWED_FEX_FEATURES - EXCLUDED_FEX_FEATURES.keys()
        if unknown:
            fail(f"FEX test has unclassified host features {sorted(unknown)}: {source}")
        blocked = sorted(features & EXCLUDED_FEX_FEATURES.keys())
        if blocked:
            reasons = "; ".join(EXCLUDED_FEX_FEATURES[feature] for feature in blocked)
            exclusions.append(Exclusion("fex", source, reasons))
            continue
        binary_path = corpus / str(item["binary"])
        config_path = corpus / str(item["config"])
        code = binary_path.read_bytes()
        config = config_path.read_bytes()
        if sha256_bytes(code) != item["binary_sha256"] or sha256_bytes(config) != item["config_sha256"]:
            fail(f"FEX corpus hash mismatch: {source}")
        config = full_precision_config(config, source)
        instructions = disassemble_raw(code, llvm_mc)
        reason = profile_exclusion(instructions)
        if reason is not None:
            exclusions.append(Exclusion("fex", source, reason))
            continue
        forms = semantic_forms(instructions)
        identity = (sha256_bytes(code), sha256_bytes(config))
        if identity in seen:
            exclusions.append(Exclusion("fex", source, "exact duplicate normalized case"))
            continue
        seen.add(identity)
        records.append(
            Record(
                "fex",
                source,
                f"fex/{source}",
                code,
                config,
                1,
                tuple(sorted(forms)),
                "FEX final-state contract",
            )
        )
    return records, exclusions


@dataclass(frozen=True)
class RepositoryCase:
    relative: str
    code: bytes
    instructions: list[Instruction]
    regions: tuple[tuple[int, int], ...]
    registers: tuple[tuple[str, tuple[int, ...]], ...]


def register_key(name: str) -> int:
    """FEX's one-bit register key: RIP, the GPRs from RAX, then XMM0-15."""
    if name == "RIP":
        return 1
    if name in GPR_NAMES:
        return 1 << (1 + GPR_NAMES.index(name))
    match = re.fullmatch(r"XMM(\d+)", name)
    if match is not None and int(match.group(1)) < 16:
        return 1 << (17 + int(match.group(1)))
    fail(f"repository cases cannot expect register {name}")


def parse_integer_text(value: object, relative: str) -> int:
    if not isinstance(value, str):
        fail(f"repository case values are hexadecimal strings: {relative}")
    try:
        return int(value, 0)
    except ValueError:
        fail(f"invalid repository case value {value!r}: {relative}")


def case_configuration(
    source: pathlib.Path, relative: str, code_size: int
) -> tuple[tuple[tuple[int, int], ...], tuple[tuple[str, tuple[int, ...]], ...]]:
    """Read the `/* CONFIG ... */` block: FEX's RegData and MemoryRegions."""
    text = source.read_text()
    marker = "/* CONFIG"
    if text.count(marker) != 1:
        fail(f"repository case needs exactly one CONFIG block: {relative}")
    body = text.split(marker, 1)[1].split("*/", 1)[0]
    try:
        config = json.loads(body)
    except json.JSONDecodeError as error:
        fail(f"invalid repository case CONFIG: {relative}: {error}")
    if not isinstance(config, dict) or not set(config) <= {
        "MemoryRegions", "RegData"
    }:
        fail(f"repository case CONFIG takes only MemoryRegions and RegData: {relative}")
    expectations = config.get("RegData")
    if not isinstance(expectations, dict) or not expectations:
        fail(f"repository case expects no registers: {relative}")
    registers: list[tuple[str, tuple[int, ...]]] = []
    for name, value in expectations.items():
        name = name.upper()
        register_key(name)
        if name.startswith("XMM"):
            if not isinstance(value, list) or len(value) not in (2, 4):
                fail(f"{name} expects two or four 64-bit lanes: {relative}")
            values = tuple(parse_integer_text(item, relative) for item in value)
        else:
            values = (parse_integer_text(value, relative),)
        if any(item >> 64 for item in values):
            fail(f"{name} expectation exceeds 64 bits: {relative}")
        registers.append((name, values))
    if len({name for name, _ in registers}) != len(registers):
        fail(f"repository case repeats a register: {relative}")
    regions: list[tuple[int, int]] = []
    code_end = CODE_ADDRESS + ((code_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
    occupied = [(CODE_ADDRESS, code_end)]
    raw_regions = config.get("MemoryRegions", {})
    if not isinstance(raw_regions, dict):
        fail(f"MemoryRegions maps addresses to sizes: {relative}")
    for address_text, size_text in raw_regions.items():
        address = parse_integer_text(address_text, relative)
        size = parse_integer_text(size_text, relative)
        end = address + size
        if (size == 0 or address % PAGE_SIZE or size % PAGE_SIZE or
                end > 1 << 47 or
                any(address < last and first < end for first, last in occupied)):
            fail(f"invalid repository case region {address:#x}+{size:#x}: {relative}")
        occupied.append((address, end))
        regions.append((address, size))
    return tuple(regions), tuple(registers)


def collect_repository_cases(
    directory: pathlib.Path,
    temporary: pathlib.Path,
    tools: dict[str, str],
) -> list[RepositoryCase]:
    cases: list[RepositoryCase] = []
    for source in sorted(directory.glob("*.s")):
        relative = source.relative_to(CORPUS).as_posix()
        try:
            code, instructions, _ = assemble_program(
                source,
                temporary,
                tools["clang"],
                tools["linker"],
                tools["objcopy"],
                tools["objdump"],
                tools["readelf"],
                "repository",
            )
        except subprocess.CalledProcessError as error:
            stderr = error.stderr.decode() if isinstance(error.stderr, bytes) else error.stderr
            fail(f"failed to assemble repository case {relative}: {(stderr or '').strip()}")
        if b"\xf4" not in code:
            fail(f"repository case has no HLT terminator: {relative}")
        regions, registers = case_configuration(source, relative, len(code))
        cases.append(
            RepositoryCase(relative, code, instructions, regions, registers)
        )
    return cases


def case_config(case: RepositoryCase) -> bytes:
    """Encode FEX's configuration record, which the runner already reads."""
    regions = b"".join(struct.pack("<QQ", *region) for region in case.regions)
    registers = b"".join(
        struct.pack("<IQ", len(values), register_key(name)) +
        struct.pack(f"<{len(values)}Q", *values)
        for name, values in case.registers
    )
    header_size = 4 * 8 + 3 * 4 + 4 * 8
    offsets = [
        header_size,
        header_size + len(regions),
        header_size + len(regions) + len(registers),
        header_size + len(regions) + len(registers),
    ]
    header = struct.pack(
        "<QQQQIII8I",
        0,
        0,
        4096,
        1,
        0,
        1,
        0,
        offsets[0], len(case.regions),
        offsets[1], len(case.registers),
        offsets[2], 0,
        offsets[3], 0,
    )
    return header + regions + registers


def repository_request(cases: list[RepositoryCase]) -> bytes:
    records = bytearray()
    for case in cases:
        name = case.relative.encode()
        records.extend(
            struct.pack("<III", len(name), len(case.code), len(case.regions))
        )
        for region in case.regions:
            records.extend(struct.pack("<QQ", *region))
        records.extend(name)
        records.extend(case.code)
    identity = hashlib.sha256(records).digest()
    return CASE_REQUEST_MAGIC + struct.pack("<II", 1, len(cases)) + identity + records


def verify_repository_cases(
    path: pathlib.Path, request: bytes, cases: list[RepositoryCase]
) -> dict[str, str]:
    """Require every declared expectation to be the native final state."""
    data = path.read_bytes()
    header = struct.Struct("<8sII12sI32s")
    if len(data) < header.size:
        fail("native repository response is truncated")
    magic, version, count, vendor, signature, identity = header.unpack_from(data)
    if magic != CASE_RESPONSE_MAGIC or version != 1:
        fail("native repository response has an invalid header")
    if identity != request[16:48] or count != len(cases):
        fail("native repository response does not match the generated request")
    cursor = header.size
    for case in cases:
        name = case.relative.encode()
        if data[cursor:cursor + 4] != struct.pack("<I", len(name)) or \
                data[cursor + 4:cursor + 4 + len(name)] != name:
            fail(f"native repository response is out of order at {case.relative}")
        cursor += 4 + len(name)
        state = data[cursor:cursor + NATIVE_STATE_SIZE]
        if len(state) != NATIVE_STATE_SIZE:
            fail("native repository response is truncated")
        cursor += NATIVE_STATE_SIZE
        event, rip_offset = struct.unpack_from("<II", state)
        gprs = struct.unpack_from("<16Q", state, 8)
        vectors = [state[136 + index * 32:168 + index * 32] for index in range(16)]
        if event != 0 or case.code[rip_offset:rip_offset + 1] != b"\xf4":
            fail(f"native execution did not stop at HLT: {case.relative}")
        for register, values in case.registers:
            if register == "RIP":
                native = (CODE_ADDRESS + rip_offset,)
            elif register in GPR_NAMES:
                native = (gprs[GPR_NAMES.index(register)],)
            else:
                vector = vectors[int(register[3:])]
                native = struct.unpack_from(f"<{len(values)}Q", vector)
            if native != values:
                fail(
                    f"{case.relative}: {register} expected "
                    f"{[hex(value) for value in values]}, native "
                    f"{[hex(value) for value in native]}"
                )
    if cursor != len(data):
        fail("native repository response has trailing bytes")
    return {
        "vendor": vendor.decode("ascii", errors="strict"),
        "cpuid_signature": f"0x{signature:08x}",
        "response_sha256": sha256_file(path),
    }


def collect_repository_records(cases: list[RepositoryCase]) -> list[Record]:
    """A repository case is written for every instruction it runs, so its
    forms are all of them rather than those left after setup moves."""
    return [
        Record(
            "repository",
            case.relative,
            f"repository/{pathlib.Path(case.relative).name}",
            case.code,
            case_config(case),
            1,
            tuple(sorted({
                instruction.form
                for instruction in case.instructions
                if instruction.mnemonic not in {"hlt", "nop"}
            })),
            "native Intel final state",
        )
        for case in cases
    ]


def encode_pack(records: list[Record]) -> bytes:
    output = bytearray(PACK_MAGIC)
    output.extend(struct.pack("<II", PACK_VERSION, len(records)))
    for record in records:
        name = record.name.encode()
        source = record.source.encode()
        if len(name) > 0xFFFF or len(source) > 0xFFFF:
            fail(f"corpus record name is too long: {record.name}")
        kind = PACK_KINDS[record.origin]
        flags = (
            (1 if record.upstream_disabled else 0)
            | (2 if record.nan_class_equivalence else 0)
        )
        output.extend(
            struct.pack(
                "<BBHHHIII",
                kind,
                flags,
                0,
                len(name),
                len(source),
                len(record.code),
                len(record.payload),
                record.case_count,
            )
        )
        output.extend(name)
        output.extend(source)
        output.extend(record.code)
        output.extend(record.payload)
    return bytes(output)


def write_outputs(
    output: pathlib.Path,
    records: list[Record],
    exclusions: list[Exclusion],
    identities: dict[str, object],
    check: bool,
) -> None:
    pack = encode_pack(records)
    manifest_lines = [
        "name\torigin\tsource\toracle\tcases\tcode_sha256\tpayload_sha256\tforms\tupstream_disabled\tnan_comparison"
    ]
    for record in records:
        manifest_lines.append(
            "\t".join(
                (
                    record.name,
                    record.origin,
                    record.source,
                    record.oracle,
                    str(record.case_count),
                    sha256_bytes(record.code),
                    sha256_bytes(record.payload),
                    ",".join(record.forms),
                    "yes" if record.upstream_disabled else "no",
                    "class" if record.nan_class_equivalence else "exact",
                )
            )
        )
    exclusion_lines = ["origin\tsource\treason"] + [
        f"{item.origin}\t{item.source}\t{item.reason}"
        for item in sorted(exclusions, key=lambda item: (item.origin, item.source))
    ]
    form_counts: dict[str, collections.Counter[str]] = collections.defaultdict(collections.Counter)
    for record in records:
        for form in record.forms:
            form_counts[form][record.origin] += record.case_count
    coverage_lines = ["form\tk_cases\tfex_cases\trepository_cases"] + [
        f"{form}\t{form_counts[form]['k']}\t{form_counts[form]['fex']}"
        f"\t{form_counts[form]['repository']}"
        for form in sorted(form_counts)
    ]
    provenance = {
        "schema": 1,
        "profile": "x86-64-v3 plus AES, PCLMUL, XSAVE, FSGSBASE, ADX, and AVX-VNNI",
        "inputs": identities,
        "records": len(records),
        "cases": sum(record.case_count for record in records),
        "k_records": sum(record.origin == "k" for record in records),
        "fex_records": sum(record.origin == "fex" for record in records),
        "repository_records": sum(
            record.origin == "repository" for record in records
        ),
        "excluded_records": len(exclusions),
        "pack_sha256": sha256_bytes(pack),
    }
    generated = {
        "x86_64_v3.pack": pack,
        "manifest.tsv": ("\n".join(manifest_lines) + "\n").encode(),
        "excluded.tsv": ("\n".join(exclusion_lines) + "\n").encode(),
        "coverage.tsv": ("\n".join(coverage_lines) + "\n").encode(),
        "provenance.json": (json.dumps(provenance, indent=2, sort_keys=True) + "\n").encode(),
    }
    if check:
        for name, contents in generated.items():
            path = output / name
            if not path.is_file() or path.read_bytes() != contents:
                fail(f"generated corpus is stale: {path}")
        print(f"verified {provenance['cases']} checked-in x86 instruction cases")
        return
    output.mkdir(parents=True, exist_ok=True)
    for name, contents in generated.items():
        destination = output / name
        temporary = destination.with_suffix(destination.suffix + ".tmp")
        temporary.write_bytes(contents)
        temporary.replace(destination)
    print(
        f"wrote {provenance['cases']} x86 instruction cases "
        f"({provenance['k_records']} K traces, {provenance['fex_records']} FEX programs, "
        f"{provenance['repository_records']} repository cases)"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--k-source", required=True, type=pathlib.Path)
    parser.add_argument("--fex-corpus", type=pathlib.Path)
    parser.add_argument(
        "--native-request",
        type=pathlib.Path,
        help="write the missing/stale K cases for the native Linux/x86 oracle",
    )
    parser.add_argument(
        "--native-response",
        type=pathlib.Path,
        help="consume the response produced by native-k-trace.cpp",
    )
    parser.add_argument(
        "--repository-request",
        type=pathlib.Path,
        help="write the repository cases for the native Linux/x86 oracle",
    )
    parser.add_argument(
        "--repository-response",
        type=pathlib.Path,
        help="consume the repository-case response produced by native-k-trace.cpp",
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=REPOSITORY / ".build/corpora/x86_64",
    )
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    try:
        _, k_commit = manifest_identity("x86-k-semantics")
        _, fex_commit = manifest_identity("fex")
        verify_checkout(arguments.k_source.resolve(), k_commit, "K x86 semantics")
    except ValueError as error:
        fail(str(error))
    tools = {
        "clang": resolve_tool("UML_X86_CORPUS_CLANG", "/opt/homebrew/opt/llvm/bin/clang"),
        "linker": resolve_tool("UML_X86_CORPUS_LINKER", "ld.lld"),
        "objcopy": resolve_tool("UML_X86_CORPUS_OBJCOPY", "/opt/homebrew/opt/llvm/bin/llvm-objcopy"),
        "objdump": resolve_tool("UML_X86_CORPUS_OBJDUMP", "/opt/homebrew/opt/llvm/bin/llvm-objdump"),
        "readelf": resolve_tool("UML_X86_CORPUS_READELF", "/opt/homebrew/opt/llvm/bin/llvm-readelf"),
        "llvm_mc": resolve_tool("UML_X86_CORPUS_LLVM_MC", "/opt/homebrew/opt/llvm/bin/llvm-mc"),
    }
    with tempfile.TemporaryDirectory(prefix="uml-x86-unit-corpus-") as directory:
        programs, exclusions = collect_k_programs(
            arguments.k_source.resolve(), pathlib.Path(directory), tools
        )
        request = native_request(programs)
        cases = collect_repository_cases(
            REPOSITORY_CASES, pathlib.Path(directory), tools
        )
        case_request = repository_request(cases)
        if arguments.native_request is not None or arguments.repository_request is not None:
            if (arguments.native_response is not None or
                    arguments.repository_response is not None or arguments.check):
                fail("a native request cannot be combined with a response or --check")
            if arguments.native_request is not None:
                destination = arguments.native_request.resolve()
                destination.write_bytes(request)
                native_count = sum(
                    needs_native_trace(program) for program in programs
                )
                print(f"wrote {native_count} native K trace requests to {destination}")
            if arguments.repository_request is not None:
                destination = arguments.repository_request.resolve()
                destination.write_bytes(case_request)
                print(f"wrote {len(cases)} repository case requests to {destination}")
            return 0
        if arguments.native_response is None:
            fail("--native-response is required when generating or checking the corpus")
        if arguments.fex_corpus is None:
            fail("--fex-corpus is required when generating or checking the corpus")
        if arguments.repository_response is None:
            fail("--repository-response is required when generating or checking the corpus")
        native, native_identity = native_states(
            arguments.native_response.resolve(), request
        )
        k_records = collect_k_records(programs, native)
        fex_records, fex_exclusions = collect_fex_records(
            arguments.fex_corpus.resolve(), tools["llvm_mc"], fex_commit
        )
        exclusions.extend(fex_exclusions)
        repository_identity = verify_repository_cases(
            arguments.repository_response.resolve(), case_request, cases
        )
        records = sorted(
            k_records + fex_records + collect_repository_records(cases),
            key=lambda item: (item.origin, item.name),
        )
        write_outputs(
            arguments.output.resolve(),
            records,
            exclusions,
            {
                "fex_commit": fex_commit,
                "fex_corpus_sha256": sha256_file(arguments.fex_corpus / "corpus.json"),
                "k_commit": k_commit,
                "native_oracle": native_identity,
                "repository_oracle": repository_identity,
                "sdm": "325462-092, June 2026",
            },
            arguments.check,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
