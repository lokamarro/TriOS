# SPDX-License-Identifier: GPL-2.0-or-later
"""The VFP and Advanced SIMD part of the gadget engine's AArch32 generator.

generate_a32.py imports this module. It adds, for the forms of the decoder's
`simd` level, their plans (the family, an operation and an aspect the builder,
gadgets/a32/program_builder_a32_*.cpp, lowers each form as), the gadgets
those lowerings use, and the tables that name the gadgets to the builder.

The registers: CpuState::vectors holds the guest's V0-V15, which overlay
D0-D31 and S0-S31 (D(2n) is the low half of V(n), D(2n+1) the high half,
S(2n) and S(2n+1) the halves of D(n)), so a VFP or Advanced SIMD register is
a byte offset in CpuState: UML_GADGET_CPU_V0 + 4 * S, + 8 * D, + 16 * Q. A
gadget reads its operands from CpuState and writes only the bytes the
instruction defines, which is what an AArch32 D or S destination requires.
The offsets in a stream are 16 bits each, packed from the low end:

    binary     d[0:16] n[16:32] m[32:48]       accumulating forms read d too
    unary      d[0:16] m[16:32]
    compare    n[0:16] m[16:32]

VFP instructions run under the guest FPCR (the native FP environment); the
Advanced SIMD floating-point instructions run between the two gadgets that
set the standard FPSCR value and restore the guest's.
"""

from __future__ import annotations

import re

import a32_neon
import a32_structure

# The families this module adds, after generate_a32.py's own.
FAMILIES = [
    "vfp3",       # VADD VSUB VMUL VDIV VNMUL; aux bit 0 = double
    "vfpmac",     # VMLA VMLS VNMLA VNMLS VFMA VFMS VFNMA VFNMS
    "vfp2",       # VABS VNEG VSQRT VMOV (register)
    "vfpcmp",     # VCMP VCMPE (sub bit 0 = E, bit 1 = against zero)
    "vfpimm",     # VMOV (immediate)
    "vfpcvt",     # every VFP conversion
    "vfpcore",    # VMOV between core and extension registers
    "vfpsys",     # VMRS VMSR
    "vfpmem",     # VLDR VSTR
    "vfpblock",   # VLDM VSTM VPUSH VPOP FLDMX FSTMX
    "neonmem",    # VLD1-4 VST1-4
    "neon",       # Advanced SIMD data processing; sub = a32_neon.py group
]

# Mnemonics whose forms are VFP when the encoding is the A2/T2 one (A1/T1 is
# Advanced SIMD), and those that exist only as VFP.
SHARED = {"VADD", "VSUB", "VMUL", "VABS", "VNEG", "VMLA", "VMLS", "VFMA",
          "VFMS"}
VFP_ONLY = {"VDIV", "VNMUL", "VNMLA", "VNMLS", "VFNMA", "VFNMS", "VSQRT",
            "VCMP", "VCMPE", "VCVTR", "VCVTB", "VCVTT", "VLDR", "VSTR",
            "VLDM", "VLDMDB", "VSTM", "VSTMDB", "FLDMDBX", "FLDMIAX",
            "FSTMDBX", "FSTMIAX", "VMRS", "VMSR"}

VFP3 = {"VADD": 0, "VSUB": 1, "VMUL": 2, "VDIV": 3, "VNMUL": 4}
VFP3_NAMES = ["vadd", "vsub", "vmul", "vdiv", "vnmul"]
VFPMAC = {"VMLA": 0, "VMLS": 1, "VNMLA": 2, "VNMLS": 3, "VFMA": 4, "VFMS": 5,
          "VFNMA": 6, "VFNMS": 7}
VFPMAC_NAMES = ["vmla", "vmls", "vnmla", "vnmls", "vfma", "vfms", "vfnma",
                "vfnms"]
VFP2 = {"VABS": 0, "VNEG": 1, "VSQRT": 2}
VFP2_NAMES = ["vabs", "vneg", "vsqrt", "vmov"]

# vfpcvt's operations.
CVT_NAMES = ["ds", "sd", "from_int", "to_int", "to_int_r", "fixed", "half"]
CVT_DS, CVT_SD, CVT_FROM_INT, CVT_TO_INT, CVT_TO_INT_R, CVT_FIXED, \
    CVT_HALF = range(7)

# vfpcore's operations.
CORE_NAMES = ["tos", "s", "toss", "ss", "tod", "d", "rs", "sr"]


def is_vfp(name: str, mnemonic: str) -> bool:
    if mnemonic in VFP_ONLY:
        return True
    if mnemonic in SHARED:
        return "_A2_" in name or "_T2_" in name
    return False


