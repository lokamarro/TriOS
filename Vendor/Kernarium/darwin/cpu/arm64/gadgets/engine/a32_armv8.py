# SPDX-License-Identifier: GPL-2.0-or-later
"""The Armv8 additions and optional extensions of the gadget engine's
AArch32 generator (the decoder's `complete` level).

generate_a32.py imports this module. A form of the complete level that
plan_of_complete does not name is the shared semantic library's
(cpu/arm64/aarch32/semantics.hpp), run through the a32_library gadget as the
whole instruction; one it names is lowered onto the host's A64 instruction,
in the way a32_simd.py and a32_neon.py lower VFP and Advanced SIMD. A
lowering that needs an optional A64 feature runs only where the guest has the
extension: the engine decodes in the profile the host gives it
(gadgets/a32/program_builder_a32.cpp decode_profile), so a form of an
extension the host lacks is UNDEFINED and the gadgets never execute an
instruction the host lacks.

Advanced SIMD forms are groups of a32_neon.py (the crypto instructions work
on whole Q registers); this module adds the groups and the families that are
not Advanced SIMD:

  crc      CRC32 and CRC32C of the low byte, halfword or word of Rm into
           Rn. Operand word: d[0:16] n[16:32] m[32:48], byte offsets of the
           core registers in CpuState.
  vfpv8    VSEL, VMAXNM, VMINNM and the VRINT forms of VFP, in single,
           double or half precision (aux 0, 1, 2; the half lives in the
           low half of an S register and a result zero-extends into it).
           Operand word as a32_simd.py: d[0:16] n[16:32] m[32:48].
  vfpcvt8  VCVTA, VCVTN, VCVTP and VCVTM of VFP: a float of the aux width
           to a signed or unsigned 32-bit integer in Sd.
  vfphalf  VINS, VMOVX and the half-precision moves of a core register.
"""

from __future__ import annotations

import re

import a32_neon
import a32_simd
from a32_neon import Variant, add, INDEX_SINGLE, INDEX_LANE, arrange, width

# The families this module adds, after a32_simd's.
FAMILIES: list[str] = ["crc", "vfpv8", "vfpcvt8", "vfphalf"]

CRC_SIZES = ["b", "h", "w"]


# ---------------------------------------------------------------------------
# Plans

def is_vfp_name(name: str) -> bool:
    """A complete-level form that is VFP (or a VFP-shaped one), not Advanced
    SIMD, though its decoder stem may be shared with an Advanced SIMD form."""
    return re.match(r"V(MAXNM|MINNM)_(A2|T2)_[HSD]$", name) is not None or \
        "_vfp_" in name


# vfpv8's operations.
VFP8_OPS = ["vseleq", "vselge", "vselgt", "vselvs", "vmaxnm", "vminnm",
            "vrinta", "vrintn", "vrintp", "vrintm", "vrintz", "vrintx",
            "vrintr"]
VFP8_SEL = {"VSELEQ": 0, "VSELGE": 1, "VSELGT": 2, "VSELVS": 3}
VFP8_RINT = {"VRINTA": 6, "VRINTN": 7, "VRINTP": 8, "VRINTM": 9,
             "VRINTZ": 10, "VRINTX": 11, "VRINTR": 12}
# vfpcvt8's rounding modes.
CVT8_MODES = {"VCVTA": 0, "VCVTN": 1, "VCVTP": 2, "VCVTM": 3}
CVT8_NAMES = ["a", "n", "p", "m"]
# vfphalf's operations.
HALF_OPS = ["vins", "vmovx", "mov_toh", "mov_h"]


# The simd level's VFP families that have half-precision forms.
VFP_HALF_FAMILIES = ("vfp3", "vfpmac", "vfp2", "vfpcmp", "vfpimm", "vfpcvt",
                     "vfpmem")


