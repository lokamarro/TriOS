# SPDX-License-Identifier: GPL-2.0-or-later
"""The parallel add and subtract family and SEL of the gadget engine's
AArch32 part, as gadgets that do the lanes with Advanced SIMD on the low
32 bits of a vector register.

generate_a32.py imports this module. The family is S, Q, SH, U, UQ and UH
(signed, signed saturating, signed halving, unsigned, unsigned saturating,
unsigned halving) by ADD16, ASX, SAX, SUB16, ADD8 and SUB8 (plan sub =
mode * 6 + kind). Operand word: d[0:8] n[8:16] m[16:24], byte offsets of
the core registers in CpuState.

  wrapping   S and U keep the low bits of each lane and set GE: a lane's
             flag is its exact signed result >= 0 (S), or the carry out of
             an add and the absence of a borrow in a subtract (U), stored as
             the 0xff / 0x00 byte lanes of CpuState::a32_ge_mask (two bytes
             per halfword lane).
  halving    SH and UH are one instruction; they set no flag.
  saturating Q and UQ set no flag and not the Q flag either, so they are
             not the Advanced SIMD saturating instructions, which would set
             FPSR.QC, a bit the guest reads in FPSCR. The saturation is
             computed with ordinary lane arithmetic.

ASX and SAX exchange the halfwords of Rm first and then add in one lane and
subtract in the other; both lanes are computed and one halfword is inserted
into the other's vector.

SEL takes each byte of Rn where the matching GE byte lane is set and of Rm
elsewhere. Its operand word is the same.
"""

from __future__ import annotations

FAMILIES: list[str] = ["parallel", "sel"]

MODES = ["S", "Q", "SH", "U", "UQ", "UH"]
KINDS = ["ADD16", "ASX", "SAX", "SUB16", "ADD8", "SUB8"]


def plan_of_parallel(mnemonic: str):
    """(family, sub, aux) of a parallel add or subtract or of SEL, or None."""
    if mnemonic == "SEL":
        return ("sel", 0, 0)
    for mode, prefix in enumerate(MODES):
        if mnemonic.startswith(prefix):
            suffix = mnemonic[len(prefix):]
            if suffix in KINDS:
                return ("parallel", mode * 6 + KINDS.index(suffix), 0)
    return None


def mnemonics() -> list[str]:
    return [prefix + suffix for prefix in MODES for suffix in KINDS] + ["SEL"]


def gadget_name(mode: int, kind: int) -> str:
    return f"par_{MODES[mode].lower()}_{KINDS[kind].lower()}"


def lane_arrangement(kind: str) -> str:
    return "8b" if kind.endswith("8") else "4h"


def lane_ops(mode: str, subtract: bool, arrangement: str, result: str,
             flags: str) -> list[str]:
    """Vector instructions that leave the lanes of v0 (Rn) and v1 (Rm) added
    or subtracted in `result`, and the GE lane masks in `flags` for S and U.
    v6 and v7 are scratch."""
    t = arrangement
    wide = "8h" if t == "8b" else "4s"
    signed = mode in ("S", "Q", "SH")
    op = "sub" if subtract else "add"
    if mode in ("SH", "UH"):
        return [f"{'s' if signed else 'u'}h{op} v{result}.{t}, v0.{t}, v1.{t}"]
    lines = [f"{op} v{result}.{t}, v0.{t}, v1.{t}"]
    if mode == "S":
        if subtract:
            lines.append(f"cmge v{flags}.{t}, v0.{t}, v1.{t}")
        else:
            lines += [f"saddl v6.{wide}, v0.{t}, v1.{t}",
                      f"cmge v6.{wide}, v6.{wide}, #0",
                      f"xtn v{flags}.{t}, v6.{wide}"]
    elif mode == "U":
        lines.append(f"cmhs v{flags}.{t}, v0.{t}, v1.{t}" if subtract else
                     f"cmhi v{flags}.{t}, v0.{t}, v{result}.{t}")
    elif mode == "UQ":
        if subtract:
            # A lane that borrows is zero.
            lines += [f"cmhs v6.{t}, v0.{t}, v1.{t}",
                      f"and v{result}.16b, v{result}.16b, v6.16b"]
        else:
            # A lane that carries is all ones.
            lines += [f"cmhi v6.{t}, v0.{t}, v{result}.{t}",
                      f"orr v{result}.16b, v{result}.16b, v6.16b"]
    else:
        # Q: signed overflow of the lane, and the bound on the side of Rn's
        # sign (0x7f.. for a non-negative Rn, 0x80.. for a negative one).
        shift = 7 if t == "8b" else 15
        if subtract:
            lines += ["eor v6.16b, v0.16b, v1.16b",
                      f"eor v7.16b, v0.16b, v{result}.16b",
                      "and v6.16b, v6.16b, v7.16b"]
        else:
            lines += [f"eor v6.16b, v0.16b, v{result}.16b",
                      f"eor v7.16b, v1.16b, v{result}.16b",
                      "and v6.16b, v6.16b, v7.16b"]
        lines += [f"cmlt v6.{t}, v6.{t}, #0",
                  f"sshr v7.{t}, v0.{t}, #{shift}",
                  (f"movi v{flags}.{t}, #0x7f" if t == "8b" else
                   f"mvni v{flags}.{t}, #0x80, lsl #8"),
                  f"eor v7.16b, v7.16b, v{flags}.16b",
                  f"bit v{result}.16b, v7.16b, v6.16b"]
    return lines


