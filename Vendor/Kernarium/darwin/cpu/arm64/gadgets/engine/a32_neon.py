# SPDX-License-Identifier: GPL-2.0-or-later
"""The Advanced SIMD (NEON) part of the gadget engine's AArch32 generator.

a32_simd.py imports this module. A32 Advanced SIMD maps almost one to one
onto A64 Advanced SIMD (the same lanes, the same saturation and rounding), so
a gadget loads its operands from CpuState (D and Q registers are byte offsets
into CpuState::vectors, a32_simd.py), runs the A64 instruction, and stores
only the bytes the A32 destination has: a D register is 8 bytes, a Q register
16. What differs is spelled out here:

  - An A64 shift by an immediate has the amount encoded in the instruction,
    which would be a gadget per amount. The shift forms instead use the A64
    shift-by-register instructions: the amount is a signed byte in the
    stream, replicated into a vector (a negative amount is a right shift).
    The forms that exist in A64 only with an immediate (SQSHLU, the converts
    with a fraction width) and VEXT have a gadget per immediate.
  - Shift-insert, the right-shifting narrows and VSHLL are the register
    shift plus the extra instruction their semantics need.
  - VMLA and VMLS on floating point are not fused: a multiply, then an add.
  - A by-scalar form replicates the scalar with LD1R and then runs the
    vector instruction.
  - VZIP, VUZP, VTRN and VSWP write both registers.
  - A D register is loaded with LDR D, which zeroes the register's upper
    half, so an operation on 64-bit lanes uses the 2d arrangement and the
    zero upper lane is never stored.
  - Advanced SIMD floating point runs under the standard FPSCR value (flush
    to zero, default NaN, round to nearest). Each such gadget starts with
    a32_fp_standard (a32.inc), which switches the native environment to it
    when it holds the guest's FPCR, and VFP's a32_fp_guest switches back, so
    a loop of one kind writes the FPCR once. A saturating
    instruction leaves its QC flag in FPSR bit 27, which is where the
    cumulative flags VFP keeps live, so it first makes the native FP
    environment active.

A group is a decoder stem (or a few) lowered by one template. The builder
chooses a gadget of the group by a variant index it computes from the form:

    size_index | q << 2 | sign << 3 | alt << 4        (a generic group)

where size_index is log2(esize / 8), q means a Q-register operation (the
form's name ends in _Q), sign is the group's signedness operand (1 =
unsigned) and alt is the operand that tells two operations of one stem apart
(floating point, polynomial, by scalar). The other groups index their own
space (INDEX_*); INDEX_LANE, which the Armv8 groups of a32_armv8.py use, is

    lane | q << 3 | rotation << 4 | half << 6

with the lane the form's index_value (when the group is indexed), the
rotation a field of the instruction word (rot_shift, rot_mask) and half set
for 16-bit elements. A variant the template does not define has no gadget; the
builder turns that into the illegal-instruction exit, which the forms test
catches.

Operand words (16-bit CpuState offsets from the low end of a word):

    dnm      d[0:16] n[16:32] m[32:48]    m is the scalar element's address
                                           in a by-scalar variant
    dnm_s    as dnm, but n and m are S registers in a D-register variant
             (VFMAL and VFMSL read pairs of halves)
    dm       d[0:16] m[16:32]
    dmsh     dm, then a word whose low byte is the signed shift amount
    imm      d, then the 64-bit immediate
    dcore    d[0:16] offset of the core register[16:32]
    delem    d[0:16] address of the scalar element[16:32]
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Optional

SIZES = (8, 16, 32, 64)


def arrange(esize: int, q: int) -> str:
    """The A64 arrangement of a D (q = 0) or Q register of esize lanes."""
    return {8: ("8b", "16b"), 16: ("4h", "8h"), 32: ("2s", "4s"),
            64: ("2d", "2d")}[esize][q]


def wide(esize: int) -> str:
    """The Q arrangement whose lanes are twice esize."""
    return {8: "8h", 16: "4s", 32: "2d"}[esize]


def narrow(esize: int) -> str:
    """The D arrangement of esize lanes (the result of a narrowing op)."""
    return {8: "8b", 16: "4h", 32: "2s"}[esize]


def width(q: int) -> str:
    return "q" if q else "d"


# Selectors: how the builder reads (sign, alt) from the form's operands.
SELECTORS = [
    "none",
    "usign",            # sign = unsigned_
    "src_unsigned",     # sign = src_unsigned
    "floating",         # alt = floating_point
    "polynomial",       # alt = polynomial
    "unsigned_poly",    # sign = unsigned_, alt = polynomial
    "scalar",           # alt = scalar_form
    "vcge",             # sign = the type is unsigned, alt = it is floating
    "vcgt",
    "ceq",              # alt = not int_operation
    "cvt_int",          # sign = unsigned_, alt = to_integer
    "cvt_fixed",        # sign = unsigned_, alt = to_fixed
]

INDEX_GENERIC, INDEX_EXT, INDEX_TBL, INDEX_FIXED, INDEX_QSHLU, \
    INDEX_SINGLE, INDEX_LANE = range(7)
INDEX_NAMES = ["generic", "ext", "tbl", "fixed", "qshlu", "single", "lane"]
SHAPES = ["dnm", "dm", "dmsh", "imm", "dcore", "delem", "dnm_s"]

# fp: which variants need the standard FPSCR value.
FP_NONE, FP_ALL, FP_ALT = range(3)
FP_NAMES = ["none", "all", "alt"]


@dataclass
class Variant:
    """One gadget of a group: what it loads, the A64 instructions, and what
    it stores. Loads: n (operand n into v1), m (operand m into v2), m1
    (operand m into v1), acc (the destination into v0); "elem" replicates
    the scalar at m's address instead (with arrangement `elem`)."""
    n: Optional[str] = None
    m: Optional[str] = None
    m1: Optional[str] = None
    acc: Optional[str] = None
    elem: Optional[str] = None
    raw: list = field(default_factory=list)   # replaces the n and m loads
    body: list = field(default_factory=list)
    out: str = "d"
    out_reg: int = 0
    out2: Optional[str] = None                 # stored to m
    out2_reg: int = 0
    qc: bool = False
    # Single-precision VMAX, VMIN, VADD, VSUB and VMUL: the result is the same
    # in either native environment unless an operand is out of the range the
    # guard names (see GUARDS), so the gadget runs in whichever is active
    # when none is and switches only for those: "minmax" excludes NaNs and
    # denormals, "range" every nonzero operand outside 2**-63 to 2**65, which
    # keeps a sum, a difference and a product normal.
    guard: str = ""