def precision(name: str) -> int:
    """The aux of a VFP form by its name's suffix: 0 single, 1 double, 2
    half."""
    return {"S": 0, "D": 1, "H": 2}[name.rsplit("_", 1)[1]]


def plan_of_complete(name: str, mnemonic: str):
    """(family, sub, aux) for a form of the complete level, or None when the
    form stays the library's."""
    if mnemonic == "SB":
        return ("hint", 0, 0)
    if mnemonic.startswith("CRC32"):
        castagnoli = "C" if mnemonic.startswith("CRC32C") else ""
        size = CRC_SIZES.index(mnemonic[len("CRC32" + castagnoli):].lower())
        return ("crc", size | (4 if castagnoli else 0), 0)
    if mnemonic in VFP8_SEL:
        return ("vfpv8", VFP8_SEL[mnemonic], precision(name))
    if is_vfp_name(name):
        if mnemonic in ("VMAXNM", "VMINNM"):
            return ("vfpv8", 4 if mnemonic == "VMAXNM" else 5,
                    precision(name))
        if mnemonic in VFP8_RINT:
            return ("vfpv8", VFP8_RINT[mnemonic], precision(name))
        if mnemonic in CVT8_MODES:
            return ("vfpcvt8", CVT8_MODES[mnemonic], precision(name))
    if name.endswith("_H"):
        # The half-precision VFP forms: the simd level's plans with aux bit
        # 2 (the half lives in the low half of an S register).
        plan = a32_simd.plan_of_simd(name, mnemonic)
        if plan is not None and plan[0] in VFP_HALF_FAMILIES:
            return (plan[0], plan[1], plan[2] | 4)
    if mnemonic in ("VCVTB", "VCVTT") and name.endswith(("_DH", "_HD")):
        return ("vfpcvt", a32_simd.CVT_HALF,
                (1 if mnemonic == "VCVTT" else 0))
    if mnemonic == "VINS":
        return ("vfphalf", 0, 0)
    if mnemonic == "VMOVX":
        return ("vfphalf", 1, 0)
    if name.startswith("VMOV_toh_"):
        return ("vfphalf", 2, 0)
    if name.startswith("VMOV_h_"):
        return ("vfphalf", 3, 0)
    if not is_vfp_name(name):
        neon = a32_neon.plan_of(name)
        if neon is not None:
            return ("neon", neon[0], neon[1])
    return None


# ---------------------------------------------------------------------------
# Advanced SIMD groups

def crypto_groups() -> None:
    """The Q-register cryptographic instructions: AES (FEAT_AES) and SHA-1
    and SHA-256 (FEAT_SHA1, FEAT_SHA256). The A32 and A64 forms compute the
    same functions on the same 128-bit operands."""
    def two(name, stem, body, acc=True):
        v = Variant(m1="q", acc="q" if acc else None, out="q", body=body)
        add(name, [stem], "dm", "none", explicit={0: ("", v)},
            index=INDEX_SINGLE, count=1)

    def three(name, stem, body):
        v = Variant(n="q", m="q", acc="q", out="q", body=body)
        add(name, [stem], "dnm", "none", explicit={0: ("", v)},
            index=INDEX_SINGLE, count=1)

    two("aese", "AESE", ["aese v0.16b, v1.16b"])
    two("aesd", "AESD", ["aesd v0.16b, v1.16b"])
    two("aesmc", "AESMC", ["aesmc v0.16b, v1.16b"], acc=False)
    two("aesimc", "AESIMC", ["aesimc v0.16b, v1.16b"], acc=False)
    two("sha1su1", "SHA1SU1", ["sha1su1 v0.4s, v1.4s"])
    two("sha256su0", "SHA256SU0", ["sha256su0 v0.4s, v1.4s"])
    # SHA1H: the rotated low word of Qm, the rest of Qd zero.
    two("sha1h", "SHA1H", ["sha1h s0, s1"], acc=False)
    three("sha1c", "SHA1C", ["sha1c q0, s1, v2.4s"])
    three("sha1p", "SHA1P", ["sha1p q0, s1, v2.4s"])
    three("sha1m", "SHA1M", ["sha1m q0, s1, v2.4s"])
    three("sha1su0", "SHA1SU0", ["sha1su0 v0.4s, v1.4s, v2.4s"])
    three("sha256h", "SHA256H", ["sha256h q0, q1, v2.4s"])
    three("sha256h2", "SHA256H2", ["sha256h2 q0, q1, v2.4s"])
    three("sha256su1", "SHA256SU1", ["sha256su1 v0.4s, v1.4s, v2.4s"])