def plan_of_simd(name: str, mnemonic: str):
    """(family, sub, aux) for a `simd` form, or None when the form is not
    lowered yet."""
    double = 1 if name.endswith("_D") else 0
    if is_vfp(name, mnemonic):
        if mnemonic in VFP3:
            return ("vfp3", VFP3[mnemonic], double)
        if mnemonic in VFPMAC:
            return ("vfpmac", VFPMAC[mnemonic], double)
        if mnemonic in VFP2:
            return ("vfp2", VFP2[mnemonic], double)
        if mnemonic in ("VCMP", "VCMPE"):
            zero = 2 if "_A2_" in name or "_T2_" in name else 0
            return ("vfpcmp", (1 if mnemonic == "VCMPE" else 0) | zero,
                    double)
        if mnemonic == "VCVTR":
            return ("vfpcvt", CVT_TO_INT_R, double)
        if mnemonic in ("VCVTB", "VCVTT"):
            return ("vfpcvt", CVT_HALF, 1 if mnemonic == "VCVTT" else 0)
        if mnemonic == "VLDR":
            return ("vfpmem", 0, double | (2 if "_l_" in name else 0))
        if mnemonic == "VSTR":
            return ("vfpmem", 1, double)
        if mnemonic in ("VLDM", "VLDMDB", "FLDMDBX", "FLDMIAX"):
            return ("vfpblock", 0, 0)
        if mnemonic in ("VSTM", "VSTMDB", "FSTMDBX", "FSTMIAX"):
            return ("vfpblock", 1, 0)
        if mnemonic == "VMRS":
            return ("vfpsys", 0, 0)
        if mnemonic == "VMSR":
            return ("vfpsys", 1, 0)
    if mnemonic == "VMOV" and ("_r_A2_" in name or "_r_T2_" in name):
        return ("vfp2", 3, double)
    if mnemonic == "VMOV" and ("_i_A2_" in name or "_i_T2_" in name):
        return ("vfpimm", 0, double)
    if mnemonic == "VCVT":
        if name.startswith(("VCVT_ds_", "VCVT_sd_")):
            return ("vfpcvt", CVT_DS if name.startswith("VCVT_ds_") else
                    CVT_SD, 0)
        if name.startswith("VCVT_vi_"):
            return ("vfpcvt", CVT_FROM_INT, double)
        if name.startswith(("VCVT_siv_", "VCVT_uiv_")):
            return ("vfpcvt", CVT_TO_INT, double)
        if name.startswith(("VCVT_toxv_", "VCVT_xv_")):
            return ("vfpcvt", CVT_FIXED, double)
    if mnemonic == "VMOV":
        core = {"VMOV_tos": 0, "VMOV_s": 1, "VMOV_toss": 2, "VMOV_ss": 3,
                "VMOV_tod": 4, "VMOV_d": 5, "VMOV_rs": 6, "VMOV_sr": 7}
        stem = name.rsplit("_", 1)[0]
        if stem in core:
            return ("vfpcore", core[stem], 0)
    structure = re.match(r"V(LD|ST)([1-4])_([m1a])_", name)
    if structure:
        # sub: bit 0 store, bits 1-2 the shape (m, 1, a), bits 3-4 structures - 1
        shape = "m1a".index(structure.group(3))
        return ("neonmem", (1 if structure.group(1) == "ST" else 0) |
                shape << 1 | (int(structure.group(2)) - 1) << 3, 0)
    if re.match(r"V(RECPS|RSQRTS)_", name):
        return ("library", 0, 0)
    neon = a32_neon.plan_of(name)
    if neon is not None:
        return ("neon", neon[0], neon[1])
    return None


# ---------------------------------------------------------------------------
# Gadgets

V0 = "UML_GADGET_CPU_V0"

# Operand fields of the word in x8, each a 16-bit CpuState offset.
def unpack(*fields: str) -> list[str]:
    """Registers named after the fields: d in x8 itself, n in x9, m in x10,
    a in x11 (the offsets in the word's four 16-bit lanes, in the order
    d n m a)."""
    lines = []
    lanes = {"d": 0, "n": 1, "m": 2, "a": 3}
    registers = {"d": "x8", "n": "x9", "m": "x10", "a": "x11"}
    for field in fields:
        if field == "d":
            continue
        lines.append(f"ubfx {registers[field]}, x8, #{16 * lanes[field]}, #16")
    if "d" in fields:
        lines.append("and x8, x8, #0xffff")
    return lines


def stored(width: str) -> str:
    """The register a result is stored from: a half-precision result
    zero-extends into its S register, so it is stored whole."""
    return "s" if width == "h" else width