@dataclass
class Group:
    name: str
    stems: list
    shape: str
    selector: str
    make: Optional[Callable] = None
    fp: int = FP_NONE
    index: int = INDEX_GENERIC
    shift_sign: int = 0        # +1 a left shift, -1 a right shift (dmsh)
    size_none: bool = False    # size index 0 whatever the form's esize
    explicit: Optional[dict] = None   # index -> (suffix, Variant)
    count: int = 32
    ident: int = 0
    indexed: bool = False      # INDEX_LANE: the lane is the form's index_value
    rot_shift: int = 0         # INDEX_LANE: the rotation field of the word
    rot_mask: int = 0


GROUPS: list = []


def add(name, stems, shape, selector, make=None, **kwargs) -> Group:
    g = Group(name, list(stems), shape, selector, make, **kwargs)
    g.ident = len(GROUPS)
    GROUPS.append(g)
    return g


# ---------------------------------------------------------------------------
# Group templates

def three_same(name, stems, signed, unsigned=None, *, sizes=(8, 16, 32),
               acc=False, qc=False, d_only=False, selector="usign",
               swap=False):
    """Vd = op(Vn, Vm), all one size; swap names the operands Vm, Vn (a
    shift by a register: the value is Vm)."""
    def make(esize, q, sign, alt):
        if esize not in sizes or (q and d_only) or alt:
            return None
        if sign and unsigned is None:
            return None
        mn = unsigned if sign else signed
        t = arrange(esize, q)
        first, second = ("v2", "v1") if swap else ("v1", "v2")
        w = width(q)
        return Variant(n=w, m=w, acc=w if acc else None, out=w, qc=qc,
                       body=[f"{mn} v0.{t}, {first}.{t}, {second}.{t}"])
    return add(name, stems, "dnm", selector, make)


def bitwise(name, stems, mnemonic, *, acc=False):
    def make(esize, q, sign, alt):
        if esize != 8 or sign or alt:
            return None
        t = arrange(8, q)
        w = width(q)
        return Variant(n=w, m=w, acc=w if acc else None, out=w,
                       body=[f"{mnemonic} v0.{t}, v1.{t}, v2.{t}"])
    return add(name, stems, "dnm", "none", make, size_none=True)


def float_same(name, stems, body, *, acc=False, d_only=False, guard=""):
    """A single- or half-precision Advanced SIMD operation; body lines use
    %T. `guard` lets the single-precision forms run in either native
    environment while the operands pass it (see Variant)."""
    def make(esize, q, sign, alt):
        if esize not in (16, 32) or sign or alt or (q and d_only):
            return None
        t = arrange(esize, q)
        w = width(q)
        return Variant(n=w, m=w, acc=w if acc else None, out=w,
                       guard=guard if esize == 32 else "",
                       body=[line.replace("%T", t) for line in body])
    return add(name, stems, "dnm", "none", make, fp=FP_ALL)


def unary(name, stems, mnemonic, *, sizes=(8, 16, 32), qc=False):
    def make(esize, q, sign, alt):
        if esize not in sizes or sign or alt:
            return None
        t = arrange(esize, q)
        w = width(q)
        return Variant(m1=w, out=w, qc=qc,
                       body=[f"{mnemonic} v0.{t}, v1.{t}"])
    return add(name, stems, "dm", "none", make)


def by_scalar(name, stems, int_mn, float_body, *, acc=False):
    """The by-scalar multiplies: an integer form (16 and 32 bits) or, when
    the form's floating_point is set, a single-precision one."""
    def make(esize, q, sign, alt):
        if sign:
            return None
        t = arrange(esize, q)
        w = width(q)
        if alt:
            if esize not in (16, 32):
                return None
            return Variant(n=w, m="elem", elem=t, acc=w if acc else None,
                           out=w,
                           body=[line.replace("%T", t) for line in float_body])
        if esize not in (16, 32):
            return None
        return Variant(n=w, m="elem", elem=t, acc=w if acc else None, out=w,
                       body=[f"{int_mn} v0.{t}, v1.{t}, v2.{t}"])
    return add(name, stems, "dnm", "floating", make, fp=FP_ALT)