def armv8_float_groups() -> None:
    """VMAXNM, VMINNM, VRINTx and VCVTA/N/P/M of Advanced SIMD, in single and
    half precision, under the standard FPSCR value."""
    def binary(name, stem, mnemonic):
        def make(esize, q, sign, alt):
            if esize not in (16, 32) or sign or alt:
                return None
            t = arrange(esize, q)
            w = width(q)
            return Variant(n=w, m=w, out=w,
                           body=[f"{mnemonic} v0.{t}, v1.{t}, v2.{t}"])
        add(name, [stem], "dnm", "none", make, fp=a32_neon.FP_ALL)

    def unary(name, stem, mnemonic):
        def make(esize, q, sign, alt):
            if esize not in (16, 32) or sign or alt:
                return None
            t = arrange(esize, q)
            w = width(q)
            return Variant(m1=w, out=w, body=[f"{mnemonic} v0.{t}, v1.{t}"])
        add(name, [stem], "dm", "none", make, fp=a32_neon.FP_ALL)

    def convert(name, stem, signed, unsigned):
        def make(esize, q, sign, alt):
            if esize not in (16, 32) or alt:
                return None
            t = arrange(esize, q)
            w = width(q)
            return Variant(m1=w, out=w,
                           body=[f"{unsigned if sign else signed} "
                                 f"v0.{t}, v1.{t}"])
        add(name, [stem], "dm", "usign", make, fp=a32_neon.FP_ALL)

    binary("vmaxnm", "VMAXNM", "fmaxnm")
    binary("vminnm", "VMINNM", "fminnm")
    for letter, mnemonic in (("a", "frinta"), ("n", "frintn"),
                             ("p", "frintp"), ("m", "frintm"),
                             ("z", "frintz"), ("x", "frintx")):
        unary(f"vrint{letter}", f"VRINT{letter.upper()}_asimd", mnemonic)
    for letter in CVT8_NAMES:
        convert(f"vcvt{letter}", f"VCVT{letter.upper()}_asimd",
                f"fcvt{letter}s", f"fcvt{letter}u")


def lane_group(name, stems, shape, make, *, indexed=False, rot=(0, 0),
               fp=a32_neon.FP_NONE) -> None:
    """A group indexed by INDEX_LANE: make(lane, q, rotation, half) returns
    the variant for one lane, Q or D form, rotation and element size, or
    None."""
    variants = {}
    for lane in range(8 if indexed else 1):
        for q in (0, 1):
            for rotation in range(4 if rot[1] else 1):
                for half in (0, 1):
                    variant = make(lane, q, rotation, half)
                    if variant is not None:
                        variants[lane | q << 3 | rotation << 4 | half << 6] = (
                            f"{lane}{width(q)}{rotation}"
                            f"{'h' if half else 's'}", variant)
    add(name, stems, shape, "none", explicit=variants, index=INDEX_LANE,
        count=128, fp=fp, indexed=indexed, rot_shift=rot[0],
        rot_mask=rot[1])