def vfp_binary(asm, width: str, name: str, operation: str) -> None:
    r = width
    body = [f"ldr {r}0, [_vcpu, x9]",
            f"ldr {r}1, [_vcpu, x10]",
            f"{operation} {r}0, {r}0, {r}1",
            f"str {stored(r)}0, [_vcpu, x8]",
            "next_gadget"]
    head = ["ldr x8, [_stream], #8", *unpack("n", "m", "d")]
    if width == "s" and name in ("vadd", "vsub", "vmul"):
        # The result is the same under the Standard value as under the
        # guest's FPCR while its rounding mode is round to nearest and no
        # nonzero operand is outside 2**-63 to 2**65, which keeps a sum, a
        # difference and a product normal (the "range" guard of a32_neon.py,
        # on the exponent field of each operand). A loop that alternates
        # with Advanced SIMD floating point then switches only for the
        # operands outside it.
        def good(offset: str, value: str, shifted: str, label: str) -> list:
            return [f"ldr {value}, [_vcpu, {offset}]",
                    f"lsl {shifted}, {value}, #1",
                    f"cbz {shifted}, {label}f",
                    f"eor {shifted}, {shifted}, {shifted}, lsl #1",
                    f"tbz {shifted}, #31, 21f",
                    f"{label}:"]
        asm.gadget(f"vfp_{name}_{width}", "\n".join(
            head + [
                "ldrb w6, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]", "cbz w6, 21f",
                "ldrb w6, [_vcpu, #UML_ARM64_CPU_A32_FP_STANDARD]",
                "cbz w6, 22f",
                "ldr w11, [_vcpu, #UML_GADGET_CPU_FPCR]",
                "tst w11, #0xc00000", "b.ne 21f"]
            + good("x9", "w11", "w12", "23") + good("x10", "w13", "w14", "24")
            + ["22:"] + body
            + ["21:", "a32_fp_guest"] + body))
        return
    asm.gadget(f"vfp_{name}_{width}", "\n".join(head + ["a32_fp_guest"] + body))


def vfp_mac(asm, width: str, name: str, body: list[str]) -> None:
    r = width
    asm.gadget(f"vfp_{name}_{width}", "\n".join([
        "ldr x8, [_stream], #8",
        *unpack("n", "m", "d"),
        "a32_fp_guest",
        f"ldr {r}0, [_vcpu, x9]",
        f"ldr {r}1, [_vcpu, x10]",
        f"ldr {r}3, [_vcpu, x8]",
        *[line.replace("%", r) for line in body],
        f"str {stored(r)}3, [_vcpu, x8]",
        "next_gadget"]))


def vfp_unary(asm, width: str, name: str, operation: str | None,
              environment: bool) -> None:
    r = width
    lines = ["ldr x8, [_stream], #8", *unpack("d", "n")]
    # unary operands: d[0:16] m[16:32]; unpack names the second lane "n".
    if environment:
        lines.append("a32_fp_guest")
    lines.append(f"ldr {r}0, [_vcpu, x9]")
    if operation is not None:
        lines.append(f"{operation} {r}0, {r}0")
    lines += [f"str {stored(r)}0, [_vcpu, x8]", "next_gadget"]
    asm.gadget(f"vfp_{name}_{width}", "\n".join(lines))


def vfp_compare(asm, width: str, signaling: bool, zero: bool,
                apsr: bool = False) -> None:
    """VCMP and VCMPE into FPSCR.NZCV; with `apsr`, fused with the
    VMRS APSR_nzcv, FPSCR after it, which copies the flags to NZCV."""
    r = width
    instruction = "fcmpe" if signaling else "fcmp"
    lines = ["ldr x8, [_stream], #8", "ubfx x9, x8, #16, #16",
             "and x8, x8, #0xffff", "a32_fp_guest",
             f"ldr {r}0, [_vcpu, x8]"]
    if zero:
        lines.append(f"{instruction} {r}0, #0.0")
    else:
        lines += [f"ldr {r}1, [_vcpu, x9]", f"{instruction} {r}0, {r}1"]
    lines += ["mrs x10, nzcv", "and w10, w10, #0xf0000000",
              "str w10, [_vcpu, #UML_ARM64_CPU_A32_FPSCR_NZCV]"]
    if apsr:
        lines.append("mov w27, w10")
    lines.append("next_gadget")
    suffix = ("e" if signaling else "") + ("z" if zero else "")
    asm.gadget(f"vfp_vcmp{suffix}_{width}" + ("_apsr" if apsr else ""),
               "\n".join(lines))


def emit_move_select(asm) -> None:
    """A conditional VMOV between S or D registers as a select of the
    source and the destination's old value, as generate_a32.py's
    conditional data processing does: the word is d[0:16] m[16:32] and the
    condition's truth table over NZCV in [32:48]. After every other gadget of
    the part so that the bank before them keeps its layout."""
    for r in "sd":
        asm.gadget(f"vfp_vmov_{r}_select", "\n".join([
            "ldr x8, [_stream], #8", "ubfx x9, x8, #16, #16",
            "and x10, x8, #0xffff", "ldr " + r + "0, [_vcpu, x9]",
            "ldr " + r + "1, [_vcpu, x10]", "lsr w14, w27, #28",
            "lsr x15, x8, #32", "lsr x15, x15, x14", "tst x15, #1",
            f"fcsel {r}0, {r}0, {r}1, ne", f"str {r}0, [_vcpu, x10]",
            "next_gadget"]))


def emit_compare_apsr(asm) -> None:
    """The compares fused with VMRS APSR_nzcv, after every other gadget of
    the part so that the bank before them keeps its layout."""
    for width in "sdh":
        for signaling in (False, True):
            for zero in (False, True):
                vfp_compare(asm, width, signaling, zero, apsr=True)