def define_groups() -> None:
    # --- integer, three registers of one size -------------------------------
    three_same("vadd_i", ["VADD_i"], "add", sizes=SIZES, selector="none")
    three_same("vsub_i", ["VSUB_i"], "sub", sizes=SIZES, selector="none")
    three_same("vaba", ["VABA"], "saba", "uaba", acc=True)
    three_same("vabd_i", ["VABD_i"], "sabd", "uabd")
    three_same("vhadd", ["VHADD"], "shadd", "uhadd")
    three_same("vrhadd", ["VRHADD"], "srhadd", "urhadd")
    three_same("vhsub", ["VHSUB"], "shsub", "uhsub")
    three_same("vmax_i", ["VMAX_i"], "smax", "umax")
    three_same("vmin_i", ["VMIN_i"], "smin", "umin")
    three_same("vqadd", ["VQADD"], "sqadd", "uqadd", sizes=SIZES, qc=True)
    three_same("vqsub", ["VQSUB"], "sqsub", "uqsub", sizes=SIZES, qc=True)
    three_same("vtst", ["VTST"], "cmtst", selector="none")
    three_same("vmla_i", ["VMLA_i"], "mla", acc=True, selector="none")
    three_same("vmls_i", ["VMLS_i"], "mls", acc=True, selector="none")
    three_same("vpadd_i", ["VPADD_i"], "addp", d_only=True, selector="none")
    three_same("vpmax_i", ["VPMAX_i"], "smaxp", "umaxp", d_only=True)
    three_same("vpmin_i", ["VPMIN_i"], "sminp", "uminp", d_only=True)
    # shifts by a register: Vd = shift(Vm, Vn)
    three_same("vshl_r", ["VSHL_r"], "sshl", "ushl", sizes=SIZES, swap=True)
    three_same("vrshl", ["VRSHL"], "srshl", "urshl", sizes=SIZES, swap=True)
    three_same("vqshl_r", ["VQSHL_r"], "sqshl", "uqshl", sizes=SIZES,
               swap=True, qc=True)
    three_same("vqrshl", ["VQRSHL"], "sqrshl", "uqrshl", sizes=SIZES,
               swap=True, qc=True)

    # VMUL_i is the integer multiply or, with alt, the polynomial one
    def make_vmul(esize, q, sign, alt):
        if sign or (alt and esize != 8) or esize not in (8, 16, 32):
            return None
        t = arrange(esize, q)
        w = width(q)
        return Variant(n=w, m=w, out=w,
                       body=[f"{'pmul' if alt else 'mul'} v0.{t}, v1.{t}, "
                             f"v2.{t}"])
    add("vmul_i", ["VMUL_i"], "dnm", "polynomial", make_vmul)

    def doubling(name, stems, mnemonic):
        """VQDMULH and VQRDMULH: three registers, or by scalar."""
        def make(esize, q, sign, alt):
            if sign or esize not in (16, 32):
                return None
            t = arrange(esize, q)
            w = width(q)
            return Variant(n=w, m="elem" if alt else w,
                           elem=t if alt else None, out=w, qc=True,
                           body=[f"{mnemonic} v0.{t}, v1.{t}, v2.{t}"])
        return add(name, stems, "dnm", "scalar", make)

    doubling("vqdmulh", ["VQDMULH"], "sqdmulh")
    doubling("vqrdmulh", ["VQRDMULH"], "sqrdmulh")

    by_scalar("vmul_s", ["VMUL_s"], "mul", ["fmul v0.%T, v1.%T, v2.%T"])
    by_scalar("vmla_s", ["VMLA_s"], "mla", ["fmul v3.%T, v1.%T, v2.%T",
                                            "fadd v0.%T, v0.%T, v3.%T"],
              acc=True)
    by_scalar("vmls_s", ["VMLS_s"], "mls", ["fmul v3.%T, v1.%T, v2.%T",
                                            "fsub v0.%T, v0.%T, v3.%T"],
              acc=True)

    # --- long, wide and narrow ---------------------------------------------
    def long_op(name, stems, signed, unsigned, *, acc=False, qc=False,
                sizes=(8, 16, 32), scalar=False, poly=None, selector=None):
        def make(esize, q, sign, alt):
            if q or esize not in sizes:
                return None
            mn = unsigned if sign else signed
            use_elem = False
            if poly is not None:           # alt = polynomial (VMULL_i)
                if alt:
                    if sign or esize not in (8, 64):
                        return None
                    if esize == 64:        # VMULL.P64: FEAT_PMULL
                        return Variant(n="d", m="d", out="q" ,
                                       body=[f"{poly} v0.1q, v1.1d, v2.1d"])
                    mn = poly
                elif esize == 64:
                    return None
            elif alt:
                return None
            if scalar:
                use_elem = True
            t = arrange(esize, 0)
            return Variant(n="d", m="elem" if use_elem else "d",
                           elem=t if use_elem else None,
                           acc="q" if acc else None, out="q", qc=qc,
                           body=[f"{mn} v0.{wide(esize)}, v1.{t}, v2.{t}"])
        return add(name, stems, "dnm",
                   selector or ("unsigned_poly" if poly else "usign"),
                   make)

    long_op("vaddl", ["VADDL"], "saddl", "uaddl")
    long_op("vsubl", ["VSUBL"], "ssubl", "usubl")
    long_op("vabdl", ["VABDL_i"], "sabdl", "uabdl")
    long_op("vabal", ["VABAL"], "sabal", "uabal", acc=True)
    long_op("vmull_i", ["VMULL_i"], "smull", "umull", poly="pmull",
            sizes=(8, 16, 32, 64))
    long_op("vmull_s", ["VMULL_s"], "smull", "umull", sizes=(16, 32),
            scalar=True)
    long_op("vmlal_i", ["VMLAL_i"], "smlal", "umlal", acc=True)
    long_op("vmlsl_i", ["VMLSL_i"], "smlsl", "umlsl", acc=True)
    long_op("vmlal_s", ["VMLAL_s"], "smlal", "umlal", acc=True,
            sizes=(16, 32), scalar=True)
    long_op("vmlsl_s", ["VMLSL_s"], "smlsl", "umlsl", acc=True,
            sizes=(16, 32), scalar=True)

    def wide_op(name, stems, signed, unsigned):
        def make(esize, q, sign, alt):
            if q or alt or esize not in (8, 16, 32):
                return None
            mn = unsigned if sign else signed
            return Variant(n="q", m="d", out="q",
                           body=[f"{mn} v0.{wide(esize)}, v1.{wide(esize)}, "
                                 f"v2.{arrange(esize, 0)}"])
        return add(name, stems, "dnm", "usign", make)

    wide_op("vaddw", ["VADDW"], "saddw", "uaddw")
    wide_op("vsubw", ["VSUBW"], "ssubw", "usubw")

    def qdmull(name, stems, mnemonic, acc):
        def make(esize, q, sign, alt):
            if q or sign or esize not in (16, 32):
                return None
            t = arrange(esize, 0)
            return Variant(n="d", m="elem" if alt else "d",
                           elem=t if alt else None,
                           acc="q" if acc else None, out="q", qc=True,
                           body=[f"{mnemonic} v0.{wide(esize)}, v1.{t}, "
                                 f"v2.{t}"])
        return add(name, stems, "dnm", "scalar", make)

    qdmull("vqdmull", ["VQDMULL"], "sqdmull", False)
    qdmull("vqdmlal", ["VQDMLAL"], "sqdmlal", True)
    qdmull("vqdmlsl", ["VQDMLSL"], "sqdmlsl", True)

    def narrow_op(name, stems, mnemonic):
        def make(esize, q, sign, alt):
            if q or sign or alt or esize not in (8, 16, 32):
                return None
            return Variant(n="q", m="q", out="d",
                           body=[f"{mnemonic} v0.{narrow(esize)}, "
                                 f"v1.{wide(esize)}, v2.{wide(esize)}"])
        return add(name, stems, "dnm", "none", make)

    narrow_op("vaddhn", ["VADDHN"], "addhn")
    narrow_op("vraddhn", ["VRADDHN"], "raddhn")
    narrow_op("vsubhn", ["VSUBHN"], "subhn")
    narrow_op("vrsubhn", ["VRSUBHN"], "rsubhn")

    # --- bitwise -----------------------------------------------------------
    bitwise("vand", ["VAND_r"], "and")
    bitwise("vbic", ["VBIC_r"], "bic")
    bitwise("vorr", ["VORR_r"], "orr")
    bitwise("vorn", ["VORN_r"], "orn")
    bitwise("veor", ["VEOR"], "eor")
    bitwise("vbsl", ["VBSL"], "bsl", acc=True)
    bitwise("vbit", ["VBIT"], "bit", acc=True)
    bitwise("vbif", ["VBIF"], "bif", acc=True)

    # --- comparisons ------------------------------------------------------
    def compare(name, stems, signed, unsigned, fp_mn, selector):
        def make(esize, q, sign, alt):
            t = arrange(esize, q)
            w = width(q)
            if alt:
                if esize not in (16, 32) or sign:
                    return None
                return Variant(n=w, m=w, out=w,
                               body=[f"{fp_mn} v0.{t}, v1.{t}, v2.{t}"])
            if esize not in (8, 16, 32):
                return None
            mn = unsigned if sign else signed
            return Variant(n=w, m=w, out=w,
                           body=[f"{mn} v0.{t}, v1.{t}, v2.{t}"])
        return add(name, stems, "dnm", selector, make, fp=FP_ALT)

    compare("vceq_r", ["VCEQ_r"], "cmeq", "cmeq", "fcmeq", "ceq")
    compare("vcge_r", ["VCGE_r"], "cmge", "cmhs", "fcmge", "vcge")
    compare("vcgt_r", ["VCGT_r"], "cmgt", "cmhi", "fcmgt", "vcgt")

    # --- floating point, three registers -----------------------------------
    float_same("vadd_f", ["VADD_f"], ["fadd v0.%T, v1.%T, v2.%T"],
               guard="range")
    float_same("vsub_f", ["VSUB_f"], ["fsub v0.%T, v1.%T, v2.%T"],
               guard="range")
    float_same("vmul_f", ["VMUL_f"], ["fmul v0.%T, v1.%T, v2.%T"],
               guard="range")
    float_same("vmla_f", ["VMLA_f"], ["fmul v3.%T, v1.%T, v2.%T",
                                      "fadd v0.%T, v0.%T, v3.%T"], acc=True)
    float_same("vmls_f", ["VMLS_f"], ["fmul v3.%T, v1.%T, v2.%T",
                                      "fsub v0.%T, v0.%T, v3.%T"], acc=True)
    float_same("vfma", ["VFMA"], ["fmla v0.%T, v1.%T, v2.%T"], acc=True)
    float_same("vfms", ["VFMS"], ["fmls v0.%T, v1.%T, v2.%T"], acc=True)
    float_same("vabd_f", ["VABD_f"], ["fabd v0.%T, v1.%T, v2.%T"])
    float_same("vmax_f", ["VMAX_f"], ["fmax v0.%T, v1.%T, v2.%T"],
               guard="minmax")
    float_same("vmin_f", ["VMIN_f"], ["fmin v0.%T, v1.%T, v2.%T"],
               guard="minmax")
    float_same("vpadd_f", ["VPADD_f"], ["faddp v0.%T, v1.%T, v2.%T"],
               d_only=True)
    float_same("vpmax_f", ["VPMAX_f"], ["fmaxp v0.%T, v1.%T, v2.%T"],
               d_only=True)
    float_same("vpmin_f", ["VPMIN_f"], ["fminp v0.%T, v1.%T, v2.%T"],
               d_only=True)
    # VRECPS and VRSQRTS are AArch32's unfused steps (the product is rounded
    # first, and infinity times zero is a zero product), not the fused A64
    # FRECPS and FRSQRTS: the semantic library's.
    float_same("vacge", ["VACGE"], ["facge v0.%T, v1.%T, v2.%T"])
    float_same("vacgt", ["VACGT"], ["facgt v0.%T, v1.%T, v2.%T"])

    # --- unary, one size ----------------------------------------------------
    def abs_neg(name, stems, int_mn, fp_mn):
        def make(esize, q, sign, alt):
            if sign:
                return None
            t = arrange(esize, q)
            w = width(q)
            if alt:
                if esize not in (16, 32):
                    return None
                return Variant(m1=w, out=w, body=[f"{fp_mn} v0.{t}, v1.{t}"])
            if esize not in (8, 16, 32):
                return None
            return Variant(m1=w, out=w, body=[f"{int_mn} v0.{t}, v1.{t}"])
        # FABS and FNEG only change the sign bit, so no FPSCR is involved.
        return add(name, stems, "dm", "floating", make)

    abs_neg("vabs", ["VABS"], "abs", "fabs")
    abs_neg("vneg", ["VNEG"], "neg", "fneg")
    unary("vqabs", ["VQABS"], "sqabs", qc=True)
    unary("vqneg", ["VQNEG"], "sqneg", qc=True)
    unary("vcls", ["VCLS"], "cls")
    unary("vclz", ["VCLZ"], "clz")
    unary("vcnt", ["VCNT"], "cnt", sizes=(8,))
    unary("vmvn_r", ["VMVN_r"], "mvn", sizes=(8,))
    unary("vrev16", ["VREV16"], "rev16", sizes=(8,))
    unary("vrev32", ["VREV32"], "rev32", sizes=(8, 16))
    unary("vrev64", ["VREV64"], "rev64", sizes=(8, 16, 32))

    def paddl(name, stems, signed, unsigned, acc):
        def make(esize, q, sign, alt):
            if alt or esize not in (8, 16, 32):
                return None
            mn = unsigned if sign else signed
            src = arrange(esize, q)
            dst = arrange(esize * 2, q) if esize < 32 else \
                ("2d" if q else "1d")
            w = width(q)
            return Variant(m1=w, acc=w if acc else None, out=w,
                           body=[f"{mn} v0.{dst}, v1.{src}"])
        return add(name, stems, "dm", "usign", make)

    paddl("vpaddl", ["VPADDL"], "saddlp", "uaddlp", False)
    paddl("vpadal", ["VPADAL"], "sadalp", "uadalp", True)

    def zero_compare(name, stems, int_mn, fp_mn):
        def make(esize, q, sign, alt):
            if sign:
                return None
            t = arrange(esize, q)
            w = width(q)
            if alt:
                if esize not in (16, 32):
                    return None
                return Variant(m1=w, out=w,
                               body=[f"{fp_mn} v0.{t}, v1.{t}, #0.0"])
            if esize not in (8, 16, 32):
                return None
            return Variant(m1=w, out=w,
                           body=[f"{int_mn} v0.{t}, v1.{t}, #0"])
        return add(name, stems, "dm", "floating", make, fp=FP_ALT)

    zero_compare("vceq_i", ["VCEQ_i"], "cmeq", "fcmeq")
    zero_compare("vcge_i", ["VCGE_i"], "cmge", "fcmge")
    zero_compare("vcgt_i", ["VCGT_i"], "cmgt", "fcmgt")
    zero_compare("vcle_i", ["VCLE_i"], "cmle", "fcmle")
    zero_compare("vclt_i", ["VCLT_i"], "cmlt", "fcmlt")

    def estimate(name, stems, fp_mn, int_mn):
        def make(esize, q, sign, alt):
            if esize not in (16, 32) or sign or (esize == 16 and not alt):
                return None
            t = arrange(esize, q)
            w = width(q)
            return Variant(m1=w, out=w,
                           body=[f"{fp_mn if alt else int_mn} v0.{t}, "
                                 f"v1.{t}"])
        return add(name, stems, "dm", "floating", make, fp=FP_ALT)

    estimate("vrecpe", ["VRECPE"], "frecpe", "urecpe")
    estimate("vrsqrte", ["VRSQRTE"], "frsqrte", "ursqrte")

    def cvt_int(name, stems):
        def make(esize, q, sign, alt):
            if esize not in (16, 32):
                return None
            t = arrange(esize, q)
            w = width(q)
            if alt:   # float to integer, toward zero
                mn = "fcvtzu" if sign else "fcvtzs"
            else:
                mn = "ucvtf" if sign else "scvtf"
            return Variant(m1=w, out=w, body=[f"{mn} v0.{t}, v1.{t}"])
        return add(name, stems, "dm", "cvt_int", make, fp=FP_ALL)

    cvt_int("vcvt_is", ["VCVT_is"])

    def cvt_half(name, stems, to_single):
        def make(esize, q, sign, alt):
            if q or sign or alt or esize != 16:
                return None
            if to_single:
                return Variant(m1="d", out="q", body=["fcvtl v0.4s, v1.4h"])
            return Variant(m1="q", out="d", body=["fcvtn v0.4h, v1.4s"])
        return add(name, stems, "dm", "none", make, fp=FP_ALL)

    cvt_half("vcvt_sh", ["VCVT_sh"], True)
    cvt_half("vcvt_hs", ["VCVT_hs"], False)

    # --- narrowing and widening moves --------------------------------------
    def movl(name, stems):
        def make(esize, q, sign, alt):
            if q or alt or esize not in (8, 16, 32):
                return None
            mn = "ushll" if sign else "sshll"
            return Variant(m1="d", out="q",
                           body=[f"{mn} v0.{wide(esize)}, "
                                 f"v1.{arrange(esize, 0)}, #0"])
        return add(name, stems, "dm", "usign", make)

    movl("vmovl", ["VMOVL"])

    def movn(name, stems, signed, unsigned, qc, selector):
        def make(esize, q, sign, alt):
            if q or alt or esize not in (8, 16, 32):
                return None
            mn = unsigned if sign else signed
            if mn is None:
                return None
            return Variant(m1="q", out="d", qc=qc,
                           body=[f"{mn} v0.{narrow(esize)}, "
                                 f"v1.{wide(esize)}"])
        return add(name, stems, "dm", selector, make)

    movn("vmovn", ["VMOVN"], "xtn", None, False, "none")
    movn("vqmovn", ["VQMOVN"], "sqxtn", "uqxtn", True, "src_unsigned")
    movn("vqmovun", ["VQMOVUN"], "sqxtun", None, True, "none")

    # --- shifts by an immediate (through the shift-by-register forms) ------
    def shift_group(name, stems, signed, unsigned, direction, *,
                    accumulate=False, qc=False, selector="usign"):
        def make(esize, q, sign, alt):
            if alt:
                return None
            mn = unsigned if sign else signed
            t = arrange(esize, q)
            w = width(q)
            body = [f"{mn} v{3 if accumulate else 0}.{t}, v1.{t}, v2.{t}"]
            if accumulate:
                body.append(f"add v0.{t}, v0.{t}, v3.{t}")
            return Variant(m1=w, acc=w if accumulate else None, out=w,
                           qc=qc, body=body)
        return add(name, stems, "dmsh", selector, make,
                   shift_sign=direction)

    shift_group("vshl_i", ["VSHL_i"], "ushl", "ushl", +1, selector="none")
    shift_group("vshr", ["VSHR"], "sshl", "ushl", -1)
    shift_group("vrshr", ["VRSHR"], "srshl", "urshl", -1)
    shift_group("vsra", ["VSRA"], "sshl", "ushl", -1, accumulate=True)
    shift_group("vrsra", ["VRSRA"], "srshl", "urshl", -1, accumulate=True)
    shift_group("vqshl_i", ["VQSHL_i"], "sqshl", "uqshl", +1, qc=True,
                selector="src_unsigned")

    def insert_group(name, stems, direction):
        def make(esize, q, sign, alt):
            if alt or sign:
                return None
            t = arrange(esize, q)
            w = width(q)
            return Variant(m1=w, acc=w, out=w, body=[
                f"ushl v3.{t}, v1.{t}, v2.{t}",
                "movi v4.16b, #0xff",
                f"ushl v4.{t}, v4.{t}, v2.{t}",
                "bit v0.16b, v3.16b, v4.16b"])
        return add(name, stems, "dmsh", "none", make, shift_sign=direction)

    insert_group("vsli", ["VSLI"], +1)
    insert_group("vsri", ["VSRI"], -1)

    def narrowing_shift(name, stems, shift_s, shift_u, narrow_s, narrow_u,
                        qc, selector):
        def make(esize, q, sign, alt):
            if q or alt or esize not in (8, 16, 32):
                return None
            smn = shift_u if sign else shift_s
            nmn = narrow_u if sign else narrow_s
            if smn is None or nmn is None:
                return None
            w = wide(esize)
            return Variant(m1="q", out="d", qc=qc, body=[
                f"{smn} v3.{w}, v1.{w}, v2.{w}",
                f"{nmn} v0.{narrow(esize)}, v3.{w}"])
        return add(name, stems, "dmsh", selector, make, shift_sign=-1)

    narrowing_shift("vshrn", ["VSHRN"], "ushl", "ushl", "xtn", "xtn", False,
                    "none")
    narrowing_shift("vrshrn", ["VRSHRN"], "urshl", "urshl", "xtn", "xtn",
                    False, "none")
    narrowing_shift("vqshrn", ["VQSHRN"], "sshl", "ushl", "sqxtn", "uqxtn",
                    True, "src_unsigned")
    narrowing_shift("vqrshrn", ["VQRSHRN"], "srshl", "urshl", "sqxtn",
                    "uqxtn", True, "src_unsigned")
    narrowing_shift("vqshrun", ["VQSHRUN"], "sshl", None, "sqxtun", None,
                    True, "none")
    narrowing_shift("vqrshrun", ["VQRSHRUN"], "srshl", None, "sqxtun", None,
                    True, "none")

    def shll(name, stems):
        def make(esize, q, sign, alt):
            if q or alt or esize not in (8, 16, 32):
                return None
            w = wide(esize)
            return Variant(m1="d", out="q", body=[
                f"{'uxtl' if sign else 'sxtl'} v3.{w}, "
                f"v1.{arrange(esize, 0)}",
                f"{'ushl' if sign else 'sshl'} v0.{w}, v3.{w}, v2.{w}"])
        return add(name, stems, "dmsh", "usign", make, shift_sign=+1)

    shll("vshll", ["VSHLL"])

    # --- immediates ---------------------------------------------------------
    def immediate(name, stems, body, acc):
        def make(esize, q, sign, alt):
            if esize != 8 or sign or alt:
                return None
            w = width(q)
            return Variant(acc=w if acc else None, out=w,
                           out_reg=0 if acc else 1,
                           body=["dup v1.2d, x9"] + body)
        return add(name, stems, "imm", "none", make, size_none=True)

    immediate("vmov_i", ["VMOV_i"], [], False)
    immediate("vmvn_i", ["VMVN_i"], ["mvn v1.16b, v1.16b"], False)
    immediate("vorr_i", ["VORR_i"], ["orr v0.16b, v0.16b, v1.16b"], True)
    immediate("vbic_i", ["VBIC_i"], ["bic v0.16b, v0.16b, v1.16b"], True)

    # --- duplicate -----------------------------------------------------------
    def vdup_r(name, stems):
        def make(esize, q, sign, alt):
            if sign or alt or esize not in (8, 16, 32):
                return None
            return Variant(out=width(q),
                           body=[f"dup v0.{arrange(esize, q)}, w9"])
        return add(name, stems, "dcore", "none", make)

    vdup_r("vdup_r", ["VDUP_r"])

    def vdup_s(name, stems):
        def make(esize, q, sign, alt):
            if sign or alt or esize not in (8, 16, 32):
                return None
            return Variant(out=width(q), elem=arrange(esize, q), body=[])
        return add(name, stems, "delem", "none", make)

    vdup_s("vdup_s", ["VDUP_s"])

    # --- permutes: both registers are read and written ----------------------
    def permute(name, stems, first, second, sizes, d_sizes):
        def make(esize, q, sign, alt):
            if sign or alt or esize not in (sizes if q else d_sizes):
                return None
            t = arrange(esize, q)
            w = width(q)
            return Variant(m=w, acc=w, out=w, out_reg=3, out2=w, out2_reg=4,
                           body=[f"{first} v3.{t}, v0.{t}, v2.{t}",
                                 f"{second} v4.{t}, v0.{t}, v2.{t}"])
        return add(name, stems, "dm", "none", make)

    permute("vtrn", ["VTRN"], "trn1", "trn2", (8, 16, 32), (8, 16, 32))
    permute("vzip", ["VZIP"], "zip1", "zip2", (8, 16, 32), (8, 16))
    permute("vuzp", ["VUZP"], "uzp1", "uzp2", (8, 16, 32), (8, 16))

    def vswp(name, stems):
        def make(esize, q, sign, alt):
            if esize != 8 or sign or alt:
                return None
            w = width(q)
            return Variant(m=w, acc=w, out=w, out_reg=2, out2=w, out2_reg=0,
                           body=[])
        return add(name, stems, "dm", "none", make, size_none=True)

    vswp("vswp", ["VSWP"])

    # --- groups with an index space of their own ----------------------------
    ext = {}
    for q in (0, 1):
        for position in range(16 if q else 8):
            w = width(q)
            t = arrange(8, q)
            ext[position | q << 4] = (
                f"{position}{w}",
                Variant(n=w, m=w, out=w,
                        body=[f"ext v0.{t}, v1.{t}, v2.{t}, #{position}"]))
    add("vext", ["VEXT"], "dnm", "none", explicit=ext, index=INDEX_EXT,
        count=32)

    for name, stem, mnemonic in (("vtbl", "VTBL", "tbl"),
                                 ("vtbx", "VTBX", "tbx")):
        table = {}
        for length in range(1, 5):
            raw = ["add x9, _vcpu, x9"]
            regs = {1: "{v16.16b}", 2: "{v16.16b}", 3: "{v16.16b, v17.16b}",
                    4: "{v16.16b, v17.16b}"}[length]
            if length == 1:
                raw.append("ldr d16, [x9]")
            elif length == 2:
                raw.append("ldr q16, [x9]")
            elif length == 3:
                raw += ["ldr q16, [x9]", "ldr d17, [x9, #16]"]
            else:
                raw.append("ldp q16, q17, [x9]")
            raw.append("ldr d2, [_vcpu, x10]")
            if mnemonic == "tbx" and length in (1, 3):
                # The A64 table is a multiple of 16 bytes: the padding would
                # answer an index the A32 table does not have, which VTBX
                # leaves at the destination's byte.
                table[length - 1] = (
                    str(length),
                    Variant(raw=raw, acc="d", out="d", out_reg=6, body=[
                        f"movi v5.8b, #{8 * length}",
                        "cmhs v5.8b, v2.8b, v5.8b",
                        f"tbl v6.8b, {regs}, v2.8b",
                        "bit v6.8b, v0.8b, v5.8b"]))
                continue
            table[length - 1] = (
                str(length),
                Variant(raw=raw, acc="d" if mnemonic == "tbx" else None,
                        out="d",
                        body=[f"{mnemonic} v0.8b, {regs}, v2.8b"]))
        add(name, [stem], "dnm", "none", explicit=table, index=INDEX_TBL,
            count=4)

    # fraction-width converts (VCVT_xs): [fbits - 1 | q << 5 | unsigned << 6
    # | to_fixed << 7 | half << 8]
    fixed = {}
    for half in (0, 1):
        for to_fixed in (0, 1):
            for sign in (0, 1):
                for q in (0, 1):
                    for fbits in range(1, 17 if half else 33):
                        t = arrange(16 if half else 32, q)
                        if to_fixed:
                            mn = "fcvtzu" if sign else "fcvtzs"
                        else:
                            mn = "ucvtf" if sign else "scvtf"
                        fixed[(fbits - 1) | q << 5 | sign << 6 |
                              to_fixed << 7 | half << 8] = (
                            f"{fbits}{width(q)}{'u' if sign else 's'}"
                            f"{'f' if to_fixed else 't'}{'h' if half else ''}",
                            Variant(m1=width(q), out=width(q),
                                    body=[f"{mn} v0.{t}, v1.{t}, #{fbits}"]))
    add("vcvt_xs", ["VCVT_xs"], "dm", "cvt_fixed", explicit=fixed,
        index=INDEX_FIXED, count=512, fp=FP_ALL)

    # VQSHLU by an immediate: [q * 120 + base[esize] + shift]
    qshlu = {}
    base = {8: 0, 16: 8, 32: 24, 64: 56}
    for q in (0, 1):
        for esize in SIZES:
            for shift in range(esize):
                t = arrange(esize, q)
                qshlu[q * 120 + base[esize] + shift] = (
                    f"{esize}{width(q)}_{shift}",
                    Variant(m1=width(q), out=width(q), qc=True,
                            body=[f"sqshlu v0.{t}, v1.{t}, #{shift}"]))
    add("vqshlu", ["VQSHLU_i"], "dm", "none", explicit=qshlu,
        index=INDEX_QSHLU, count=240)