def parallel_gadget(mode: int, kind: int) -> str:
    prefix, name = MODES[mode], KINDS[kind]
    t = lane_arrangement(name)
    sets_flags = prefix in ("S", "U")
    lines = ["ldr x8, [_stream], #8", "ubfx x9, x8, #8, #8",
             "ldr w9, [_vcpu, x9]", "ubfx x10, x8, #16, #8",
             "ldr w10, [_vcpu, x10]"]
    if name in ("ASX", "SAX"):
        lines.append("ror w10, w10, #16")
    lines += ["fmov s0, w9", "fmov s1, w10"]
    if name in ("ADD16", "ADD8", "SUB16", "SUB8"):
        subtract = name.startswith("SUB")
        lines += lane_ops(prefix, subtract, t, "2", "4")
        result, flags = "2", "4"
    else:
        # Lane 0 of ASX subtracts and lane 1 adds; SAX is the reverse.
        lines += lane_ops(prefix, False, t, "2", "4")
        lines += lane_ops(prefix, True, t, "3", "5")
        base, other = ("3", "2") if name == "ASX" else ("2", "3")
        flag_base, flag_other = ("5", "4") if name == "ASX" else ("4", "5")
        lines.append(f"mov v{base}.h[1], v{other}.h[1]")
        if sets_flags:
            lines.append(f"mov v{flag_base}.h[1], v{flag_other}.h[1]")
        result, flags = base, flag_base
    lines.append(f"fmov w11, s{result}")
    if sets_flags:
        lines += [f"fmov w12, s{flags}",
                  "str w12, [_vcpu, #UML_ARM64_CPU_A32_GE_MASK]"]
    lines += ["and x8, x8, #0xff", "str w11, [_vcpu, x8]", "next_gadget"]
    return "\n".join(lines)


def sel_gadget() -> str:
    return "\n".join([
        "ldr x8, [_stream], #8", "ubfx x9, x8, #8, #8", "ldr w9, [_vcpu, x9]",
        "ubfx x10, x8, #16, #8", "ldr w10, [_vcpu, x10]",
        "ldr w12, [_vcpu, #UML_ARM64_CPU_A32_GE_MASK]",
        "and w11, w9, w12", "bic w13, w10, w12", "orr w11, w11, w13",
        "and x8, x8, #0xff", "str w11, [_vcpu, x8]", "next_gadget"])


def emit(asm) -> None:
    for mode in range(len(MODES)):
        for kind in range(len(KINDS)):
            asm.gadget(gadget_name(mode, kind), parallel_gadget(mode, kind))
    asm.gadget("sel", sel_gadget())


def header_tables(names: set[str]) -> list[str]:
    def g(name: str) -> str:
        return f"G::{name}" if name in names else "G::count"

    out = ["/* Parallel add and subtract: [S, Q, SH, U, UQ, UH][ADD16, ASX, "
           "SAX, SUB16, ADD8, SUB8]. */",
           "inline constexpr G kParallel[6][6] = {"]
    for mode in range(len(MODES)):
        out.append("\t{" + ", ".join(
            g(gadget_name(mode, kind)) for kind in range(len(KINDS))) + "},")
    out.append("};")
    return out