def emit_vfp_arithmetic(asm) -> None:
    for width in ("s", "d", "h"):
        for name, op in (("vadd", "fadd"), ("vsub", "fsub"),
                         ("vmul", "fmul"), ("vdiv", "fdiv"),
                         ("vnmul", "fnmul")):
            vfp_binary(asm, width, name, op)
        # Sd is the accumulator in register 3; the product is register 2.
        vfp_mac(asm, width, "vmla",
                ["fmul %2, %0, %1", "fadd %3, %3, %2"])
        vfp_mac(asm, width, "vmls",
                ["fmul %2, %0, %1", "fneg %2, %2", "fadd %3, %3, %2"])
        vfp_mac(asm, width, "vnmla",
                ["fmul %2, %0, %1", "fneg %2, %2", "fneg %3, %3",
                 "fadd %3, %3, %2"])
        vfp_mac(asm, width, "vnmls",
                ["fmul %2, %0, %1", "fneg %3, %3", "fadd %3, %3, %2"])
        vfp_mac(asm, width, "vfma", ["fmadd %3, %0, %1, %3"])
        vfp_mac(asm, width, "vfms", ["fmsub %3, %0, %1, %3"])
        vfp_mac(asm, width, "vfnma", ["fnmadd %3, %0, %1, %3"])
        vfp_mac(asm, width, "vfnms", ["fnmsub %3, %0, %1, %3"])
        vfp_unary(asm, width, "vabs", "fabs", False)
        vfp_unary(asm, width, "vneg", "fneg", False)
        vfp_unary(asm, width, "vsqrt", "fsqrt", True)
        vfp_unary(asm, width, "vmov", None, False)
        for signaling in (False, True):
            for zero in (False, True):
                vfp_compare(asm, width, signaling, zero)


def emit_vfp_immediate(asm) -> None:
    # [d offset][immediate]
    asm.gadget("vfp_vmov_imm_s", "ldp x8, x9, [_stream], #16\n"
               "str w9, [_vcpu, x8]\nnext_gadget")
    asm.gadget("vfp_vmov_imm_d", "ldp x8, x9, [_stream], #16\n"
               "str x9, [_vcpu, x8]\nnext_gadget")