# ---------------------------------------------------------------------------
# Rendering

def stem_of(name: str) -> str:
    """The decoder stem of a form name: VADD_i_A1_D -> VADD_i."""
    parts = name.split("_")
    while parts and parts[-1] in ("D", "Q", "DH", "DS", "QH", "QS", "nowb",
                                   "posti", "postr"):
        parts.pop()
    if parts and len(parts[-1]) == 2 and parts[-1][0] in "AT" and \
            parts[-1][1].isdigit():
        parts.pop()
    return "_".join(parts)


_BY_STEM: dict = {}


def plan_of(name: str):
    """(sub, aux) for a NEON compute form, or None. aux bit 0 is a Q-register
    form."""
    if not _BY_STEM:
        for g in GROUPS:
            for stem in g.stems:
                _BY_STEM[stem] = g
    g = _BY_STEM.get(stem_of(name))
    if g is None:
        return None
    return g.ident, 1 if name.endswith("_Q") else 0


# The guards of Variant.guard, run with the guest's FPCR native: they branch
# to 21f (the slow path, which switches to the Standard value) when the
# operands in v1 and v2 would give a different result under it. A lane
# shifted left once drops the sign: zero is 0, a denormal 2 to 0x00fffffe, a
# normal number or an infinity 0x01000000 to 0xff000000 and a NaN above that.
# A D register's upper lanes are zero, so they are good.
GUARDS = {
    # Subtracting 0x01000000 turns the good lanes into [0, 0xfe000000] and
    # zero into 0xff000000, so a lane is bad when it is above 0xfe000000 and
    # not zero.
    "minmax": [
        "movi v7.4s, #0x1, lsl #24", "movi v16.4s, #0xfe, lsl #24",
        "shl v3.4s, v1.4s, #1", "shl v4.4s, v2.4s, #1",
        "sub v5.4s, v3.4s, v7.4s", "sub v6.4s, v4.4s, v7.4s",
        "cmhi v5.4s, v5.4s, v16.4s", "cmhi v6.4s, v6.4s, v16.4s",
        "cmtst v3.4s, v3.4s, v3.4s", "cmtst v4.4s, v4.4s, v4.4s",
        "and v5.16b, v5.16b, v3.16b", "and v6.16b, v6.16b, v4.16b",
        "orr v5.16b, v5.16b, v6.16b", "umaxv s5, v5.4s",
        "fmov w11, s5", "cbnz w11, 21f"],
    # The rounding mode must be the Standard value's. A sum or difference of
    # numbers from 2**-63 up is a multiple of 2**-86, so it is zero or
    # normal, and a product of two is at least 2**-126, so neither flushes a
    # result nor reads a denormal. An exponent field of 64 to 191 is a
    # shifted lane from 0x80000000 to 0x17fffffff, whose top two bits
    # differ: subtracting 0x40000000 leaves [0, 0x7fffffff], and anything
    # else, zero included, goes negative. A nonzero negative lane is bad.
    "range": [
        "ldr w11, [_vcpu, #UML_GADGET_CPU_FPCR]", "tst w11, #0xc00000",
        "b.ne 21f",
        "movi v7.4s, #0x40, lsl #24",
        "shl v3.4s, v1.4s, #1", "shl v4.4s, v2.4s, #1",
        "sub v5.4s, v3.4s, v7.4s", "sub v6.4s, v4.4s, v7.4s",
        "cmlt v5.4s, v5.4s, #0", "cmlt v6.4s, v6.4s, #0",
        "cmtst v3.4s, v3.4s, v3.4s", "cmtst v4.4s, v4.4s, v4.4s",
        "and v5.16b, v5.16b, v3.16b", "and v6.16b, v6.16b, v4.16b",
        "orr v5.16b, v5.16b, v6.16b", "umaxv s5, v5.4s",
        "fmov w11, s5", "cbnz w11, 21f"],
}