def armv8_integer_groups() -> None:
    """The dot products (FEAT_DotProd), VQRDMLAH and VQRDMLSH (FEAT_RDM)."""
    def dot(name, stem, mnemonic, *, element):
        def make(lane, q, rotation, half):
            if half or (lane > 1 and element):
                return None
            t = "4s" if q else "2s"
            b = "16b" if q else "8b"
            w = width(q)
            third = f"v2.4b[{lane}]" if element else f"v2.{b}"
            return Variant(n=w, m=w if not element else "d", acc=w, out=w,
                           body=[f"{mnemonic} v0.{t}, v1.{b}, {third}"])
        lane_group(name, [stem], "dnm", make, indexed=element)

    dot("vsdot", "VSDOT", "sdot", element=False)
    dot("vsdot_s", "VSDOT_s", "sdot", element=True)
    dot("vudot", "VUDOT", "udot", element=False)
    dot("vudot_s", "VUDOT_s", "udot", element=True)

    def rdm(name, stem, mnemonic):
        # Three registers, or by scalar (alt): the scalar is replicated.
        def make(esize, q, sign, alt):
            if sign or esize not in (16, 32):
                return None
            t = arrange(esize, q)
            w = width(q)
            return Variant(n=w, m="elem" if alt else w,
                           elem=t if alt else None, acc=w, out=w, qc=True,
                           body=[f"{mnemonic} v0.{t}, v1.{t}, v2.{t}"])
        add(name, [stem], "dnm", "scalar", make)

    rdm("vqrdmlah", "VQRDMLAH", "sqrdmlah")
    rdm("vqrdmlsh", "VQRDMLSH", "sqrdmlsh")


def armv8_complex_groups() -> None:
    """VCADD and VCMLA (FEAT_FCMA), and VFMAL and VFMSL (FEAT_FHM), under the
    standard FPSCR value."""
    def cadd(lane, q, rotation, half):
        if rotation > 1:
            return None
        t = arrange(16 if half else 32, q)
        w = width(q)
        return Variant(n=w, m=w, out=w,
                       body=[f"fcadd v0.{t}, v1.{t}, v2.{t}, "
                             f"#{90 if rotation == 0 else 270}"])
    lane_group("vcadd", ["VCADD"], "dnm", cadd, rot=(24, 1),
               fp=a32_neon.FP_ALL)

    def cmla(lane, q, rotation, half):
        t = arrange(16 if half else 32, q)
        w = width(q)
        return Variant(n=w, m=w, acc=w, out=w,
                       body=[f"fcmla v0.{t}, v1.{t}, v2.{t}, #{90 * rotation}"])
    lane_group("vcmla", ["VCMLA"], "dnm", cmla, rot=(23, 3),
               fp=a32_neon.FP_ALL)

    def cmla_s(lane, q, rotation, half):
        if lane > (1 if half else 0):
            return None
        if not half and not q:
            # A single complex pair of single-precision elements: the
            # vector form on the whole D register (no element form has
            # two lanes), so no lane computes a dead product.
            return Variant(n="d", m="d", acc="d", out="d",
                           body=[f"fcmla v0.2s, v1.2s, v2.2s, "
                                 f"#{90 * rotation}"])
        t = arrange(16 if half else 32, q)
        w = width(q)
        element = "h" if half else "s"
        return Variant(n=w, m="d", acc=w, out=w,
                       body=[f"fcmla v0.{t}, v1.{t}, "
                             f"v2.{element}[{lane}], #{90 * rotation}"])
    lane_group("vcmla_s", ["VCMLA_s"], "dnm", cmla_s, indexed=True,
               rot=(20, 3), fp=a32_neon.FP_ALL)

    def fmal(name, stem, mnemonic, *, element):
        def make(lane, q, rotation, half):
            if half or (not element and lane):
                return None
            t = "4s" if q else "2s"
            h = "4h" if q else "2h"
            w = width(q)
            source = "d" if q else "s"
            third = f"v2.h[{lane}]" if element else f"v2.{h}"
            return Variant(n=source, m=source, acc=w, out=w,
                           body=[f"{mnemonic} v0.{t}, v1.{h}, {third}"])
        lane_group(name, [stem], "dnm_s", make, indexed=element,
                   fp=a32_neon.FP_ALL)

    fmal("vfmal", "VFMAL", "fmlal", element=False)
    fmal("vfmsl", "VFMSL", "fmlsl", element=False)
    fmal("vfmal_s", "VFMAL_s", "fmlal", element=True)
    fmal("vfmsl_s", "VFMSL_s", "fmlsl", element=True)