def emit_vfp_convert(asm) -> None:
    for width in ("s", "d", "h"):
        r = width
        for sign, signed_name in (("s", "scvtf"), ("u", "ucvtf")):
            # integer in Sm, to a float of `width`
            if width == "h":
                asm.gadget(f"vfp_cvt_from_{sign}_{width}", "\n".join([
                    "ldr x8, [_stream], #8", *unpack("d", "n"),
                    "a32_fp_guest",
                    "ldr w9, [_vcpu, x9]", f"{signed_name} {r}0, w9",
                    f"str {stored(r)}0, [_vcpu, x8]", "next_gadget"]))
            else:
                # An integer to a single or double is the same in either
                # native environment unless the guest rounds other than to
                # nearest (the result is never a denormal, and no NaN is
                # involved), so it runs in whichever is active, and in the
                # guest's only for the other rounding modes.
                asm.gadget(f"vfp_cvt_from_{sign}_{width}", "\n".join([
                    "ldr x8, [_stream], #8", *unpack("d", "n"),
                    "ldrb w6, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]",
                    "cbz w6, 21f",
                    "ldr w10, [_vcpu, #UML_GADGET_CPU_FPCR]",
                    "tst w10, #0xc00000", "b.ne 21f",
                    "ldr w9, [_vcpu, x9]", f"{signed_name} {r}0, w9",
                    f"str {stored(r)}0, [_vcpu, x8]", "next_gadget",
                    "21:", "a32_fp_guest",
                    "ldr w9, [_vcpu, x9]", f"{signed_name} {r}0, w9",
                    f"str {stored(r)}0, [_vcpu, x8]", "next_gadget"]))
            to = "fcvtzs" if sign == "s" else "fcvtzu"
            if width == "h":
                asm.gadget(f"vfp_cvt_to_{sign}_{width}", "\n".join([
                    "ldr x8, [_stream], #8", *unpack("d", "n"),
                    "a32_fp_guest",
                    f"ldr {r}0, [_vcpu, x9]", f"{to} w9, {r}0",
                    "str w9, [_vcpu, x8]", "next_gadget"]))
            else:
                # A conversion to an integer always rounds toward zero, so
                # its result and flags are the same in either native
                # environment unless the input is a denormal, which flush to
                # zero turns into an exact zero (Input Denormal, not
                # Inexact): that case, and an inactive environment, take
                # the guest's. The clamp-and-convert idiom of image code
                # puts this between Advanced SIMD VMAX and VMIN, which run
                # in the other environment.
                if width == "s":
                    test = ["ldr w10, [_vcpu, x9]", "and w11, w10, #0x7fffffff",
                            "sub w11, w11, #1", "mov w12, #0x7fffff",
                            "cmp w11, w12", "b.lo 21f", "fmov s0, w10"]
                else:
                    test = ["ldr x10, [_vcpu, x9]",
                            "and x11, x10, #0x7fffffffffffffff",
                            "sub x11, x11, #1", "mov x12, #0xfffffffffffff",
                            "cmp x11, x12", "b.lo 21f", "fmov d0, x10"]
                asm.gadget(f"vfp_cvt_to_{sign}_{width}", "\n".join([
                    "ldr x8, [_stream], #8", *unpack("d", "n"),
                    "ldrb w6, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]",
                    "cbz w6, 21f", *test,
                    f"{to} w9, {r}0", "str w9, [_vcpu, x8]", "next_gadget",
                    "21:", "a32_fp_guest",
                    f"ldr {r}0, [_vcpu, x9]", f"{to} w9, {r}0",
                    "str w9, [_vcpu, x8]", "next_gadget"]))
            # VCVTR: one rounding, by the FPSCR mode (N, P, M, Z), so the
            # flags are those of a single conversion: an overflow raises
            # Invalid alone and an inexact result Inexact alone.
            modes = [("n", "fcvtn"), ("p", "fcvtp"), ("m", "fcvtm"),
                     ("z", "fcvtz")]
            suffix = "s" if sign == "s" else "u"
            asm.gadget(f"vfp_cvt_tor_{sign}_{width}", "\n".join([
                "ldr x8, [_stream], #8", *unpack("d", "n"),
                "a32_fp_guest",
                f"ldr {r}0, [_vcpu, x9]",
                "ldr w10, [_vcpu, #UML_GADGET_CPU_FPCR]",
                "ubfx w10, w10, #22, #2",
                "cbz w10, 11f", "cmp w10, #1", "b.eq 12f",
                "cmp w10, #2", "b.eq 13f",
                f"{to} w9, {r}0", "b 14f",
                f"11:", f"{modes[0][1]}{suffix} w9, {r}0", "b 14f",
                f"12:", f"{modes[1][1]}{suffix} w9, {r}0", "b 14f",
                f"13:", f"{modes[2][1]}{suffix} w9, {r}0",
                "14:", "str w9, [_vcpu, x8]", "next_gadget"]))
    # single <-> double
    asm.gadget("vfp_cvt_ds", "\n".join([
        "ldr x8, [_stream], #8", *unpack("d", "n"),
        "a32_fp_guest", "ldr s0, [_vcpu, x9]", "fcvt d0, s0",
        "str d0, [_vcpu, x8]", "next_gadget"]))
    asm.gadget("vfp_cvt_sd", "\n".join([
        "ldr x8, [_stream], #8", *unpack("d", "n"),
        "a32_fp_guest", "ldr d0, [_vcpu, x9]", "fcvt s0, d0",
        "str s0, [_vcpu, x8]", "next_gadget"]))
    # half precision, in the bottom (B) or top (T) half of a single register:
    # the offsets of the half are the builder's (+2 for the top).
    asm.gadget("vfp_cvt_sh", "\n".join([
        "ldr x8, [_stream], #8", *unpack("d", "n"),
        "a32_fp_guest", "ldr h0, [_vcpu, x9]", "fcvt s0, h0",
        "str s0, [_vcpu, x8]", "next_gadget"]))
    asm.gadget("vfp_cvt_hs", "\n".join([
        "ldr x8, [_stream], #8", *unpack("d", "n"),
        "a32_fp_guest", "ldr s0, [_vcpu, x9]", "fcvt h0, s0",
        "str h0, [_vcpu, x8]", "next_gadget"]))
    # fixed point: [offset][fbits at bit 16], fbits from 0 to 32; the register
    # is both the source and the destination. A conversion with an immediate
    # fraction width is selected through a table of the 33 widths, two
    # instructions each. To fixed point rounds toward zero and fills the
    # whole register with the sign- or zero-extended result; from fixed
    # point rounds to nearest whatever the FPSCR says.
    for width in ("s", "d", "h"):
        r = width
        for sign in ("s", "u"):
            to = "fcvtzs" if sign == "s" else "fcvtzu"
            fr = "scvtf" if sign == "s" else "ucvtf"
            for size in (32, 16):
                table = "\n".join(
                    [f"{to} w9, {r}0\nb 3f"] +
                    [f"{to} w9, {r}0, #{k}\nb 3f" for k in range(1, 33)])
                body = [
                    "ldr x8, [_stream], #8",
                    "ubfx x13, x8, #16, #8", "and x8, x8, #0xffff",
                    "a32_fp_guest",
                    f"ldr {r}0, [_vcpu, x8]",
                ]
                if size == 16:
                    body.append("mrs x14, fpsr")
                body += ["adr x12, 2f", "add x12, x12, x13, lsl #3", "br x12",
                         "2:", table, "3:"]
                if size == 16:
                    if sign == "s":
                        body += ["mov w10, #32767", "cmp w9, w10",
                                 "b.gt 4f", "mov w10, #-32768",
                                 "cmp w9, w10", "b.ge 5f", "4:",
                                 "orr x14, x14, #1", "msr fpsr, x14",
                                 "csel w9, w10, w9, lt",
                                 "mov w11, #32767",
                                 "cmp w9, w11", "csel w9, w11, w9, gt",
                                 "5:"]
                    else:
                        body += ["mov w10, #65535", "cmp w9, w10", "b.ls 5f",
                                 "orr x14, x14, #1", "msr fpsr, x14",
                                 "mov w9, w10", "5:"]
                if width == "d":
                    if sign == "s":
                        body.append("sxtw x9, w9")
                    body.append("str x9, [_vcpu, x8]")
                else:
                    body.append("str w9, [_vcpu, x8]")
                body.append("next_gadget")
                asm.gadget(f"vfp_cvt_tofix_{sign}{size}_{width}",
                           "\n".join(body))
                table = "\n".join(
                    [f"{fr} {r}0, w9\nb 3f"] +
                    [f"{fr} {r}0, w9, #{k}\nb 3f" for k in range(1, 33)])
                body = [
                    "ldr x8, [_stream], #8",
                    "ubfx x13, x8, #16, #8", "and x8, x8, #0xffff",
                    "a32_fp_guest",
                    ("ldr w9, [_vcpu, x8]" if size == 32 else
                     ("ldrsh w9, [_vcpu, x8]" if sign == "s" else
                      "ldrh w9, [_vcpu, x8]")),
                    "mrs x14, fpcr", "bic x15, x14, #0xc00000",
                    "msr fpcr, x15",
                    "adr x12, 2f", "add x12, x12, x13, lsl #3", "br x12",
                    "2:", table, "3:", "msr fpcr, x14",
                    f"str {stored(r)}0, [_vcpu, x8]", "next_gadget"]
                asm.gadget(f"vfp_cvt_fromfix_{sign}{size}_{width}",
                           "\n".join(body))