def render(g: Group, v: Variant, floating: bool = False) -> list:
    lines: list = []
    shape = g.shape
    if shape in ("dnm", "dnm_s"):
        lines += ["ldr x8, [_stream], #8", "ubfx x9, x8, #16, #16",
                  "ubfx x10, x8, #32, #16", "and x8, x8, #0xffff"]
    elif shape == "dm":
        lines += ["ldr x8, [_stream], #8", "ubfx x10, x8, #16, #16",
                  "and x8, x8, #0xffff"]
    elif shape == "dmsh":
        lines += ["ldp x8, x11, [_stream], #16", "ubfx x10, x8, #16, #16",
                  "and x8, x8, #0xffff", "dup v2.16b, w11"]
    elif shape == "imm":
        lines += ["ldp x8, x9, [_stream], #16"]
    elif shape == "dcore":
        lines += ["ldr x8, [_stream], #8", "ubfx x9, x8, #16, #16",
                  "and x8, x8, #0xffff", "ldr w9, [_vcpu, x9]"]
    elif shape == "delem":
        lines += ["ldr x8, [_stream], #8", "ubfx x10, x8, #16, #16",
                  "and x8, x8, #0xffff"]
    guarded = floating and v.guard
    if floating and not guarded:
        lines.append("a32_fp_standard")
    elif v.qc:
        lines.append("ensure_fp_environment")
    if v.raw:
        lines += v.raw
    if v.n:
        lines.append(f"ldr {v.n}1, [_vcpu, x9]")
    if v.m == "elem":
        lines += ["add x10, _vcpu, x10", f"ld1r {{v2.{v.elem}}}, [x10]"]
    elif v.m:
        lines.append(f"ldr {v.m}2, [_vcpu, x10]")
    if guarded:
        assert shape == "dnm" and not v.m1 and not v.acc and v.m != "elem"
        # The Standard value is native: run, as every other gadget does. The
        # environment is not active at all: switch to the Standard value.
        # The guest's is native: run if the operands pass.
        lines += ["ldrb w6, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]", "cbz w6, 21f",
                  "ldrb w6, [_vcpu, #UML_ARM64_CPU_A32_FP_STANDARD]",
                  "cbnz w6, 22f"] + GUARDS[v.guard] + ["22:"]
    if v.m1:
        lines.append(f"ldr {v.m1}1, [_vcpu, x10]")
    if shape == "delem":
        lines += ["add x10, _vcpu, x10", f"ld1r {{v0.{v.elem}}}, [x10]"]
    if v.acc:
        lines.append(f"ldr {v.acc}0, [_vcpu, x8]")
    lines += v.body
    # With d == m the register named last wins; the Arm oracle (and so the
    # corpus) leaves the d result.
    if v.out2:
        lines.append(f"str {v.out2}{v.out2_reg}, [_vcpu, x10]")
    lines.append(f"str {v.out}{v.out_reg}, [_vcpu, x8]")
    lines.append("next_gadget")
    if guarded:
        lines += ["21:", "a32_fp_standard"] + v.body
        lines.append(f"str {v.out}{v.out_reg}, [_vcpu, x8]")
        lines.append("next_gadget")
    return lines