crypto_groups()
armv8_float_groups()
armv8_integer_groups()
armv8_complex_groups()


# ---------------------------------------------------------------------------
# Gadgets

def emit_crc(asm) -> None:
    for castagnoli in (0, 1):
        for size, suffix in enumerate(CRC_SIZES):
            mnemonic = f"crc32{'c' if castagnoli else ''}{suffix}"
            asm.gadget(mnemonic, "\n".join([
                "ldr x8, [_stream], #8",
                "ubfx x9, x8, #16, #16", "ubfx x10, x8, #32, #16",
                "and x8, x8, #0xffff",
                "ldr w9, [_vcpu, x9]", "ldr w10, [_vcpu, x10]",
                f"{mnemonic} w9, w9, w10", "str w9, [_vcpu, x8]",
                "next_gadget"]))


def vfp8_operands(r: str, binary: bool) -> list[str]:
    lines = ["ldr x8, [_stream], #8"]
    lines += a32_simd.unpack("n", "m", "d") if binary else \
        a32_simd.unpack("d", "n")
    return lines


def emit_vfpv8(asm) -> None:
    for r, label in (("s", "s"), ("d", "d"), ("h", "h")):
        store = "s" if r == "h" else r     # a half result zero-extends in Sd
        conditions = ("eq", "ge", "gt", "vs")
        for op, cc in zip(VFP8_OPS[:4], conditions):
            asm.gadget(f"vfp8_{op}_{label}", "\n".join([
                *vfp8_operands(r, True),
                f"ldr {r}0, [_vcpu, x9]", f"ldr {r}1, [_vcpu, x10]",
                "msr nzcv, x27", f"fcsel {r}0, {r}0, {r}1, {cc}",
                f"str {store}0, [_vcpu, x8]", "next_gadget"]))
        for op, mnemonic in (("vmaxnm", "fmaxnm"), ("vminnm", "fminnm")):
            asm.gadget(f"vfp8_{op}_{label}", "\n".join([
                *vfp8_operands(r, True), "a32_fp_guest",
                f"ldr {r}0, [_vcpu, x9]", f"ldr {r}1, [_vcpu, x10]",
                f"{mnemonic} {r}0, {r}0, {r}1",
                f"str {store}0, [_vcpu, x8]", "next_gadget"]))
        for op, mnemonic in (("vrinta", "frinta"), ("vrintn", "frintn"),
                             ("vrintp", "frintp"), ("vrintm", "frintm"),
                             ("vrintz", "frintz"), ("vrintx", "frintx"),
                             ("vrintr", "frinti")):
            asm.gadget(f"vfp8_{op}_{label}", "\n".join([
                *vfp8_operands(r, False), "a32_fp_guest",
                f"ldr {r}0, [_vcpu, x9]", f"{mnemonic} {r}0, {r}0",
                f"str {store}0, [_vcpu, x8]", "next_gadget"]))


def emit_vfpcvt8(asm) -> None:
    for letter in CVT8_NAMES:
        for sign, suffix in ((0, "s"), (1, "u")):
            for r in ("s", "d", "h"):
                asm.gadget(f"vfp8_cvt{letter}_{suffix}_{r}", "\n".join([
                    *vfp8_operands(r, False), "a32_fp_guest",
                    f"ldr {r}0, [_vcpu, x9]",
                    f"fcvt{letter}{suffix} w9, {r}0",
                    "str w9, [_vcpu, x8]", "next_gadget"]))