def emit_vfp_core(asm) -> None:
    # [d offset][t offset]: a single word in each direction
    asm.gadget("vfp_mov_w", "\n".join([
        "ldr x8, [_stream], #8", *unpack("d", "n"),
        "ldr w9, [_vcpu, x9]", "str w9, [_vcpu, x8]", "next_gadget"]))
    # two words, adjacent in CpuState: [d][t][t2] core to extension
    asm.gadget("vfp_mov_core_pair_in", "\n".join([
        "ldr x8, [_stream], #8", *unpack("d", "n", "m"),
        "ldr w9, [_vcpu, x9]", "ldr w10, [_vcpu, x10]",
        "add x8, _vcpu, x8", "stp w9, w10, [x8]", "next_gadget"]))
    # [t][t2][s]: extension to core
    asm.gadget("vfp_mov_core_pair_out", "\n".join([
        "ldr x8, [_stream], #8", *unpack("d", "n", "m"),
        "add x10, _vcpu, x10", "ldp w11, w12, [x10]",
        "str w11, [_vcpu, x8]", "str w12, [_vcpu, x9]", "next_gadget"]))
    # scalar lanes: core to lane [lane][t], lane to core [t][lane]
    for suffix, store in (("b", "strb"), ("h", "strh"), ("w", "str")):
        asm.gadget(f"vfp_mov_to_lane_{suffix}", "\n".join([
            "ldr x8, [_stream], #8", *unpack("d", "n"),
            "ldr w9, [_vcpu, x9]", f"{store} w9, [_vcpu, x8]", "next_gadget"]))
    for suffix, load in (("ub", "ldrb"), ("uh", "ldrh"), ("w", "ldr"),
                         ("sb", "ldrsb"), ("sh", "ldrsh")):
        asm.gadget(f"vfp_mov_from_lane_{suffix}", "\n".join([
            "ldr x8, [_stream], #8", *unpack("d", "n"),
            f"{load} w9, [_vcpu, x9]", "str w9, [_vcpu, x8]", "next_gadget"]))


FPSCR_NZCV = "0xf0000000"


def emit_vfp_system(asm) -> None:
    # VMRS Rt, FPSCR: control from CpuState (the guest's FPCR), the cumulative
    # status from the live FPSR while the native environment is active, and
    # FPSCR.NZCV from CpuState. [t offset]
    asm.gadget("vfp_vmrs", "\n".join([
        "ldr x8, [_stream], #8",
        "ldrb w11, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]",
        "cbz w11, 1f", "mrs x9, fpsr", "b 2f",
        "1:", "ldr w9, [_vcpu, #UML_GADGET_CPU_FPSR]",
        "2:", "mov w10, #(UML_ARM64_FPSR_WRITABLE & 0xffff)",
        "movk w10, #(UML_ARM64_FPSR_WRITABLE >> 16), lsl #16",
        "and w9, w9, w10",
        "ldr w10, [_vcpu, #UML_GADGET_CPU_FPCR]",
        "movz w11, #0x07c8, lsl #16", "and w10, w10, w11",
        "orr w9, w9, w10",
        "ldr w10, [_vcpu, #UML_ARM64_CPU_A32_FPSCR_NZCV]",
        f"and w10, w10, #{FPSCR_NZCV}", "orr w9, w9, w10",
        "str w9, [_vcpu, x8]", "next_gadget"]))
    # VMRS APSR_nzcv, FPSCR
    asm.gadget("vfp_vmrs_apsr", "\n".join([
        "ldr w9, [_vcpu, #UML_ARM64_CPU_A32_FPSCR_NZCV]",
        f"and w27, w9, #{FPSCR_NZCV}", "next_gadget"]))
    # VMSR FPSCR, Rt: the control bits the profile has (the trap enables,
    # LEN and STRIDE read as zero and ignore writes) go to CpuState and the
    # live FPCR; the status bits to CpuState and the live FPSR; the flags to
    # CpuState. [t offset]
    asm.gadget("vfp_vmsr", "\n".join([
        "ldr x8, [_stream], #8", "ldr w9, [_vcpu, x8]",
        f"and w10, w9, #{FPSCR_NZCV}",
        "str w10, [_vcpu, #UML_ARM64_CPU_A32_FPSCR_NZCV]",
        "movz w10, #0x07c8, lsl #16", "and w10, w9, w10",
        "str w10, [_vcpu, #UML_GADGET_CPU_FPCR]",
        "mov w11, #(UML_ARM64_FPSR_WRITABLE & 0xffff)",
        "movk w11, #(UML_ARM64_FPSR_WRITABLE >> 16), lsl #16",
        "and w11, w9, w11", "str w11, [_vcpu, #UML_GADGET_CPU_FPSR]",
        "ldrb w12, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]", "cbz w12, 1f",
        "msr fpcr, x10", "msr fpsr, x11",
        "strb wzr, [_vcpu, #UML_ARM64_CPU_A32_FP_STANDARD]", "1:",
        "next_gadget"]))