def variants_of(g: Group):
    """(index, suffix, Variant) of every gadget the group defines."""
    if g.explicit is not None:
        for index, (suffix, variant) in sorted(g.explicit.items()):
            yield index, suffix, variant
        return
    for alt in (0, 1):
        for sign in (0, 1):
            for q in (0, 1):
                for s, esize in enumerate(SIZES):
                    variant = g.make(esize, q, sign, alt)
                    if variant is not None:
                        yield (s | q << 2 | sign << 3 | alt << 4,
                               f"{esize}{width(q)}{'u' if sign else 's'}"
                               f"{'a' if alt else ''}", variant)


def gadget_name(g: Group, suffix: str) -> str:
    return f"neon_{g.name}_{suffix}"


def emit(asm) -> None:
    if not GROUPS:
        define_groups()
    for g in GROUPS:
        for index, suffix, variant in variants_of(g):
            # The variants that run under the standard FPSCR value: all of an
            # FP_ALL group, those with the alt bit of an FP_ALT one.
            floating = g.fp == FP_ALL or (
                g.fp == FP_ALT and g.index == INDEX_GENERIC and
                (index >> 4 & 1) != 0)
            asm.gadget(gadget_name(g, suffix),
                       "\n".join(render(g, variant, floating)))


def header_tables(names) -> list:
    """The C++ metadata and gadget tables; `names` maps gadget names to
    their index in G."""
    if not GROUPS:
        define_groups()
    out = ["/* Advanced SIMD: the groups (a32_neon.py). */",
           "enum class NeonShape : std::uint8_t {"]
    out += [f"\t{s}," for s in SHAPES]
    out += ["};", "enum class NeonSelector : std::uint8_t {"]
    out += [f"\t{s}," for s in SELECTORS]
    out += ["};", "enum class NeonIndex : std::uint8_t {"]
    out += [f"\t{s}," for s in INDEX_NAMES]
    out += ["};", "enum class NeonFp : std::uint8_t {"]
    out += [f"\t{s}," for s in FP_NAMES]
    out += ["};", "enum class NeonScalar : std::uint8_t { never, always, alt };",
            "struct NeonGroup final {",
            "\tstd::uint16_t base;", "\tstd::uint16_t count;",
            "\tNeonShape shape;", "\tNeonSelector selector;",
            "\tNeonIndex index;", "\tNeonFp fp;", "\tNeonScalar scalar;",
            "\tstd::int8_t shift;", "\tbool size_none;",
            "\tbool indexed;", "\tstd::uint8_t rot_shift;",
            "\tstd::uint8_t rot_mask;", "};", ""]
    base = 0
    rows = []
    entries = []
    for g in GROUPS:
        slots = ["G::count"] * g.count
        for index, suffix, variant in variants_of(g):
            name = gadget_name(g, suffix)
            slots[index] = f"static_cast<G>({names[name]})"
        elems = [(alt, variant.m == "elem")
                 for index, _suffix, variant in variants_of(g)
                 for alt in [index >> 4 & 1]]
        if elems and all(e for _, e in elems):
            scalar = "always"
        elif elems and all(e == bool(a) for a, e in elems) and \
                any(e for _, e in elems):
            scalar = "alt"
        else:
            scalar = "never"
        rows.append(
            f"\t{{{base}, {g.count}, NeonShape::{g.shape}, "
            f"NeonSelector::{g.selector}, NeonIndex::{INDEX_NAMES[g.index]},"
            f" NeonFp::{FP_NAMES[g.fp]}, NeonScalar::{scalar}, {g.shift_sign},"
            f" {'true' if g.size_none else 'false'},"
            f" {'true' if g.indexed else 'false'}, {g.rot_shift},"
            f" {g.rot_mask}}},  // {g.name}")
        entries.append(f"\t// {g.name}")
        for i in range(0, g.count, 8):
            entries.append("\t" + ", ".join(slots[i:i + 8]) + ",")
        base += g.count
    out.append("/* The groups the builder names. */")
    out.append("namespace neon_group {")
    for g in GROUPS:
        if g.name in ("vtrn", "vrev64"):
            out.append(f"inline constexpr std::uint8_t {g.name} = {g.ident};")
    out += ["}  // namespace neon_group", ""]
    out.append(f"inline constexpr NeonGroup kNeonGroups[{len(GROUPS)}] = {{")
    out += rows
    out += ["};", "",
            f"inline constexpr G kNeon[{base}] = {{"]
    out += entries
    out += ["};"]
    return out


define_groups()