def emit_vfphalf(asm) -> None:
    # VINS Sd, Sm: the low half of Sm into the top half of Sd. [d][m]
    asm.gadget("vfp_ins", "\n".join([
        "ldr x8, [_stream], #8", *a32_simd.unpack("d", "n"),
        "ldr w9, [_vcpu, x9]", "ldr w10, [_vcpu, x8]",
        "bfi w10, w9, #16, #16", "str w10, [_vcpu, x8]", "next_gadget"]))
    # VMOVX Sd, Sm: the top half of Sm, zero-extended.
    asm.gadget("vfp_movx", "\n".join([
        "ldr x8, [_stream], #8", *a32_simd.unpack("d", "n"),
        "ldr w9, [_vcpu, x9]", "lsr w9, w9, #16", "str w9, [_vcpu, x8]",
        "next_gadget"]))
    # VMOV Hn, Rt: the low half of Rt, zero-extended. [Sn][Rt]
    asm.gadget("vfp_mov_toh", "\n".join([
        "ldr x8, [_stream], #8", *a32_simd.unpack("d", "n"),
        "ldr w9, [_vcpu, x9]", "and w9, w9, #0xffff", "str w9, [_vcpu, x8]",
        "next_gadget"]))
    # VMOV Rt, Hn: the half of Sn, zero-extended. [Rt][Sn]
    asm.gadget("vfp_mov_h", "\n".join([
        "ldr x8, [_stream], #8", *a32_simd.unpack("d", "n"),
        "ldrh w9, [_vcpu, x9]", "str w9, [_vcpu, x8]", "next_gadget"]))
    # VCVTB and VCVTT between half and double: the half is in the bottom or
    # top of an S register (the builder's offset). [d][m]
    asm.gadget("vfp_cvt_dh", "\n".join([
        "ldr x8, [_stream], #8", *a32_simd.unpack("d", "n"),
        "a32_fp_guest", "ldr h0, [_vcpu, x9]", "fcvt d0, h0",
        "str d0, [_vcpu, x8]", "next_gadget"]))
    asm.gadget("vfp_cvt_hd", "\n".join([
        "ldr x8, [_stream], #8", *a32_simd.unpack("d", "n"),
        "a32_fp_guest", "ldr d0, [_vcpu, x9]", "fcvt h0, d0",
        "str h0, [_vcpu, x8]", "next_gadget"]))


def emit(asm) -> None:
    emit_crc(asm)
    emit_vfpv8(asm)
    emit_vfpcvt8(asm)
    emit_vfphalf(asm)


def header_tables(names: set[str]) -> list[str]:
    def g(name: str) -> str:
        return f"G::{name}" if name in names else "G::count"

    out = ["/* CRC32 and CRC32C: [castagnoli][byte, halfword, word]. */",
           "inline constexpr G kCrc32[2][3] = {"]
    for castagnoli in (0, 1):
        out.append("\t{" + ", ".join(
            g(f"crc32{'c' if castagnoli else ''}{s}") for s in CRC_SIZES) +
            "},")
    out.append("};")
    out.append("/* VSEL, VMAXNM, VMINNM and VRINT of VFP: [operation][s, d, h]. */")
    out.append(f"inline constexpr G kVfp8[{len(VFP8_OPS)}][3] = {{")
    for op in VFP8_OPS:
        out.append("\t{" + ", ".join(g(f"vfp8_{op}_{w}") for w in "sdh") +
                   "},")
    out.append("};")
    out.append("/* VCVTA, VCVTN, VCVTP and VCVTM of VFP: [mode][signed, "
               "unsigned][s, d, h]. */")
    out.append("inline constexpr G kVfpCvt8[4][2][3] = {")
    for letter in CVT8_NAMES:
        out.append("\t{" + ", ".join(
            "{" + ", ".join(g(f"vfp8_cvt{letter}_{sign}_{w}") for w in "sdh")
            + "}" for sign in "su") + "},")
    out.append("};")
    return out