def memory_prologue(label: str, mode: str, mask: int = 3) -> list[str]:
    """[word]: the vector register offset in bits 0-15, the position at bit
    24 (a32_layout.h) and, for a base register, its offset in bits 16-23 and
    the signed displacement in bits 32-63, or for an absolute address the
    address in bits 32-63. The address is left in w7."""
    lines = ["ldr x19, [_stream], #8"]
    if mode == "abs":
        lines.append("lsr x7, x19, #32")
    else:
        lines += ["ubfx x9, x19, #16, #8", "ldr w9, [_vcpu, x9]",
                  "lsr x10, x19, #32", "add w7, w9, w10"]
    lines += [f"tst w7, #{mask}", f"b.ne Lalign_{label}"]
    return lines


def vfp_memory(asm, load: bool, width: str, mode: str) -> None:
    size = {"d": 8, "s": 4, "h": 2}[width]
    name = f"vfp_{'ldr' if load else 'str'}_{width}_{mode}"
    direction = 0 if load else 1
    page = "UML_GADGET_TLB_ENTRY_READ_PAGE" if load else \
        "UML_GADGET_TLB_ENTRY_WRITE_PAGE"
    # A doubleword needs only word alignment; a halfword, halfword alignment.
    lines = memory_prologue(name, mode, 1 if size == 2 else 3)
    translate = (f"{direction}, {page}, {size}, x7, Lcold_{name},"
                 f" Lresume_{name}, Lcross_{name}, 0,"
                 f" fault=La32_position_fault")
    lines.append("a32_translate " + translate)
    lines.append("and x14, x19, #0xffff")
    if load:
        lines += [f"ldr {width}0, [x7]",
                  f"str {stored(width)}0, [_vcpu, x14]"]
    else:
        lines += [f"ldr {width}0, [_vcpu, x14]", f"str {width}0, [x7]"]
    lines.append("next_gadget")
    lines.append("a32_translate_cold " + translate)
    lines.append(f"Lcross_{name}:")
    if load:
        lines += [f"transfer_slow 0, x7, {size}, fault=La32_position_fault",
                  "and x14, x19, #0xffff",
                  f"str {stored(width)}0, [_vcpu, x14]"]
    else:
        lines += ["and x14, x19, #0xffff", f"ldr {width}0, [_vcpu, x14]",
                  f"transfer_slow 1, x7, {size}, fault=La32_position_fault"]
    lines.append("next_gadget")
    lines += [f"Lalign_{name}:", "b La32_position_alignment_fault_" +
              ("read" if load else "write")]
    asm.gadget(name, "\n".join(lines))


def block_copy(bytes_: int, source: str, destination: str) -> list[str]:
    """Copy `bytes_` bytes (a multiple of four) between two base registers,
    in at most 32-byte steps."""
    lines = []
    offset = 0
    remaining = bytes_
    while remaining >= 32:
        lines += [f"ldp q0, q1, [{source}, #{offset}]",
                  f"stp q0, q1, [{destination}, #{offset}]"]
        offset += 32
        remaining -= 32
    if remaining >= 16:
        lines += [f"ldr q0, [{source}, #{offset}]",
                  f"str q0, [{destination}, #{offset}]"]
        offset += 16
        remaining -= 16
    if remaining >= 8:
        lines += [f"ldr d0, [{source}, #{offset}]",
                  f"str d0, [{destination}, #{offset}]"]
        offset += 8
        remaining -= 8
    if remaining >= 4:
        lines += [f"ldr s0, [{source}, #{offset}]",
                  f"str s0, [{destination}, #{offset}]"]
    return lines


def vfp_block(asm, load: bool, bytes_: int) -> None:
    """VLDM and VSTM of `bytes_` bytes. [key][word][library operand]: Rn's
    offset in bits 0-14, writeback in bit 15, the signed displacement from Rn
    of the first address in bits 16-31 and of the written-back Rn in bits
    32-47, and the offset of the first register in bits 48-63. A transfer
    that crosses a page is the semantic library's, run as the whole
    instruction through the operand the stream ends with."""
    name = f"vfp_{'ldm' if load else 'stm'}_{bytes_}"
    direction = 0 if load else 1
    page = "UML_GADGET_TLB_ENTRY_READ_PAGE" if load else \
        "UML_GADGET_TLB_ENTRY_WRITE_PAGE"
    translate = (f"{direction}, {page}, {bytes_}, x7, Lcold_{name},"
                 f" Lresume_{name}, Lcross_{name}, 0,"
                 f" fault=La32_memory_fault")
    lines = ["ldp x21, x19, [_stream], #16",
             "and x9, x19, #0x7fff", "ldr w9, [_vcpu, x9]",
             "sbfx x10, x19, #16, #16", "add w7, w9, w10",
             "sbfx x11, x19, #32, #16", "add w22, w9, w11",
             "tst w7, #3", f"b.ne Lalign_{name}",
             "a32_translate " + translate,
             "ubfx x13, x19, #48, #16", "add x13, _vcpu, x13"]
    if load:
        lines += block_copy(bytes_, "x7", "x13")
    else:
        lines += block_copy(bytes_, "x13", "x7")
    lines += ["tbz x19, #15, 1f", "and x9, x19, #0x7fff",
              "str w22, [_vcpu, x9]", "1:", "add _stream, _stream, #8",
              "next_gadget",
              "a32_translate_cold " + translate,
              f"Lcross_{name}:", "ldr x8, [_stream], #8",
              "a32_library_call", "next_gadget",
              f"Lalign_{name}:",
              "b La32_alignment_fault_" + ("read" if load else "write")]
    asm.gadget(name, "\n".join(lines))


def emit_vfp_memory(asm) -> None:
    for width in ("s", "d", "h"):
        for mode in ("i", "abs"):
            vfp_memory(asm, True, width, mode)
        vfp_memory(asm, False, width, "i")
    for bytes_ in range(4, 129, 4):
        vfp_block(asm, True, bytes_)
        vfp_block(asm, False, bytes_)


def emit(asm) -> None:
    a32_neon.emit(asm)
    a32_structure.emit(asm)
    emit_vfp_arithmetic(asm)
    emit_vfp_immediate(asm)
    emit_vfp_convert(asm)
    emit_vfp_core(asm)
    emit_vfp_system(asm)
    emit_vfp_memory(asm)


def header_tables(names: set[str]) -> list[str]:
    """C++ tables of the gadgets, for the builder; `names` are the gadget
    names that exist."""
    def g(name: str) -> str:
        return f"G::{name}" if name in names else "G::count"

    out = []
    out += a32_neon.header_tables(names)
    out.append("")
    out += a32_structure.header_tables(names)
    out.append("")
    out.append("/* VFP: [operation][single, double, half]. */")
    out.append("inline constexpr G kVfp3[5][3] = {")
    for op in VFP3_NAMES:
        out.append("\t{" + ", ".join(g(f"vfp_{op}_{w}") for w in "sdh") + "},")
    out.append("};")
    out.append("inline constexpr G kVfpMac[8][3] = {")
    for op in VFPMAC_NAMES:
        out.append("\t{" + ", ".join(g(f"vfp_{op}_{w}") for w in "sdh") + "},")
    out.append("};")
    out.append("inline constexpr G kVfp2[4][3] = {")
    for op in VFP2_NAMES:
        out.append("\t{" + ", ".join(g(f"vfp_{op}_{w}") for w in "sdh") + "},")
    out.append("};")
    out.append("/* VCMP: [E | zero << 1][single, double, half]. */")
    out.append("inline constexpr G kVfpCmp[4][3] = {")
    for suffix in ("", "e", "z", "ez"):
        out.append("\t{" + ", ".join(
            g(f"vfp_vcmp{suffix}_{w}") for w in "sdh") + "},")
    out.append("};")
    out.append("/* A conditional VMOV (register) as a select: [single, "
               "double]. */")
    out.append(f"inline constexpr unsigned kVfp2Vmov = "
               f"{VFP2_NAMES.index('vmov')};")
    out.append("inline constexpr G kVfpMoveSelect[2] = {" + ", ".join(
        g(f"vfp_vmov_{w}_select") for w in "sd") + "};")
    out.append("/* VCMP fused with VMRS APSR_nzcv: as kVfpCmp. */")
    out.append("inline constexpr G kVfpCmpApsr[4][3] = {")
    for suffix in ("", "e", "z", "ez"):
        out.append("\t{" + ", ".join(
            g(f"vfp_vcmp{suffix}_{w}_apsr") for w in "sdh") + "},")
    out.append("};")
    out.append("/* VLDM and VSTM by transfer size in words. */")
    for kind in ("ldm", "stm"):
        out.append(f"inline constexpr G kVfp{kind.capitalize()}[33] = {{"
                   "G::count, " + ", ".join(
                       g(f"vfp_{kind}_{4 * words}")
                       for words in range(1, 33)) + "};")
    return out
