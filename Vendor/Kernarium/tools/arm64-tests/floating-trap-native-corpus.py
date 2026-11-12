#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record the A64 instruction corpus's trapped floating-point cases natively.

Apple's arm64 cores trap floating-point exceptions whose FPCR enable is
set, so the host decides what a trapped FP&SIMD instruction does. This
script writes a request for tools/arm64-tests/sme_native_oracle.cpp, which
runs each case on the calling host without SME state, and turns the
oracle's response into corpus records:

    floating-trap-native-corpus.py --request trap.request
    sme_native_oracle trap.request trap.response      (on an arm64 Mac)
    floating-trap-native-corpus.py native.jsonl trap.jsonl \\
        --response trap.response --response trap.response2

The input is a dump after the native SME step. The output is it with the
outcome of every register-only case of the arm64-floating-trap-scalar,
-vector and -control records replaced by the host's (and the one case name
that described the former trap contract corrected), and with the arm64-floating-trap-native records
after it, replacing any it held. Those records cover, by group:

- flush: FPRoundBase() (ISA_A64_xml_A_profile-2026-06) never traps the
  underflow of a flush to zero, so a tiny result under FZ, or FZ16 in half
  precision, completes with FPSR.UFC even with UFE set; FZ alone does not
  flush half precision, FCVT to half precision ignores FZ16, and an input
  flush raises Input Denormal;
- exact: with FPCR.UFE set an exact tiny result raises Underflow, in every
  form that rounds: products, sums, quotients, fused and pairwise forms,
  conversions, the maximum and minimum forms, and each step of a
  maximum or minimum reduction; the forms that do not round (FABS, FNEG,
  FMOV) and those whose result is never tiny (FSQRT, FRECPX, FRSQRTE) raise
  nothing;
- inexact: an inexact tiny result raises Underflow and Inexact;
- priority: several raised enabled exceptions, in one element or across
  lanes in either order, report one cause, the first of Input Denormal,
  Invalid Operation, Divide by Zero, Overflow, Underflow and Inexact;
- commit: a trap leaves FPSR, the destination and every other register as
  they were, raised unenabled flags included, while a completion with
  enables set commits every flag it raised.

Each authored case names the cause the architecture and that priority give
it, or none, and the script refuses a response that disagrees, that traps
with another syndrome than EC 0x2c with IL, TFV and one cause, or whose
trap changes any state.

A second request holds the cases that need FEAT_AFP or FEAT_BF16, which the
M1 lacks: FPCR.AH, which detects tininess after rounding, flushes with an
Inexact exception, leaves FMAX and FMIN unflushed, makes the estimate and
step forms raise nothing, and reports a denormal single- or
double-precision input as Input Denormal; FPCR.FIZ and FPCR.NEP; and the
BFloat16 conversions and widening multiply-add. --afp-request writes it,
and --afp-response adds its records from a FEAT_AFP and FEAT_BF16 host,
the M4:

    floating-trap-native-corpus.py --afp-request afp.request

A regeneration that leaves the requests unchanged needs no host: --carry
PACK copies the outcomes and records of the current pack instead. The
request and response formats are sme-native-corpus.py's, at SVL 0.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import importlib.util
import json
import pathlib
import struct
import sys
from dataclasses import dataclass, field

HERE = pathlib.Path(__file__).resolve().parent


def _load(name: str, path: pathlib.Path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


native = _load("sme_native_corpus", HERE / "sme-native-corpus.py")
corpus = native.corpus

SITE = "tools/arm64-tests/floating-trap-native-corpus.py"
PRODUCER = "arm64-floating-trap-native"
RERECORDED = ("arm64-floating-trap-scalar", "arm64-floating-trap-vector",
              "arm64-floating-trap-control")
# Producer case names that describe the former trap contract.
RENAMED = {"vector collects all lane causes":
           "vector reports one lane cause by priority"}
GROUPS = ("flush", "exact", "inexact", "priority", "commit", "afp", "bf16")
SVL = 0

IOC, DZC, OFC, UFC, IXC, IDC = 0x01, 0x02, 0x04, 0x08, 0x10, 0x80
FLAGS = 0x9F
QC = 1 << 27
IOE, DZE, OFE, UFE, IXE, IDE = (flag << 8 for flag in
                                (IOC, DZC, OFC, UFC, IXC, IDC))
FIZ, AH, NEP = 1 << 0, 1 << 1, 1 << 2
FZ16, FZ = 1 << 19, 1 << 24
SYNDROME = 0xB280_0000  # EC 0x2c, IL, TFV
CAUSES = {IDC: "ID", IOC: "IO", DZC: "DZ", OFC: "OF", UFC: "UF", IXC: "IX"}


def single(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def double(value: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def lanes(bits: int, *values: int) -> bytes:
    data = b"".join(value.to_bytes(bits // 8, "little") for value in values)
    return data + bytes(16 - len(data))


def s(*values: int) -> bytes:
    return lanes(32, *values)


def d(*values: int) -> bytes:
    return lanes(64, *values)


def h(*values: int) -> bytes:
    return lanes(16, *values)


ZERO = bytes(16)
ONE = single(1.0)
ONE_H = 0x3C00
BIG = single(2.0 ** 100)            # BIG * BIG overflows
EXACT = 0x1F00_0000                 # 2^-65: EXACT * EXACT is 2^-130
INEXACT = 0x1F00_0001               # INEXACT * EXACT is tiny and inexact
SUBNORMAL = 0x0008_0000             # 2^-130
MIN_NORMAL = 0x0080_0000            # 2^-126
INFINITY = 0x7F80_0000
SIGNALLING = 0x7FA0_0000
QUIET = 0x7FC0_0000
D_EXACT = 0x1F70_0000_0000_0000     # 2^-520: squared, 2^-1040
H_EXACT = 0x1C00                    # 2^-8: squared, 2^-16
H_INEXACT = 0x1C01
STEP = single(1.0 + 2.0 ** -23)     # STEP * STEP is inexact

# One instruction each, with V0 the destination and V1-V3 the sources.
FMUL_S = 0x1E22_0820    # FMUL S0, S1, S2
FMUL_D = 0x1E62_0820    # FMUL D0, D1, D2
FMUL_H = 0x1EE2_0820    # FMUL H0, H1, H2
FMUL_4S = 0x6E22_DC20   # FMUL V0.4S, V1.4S, V2.4S
FMUL_2S = 0x2E22_DC20   # FMUL V0.2S, V1.2S, V2.2S
FSUB_S = 0x1E22_3820    # FSUB S0, S1, S2
FADD_S = 0x1E22_2820    # FADD S0, S1, S2
FDIV_S = 0x1E22_1820    # FDIV S0, S1, S2
FDIV_4S = 0x6E22_FC20   # FDIV V0.4S, V1.4S, V2.4S
FMADD_S = 0x1F02_0C20   # FMADD S0, S1, S2, S3
FMULX_S = 0x5E22_DC20   # FMULX S0, S1, S2
FMLA_4S = 0x4E22_CC20   # FMLA V0.4S, V1.4S, V2.4S
FADDP_S = 0x7E30_D820   # FADDP S0, V1.2S
FCVTXN_S = 0x7E61_6820  # FCVTXN S0, D1
FCVT_SD = 0x1E62_4020   # FCVT S0, D1
FCVT_HS = 0x1E23_C020   # FCVT H0, S1
FCVTN_4H = 0x0E21_6820  # FCVTN V0.4H, V1.4S
SCVTF_4H = 0x0F10_E420  # SCVTF V0.4H, V1.4H, #16
FMLAL_4S = 0x4E22_EC20  # FMLAL V0.4S, V1.4H, V2.4H
FMAX_S = 0x1E22_4820    # FMAX S0, S1, S2
FMIN_S = 0x1E22_5820    # FMIN S0, S1, S2
FMAXNM_S = 0x1E22_6820  # FMAXNM S0, S1, S2
FMAXV_4S = 0x6E30_F820  # FMAXV S0, V1.4S
FMAXNMV_4S = 0x6E30_C820  # FMAXNMV S0, V1.4S
FABS_S = 0x1E20_C020    # FABS S0, S1
FNEG_S = 0x1E21_4020    # FNEG S0, S1
FMOV_S = 0x1E20_4020    # FMOV S0, S1
FSQRT_S = 0x1E21_C020   # FSQRT S0, S1
FRECPX_S = 0x5EA1_F820  # FRECPX S0, S1
FRSQRTE_S = 0x7EA1_D820  # FRSQRTE S0, S1
FRECPE_S = 0x5EA1_D820  # FRECPE S0, S1
FRECPS_S = 0x5E22_FC20  # FRECPS S0, S1, S2
BFCVT = 0x1E63_4020     # BFCVT H0, S1
BFCVTN_4H = 0x0EA1_6820  # BFCVTN V0.4H, V1.4S
BFMLALB = 0x2EC2_FC20   # BFMLALB V0.4S, V1.8H, V2.8H
BFDOT_2S = 0x2E42_FC20  # BFDOT V0.2S, V1.4H, V2.4H

HALF = ("floating_point", "fp16")
SCALAR = ("floating_point",)
VECTOR = ("advanced_simd",)
VECTOR_HALF = ("advanced_simd", "fp16")


@dataclass
class Draft:
    group: str
    name: str
    word: int
    fpcr: int
    v: dict[int, bytes]
    native: tuple[str, ...]
    # The reported cause the architecture gives, or 0 for a completion.
    expect: int
    fpsr: int = 0
    x: dict[int, int] = field(default_factory=dict)

    def wire(self):
        state = native.WireState(x={18: 0, **self.x}, fpcr=self.fpcr,
                                 fpsr=self.fpsr, v=dict(self.v))
        return native.WireCase(words=(self.word, native.SVC), entry=0,
                               state=state)


def flush_drafts() -> list[Draft]:
    out: list[Draft] = []

    def add(name, word, fpcr, v, need, expect=0):
        out.append(Draft("flush", name, word, fpcr, v, need, expect))
    add("FMUL S exact tiny, FZ UFE", FMUL_S, FZ | UFE,
        {1: s(EXACT), 2: s(EXACT)}, SCALAR)
    add("FMUL S inexact tiny, FZ UFE", FMUL_S, FZ | UFE,
        {1: s(INEXACT), 2: s(EXACT)}, SCALAR)
    add("FMUL S inexact tiny, FZ UFE IXE", FMUL_S, FZ | UFE | IXE,
        {1: s(INEXACT), 2: s(EXACT)}, SCALAR)
    add("FMUL D exact tiny, FZ UFE", FMUL_D, FZ | UFE,
        {1: d(D_EXACT), 2: d(D_EXACT)}, SCALAR)
    add("FMUL H exact tiny, FZ16 UFE", FMUL_H, FZ16 | UFE,
        {1: h(H_EXACT), 2: h(H_EXACT)}, HALF)
    add("FMUL H exact tiny, FZ UFE: FZ does not flush half precision",
        FMUL_H, FZ | UFE, {1: h(H_EXACT), 2: h(H_EXACT)}, HALF, UFC)
    add("FMUL 4S inexact tiny lane 2, FZ UFE", FMUL_4S, FZ | UFE,
        {1: s(ONE, ONE, INEXACT, ONE), 2: s(ONE, ONE, EXACT, ONE)}, VECTOR)
    add("FSUB S exact subnormal, FZ UFE", FSUB_S, FZ | UFE,
        {1: s(0x00C0_0000), 2: s(MIN_NORMAL)}, SCALAR)
    add("FCVT S from D exact tiny, FZ UFE", FCVT_SD, FZ | UFE,
        {1: d(double(2.0 ** -130))}, SCALAR)
    add("FCVT H from S exact tiny, FZ16 UFE: FPRoundCV ignores FZ16",
        FCVT_HS, FZ16 | UFE, {1: s(single(2.0 ** -16))}, HALF, UFC)
    add("FCVTN 4H exact tiny, FZ16 UFE: FPRoundCV ignores FZ16",
        FCVTN_4H, FZ16 | UFE, {1: s(single(2.0 ** -16), ONE, ONE, ONE)},
        VECTOR_HALF, UFC)
    add("SCVTF 4H fixed-point tiny, FZ16 UFE", SCVTF_4H, FZ16 | UFE,
        {1: h(1, 1, 1, 1)}, VECTOR_HALF)
    add("FRECPE S of 2^127, FZ UFE: the estimate flushes", FRECPE_S,
        FZ | UFE, {1: s(single(2.0 ** 127))}, SCALAR)
    add("FRECPE S of 2^127, UFE: the subnormal estimate", FRECPE_S, UFE,
        {1: s(single(2.0 ** 127))}, SCALAR)
    add("FMUL S subnormal input, FZ UFE", FMUL_S, FZ | UFE,
        {1: s(SUBNORMAL), 2: s(ONE)}, SCALAR)
    add("FMUL S subnormal input, FZ IDE UFE", FMUL_S, FZ | IDE | UFE,
        {1: s(SUBNORMAL), 2: s(ONE)}, SCALAR, IDC)
    return out


def exact_drafts() -> list[Draft]:
    out: list[Draft] = []

    def add(name, word, fpcr, v, need, expect=UFC):
        out.append(Draft("exact", name, word, fpcr, v, need, expect))
    add("FMUL S exact tiny, UFE", FMUL_S, UFE,
        {1: s(EXACT), 2: s(EXACT)}, SCALAR)
    add("FMUL S exact tiny, IXE", FMUL_S, IXE,
        {1: s(EXACT), 2: s(EXACT)}, SCALAR, 0)
    add("FMUL D exact tiny, UFE", FMUL_D, UFE,
        {1: d(D_EXACT), 2: d(D_EXACT)}, SCALAR)
    add("FMUL H exact tiny, UFE", FMUL_H, UFE,
        {1: h(H_EXACT), 2: h(H_EXACT)}, HALF)
    add("FMUL 4S exact tiny lane 0, UFE", FMUL_4S, UFE,
        {1: s(EXACT, ONE, ONE, ONE), 2: s(EXACT, ONE, single(2.0), ONE)},
        VECTOR)
    add("FMUL 2S with exact tiny upper lanes, UFE", FMUL_2S, UFE,
        {1: s(ONE, ONE, EXACT, ONE), 2: s(ONE, ONE, EXACT, ONE)}, VECTOR, 0)
    add("FSUB S exact subnormal, UFE", FSUB_S, UFE,
        {1: s(0x00C0_0000), 2: s(MIN_NORMAL)}, SCALAR)
    add("FADD S subnormal plus zero, UFE", FADD_S, UFE,
        {1: s(SUBNORMAL), 2: ZERO}, SCALAR)
    add("FDIV S exact tiny, UFE", FDIV_S, UFE,
        {1: s(MIN_NORMAL), 2: s(single(4.0))}, SCALAR)
    add("FMADD S exact tiny, UFE", FMADD_S, UFE,
        {1: s(EXACT), 2: s(EXACT), 3: ZERO}, SCALAR)
    add("FMULX S exact tiny, UFE", FMULX_S, UFE,
        {1: s(EXACT), 2: s(EXACT)}, VECTOR)
    add("FMLA 4S exact tiny, UFE", FMLA_4S, UFE,
        {0: ZERO, 1: s(EXACT, ONE, ONE, ONE),
         2: s(EXACT, ONE, single(2.0), ONE)}, VECTOR)
    add("FADDP S exact subnormal, UFE", FADDP_S, UFE,
        {1: s(0x00C0_0000, 0x8080_0000)}, VECTOR)
    add("FCVTXN S exact tiny, UFE", FCVTXN_S, UFE,
        {1: d(double(2.0 ** -130))}, VECTOR)
    add("FCVT S from D exact tiny, UFE", FCVT_SD, UFE,
        {1: d(double(2.0 ** -130))}, SCALAR)
    add("FCVT H from S exact tiny, UFE", FCVT_HS, UFE,
        {1: s(single(2.0 ** -16))}, HALF)
    add("FCVTN 4H exact tiny, UFE", FCVTN_4H, UFE,
        {1: s(single(2.0 ** -16), ONE, ONE, ONE)}, VECTOR_HALF)
    add("SCVTF 4H fixed-point tiny, UFE", SCVTF_4H, UFE,
        {1: h(1, 1, 1, 1)}, VECTOR_HALF)
    add("FMLAL 4S subnormal accumulator, UFE", FMLAL_4S, UFE,
        {0: s(SUBNORMAL, ONE, ONE, ONE), 1: ZERO, 2: ZERO},
        ("advanced_simd", "fhm"))
    add("FMAX S passes a subnormal, UFE", FMAX_S, UFE,
        {1: s(SUBNORMAL), 2: s(single(-1.0))}, SCALAR)
    add("FMIN S passes a subnormal, UFE", FMIN_S, UFE,
        {1: s(SUBNORMAL), 2: s(ONE)}, SCALAR)
    add("FMAXNM S passes a subnormal, UFE", FMAXNM_S, UFE,
        {1: s(SUBNORMAL), 2: s(single(-1.0))}, SCALAR)
    add("FMAXV 4S subnormal maximum, UFE", FMAXV_4S, UFE,
        {1: s(SUBNORMAL, 0x8000_0001, 0x8080_0000, 0x8080_0000)}, VECTOR)
    add("FMAXV 4S subnormal intermediate maximum, UFE", FMAXV_4S, UFE,
        {1: s(SUBNORMAL, 0x0004_0000, ONE, 0)}, VECTOR)
    add("FMAXNMV 4S quiet NaN and subnormal, UFE", FMAXNMV_4S, UFE,
        {1: s(QUIET, SUBNORMAL, single(-1.0), single(-2.0))}, VECTOR)
    add("FABS S subnormal, UFE", FABS_S, UFE, {1: s(SUBNORMAL)}, SCALAR, 0)
    add("FNEG S subnormal, UFE", FNEG_S, UFE, {1: s(SUBNORMAL)}, SCALAR, 0)
    add("FMOV S subnormal, UFE", FMOV_S, UFE, {1: s(SUBNORMAL)}, SCALAR, 0)
    add("FSQRT S subnormal, UFE", FSQRT_S, UFE, {1: s(SUBNORMAL)}, SCALAR, 0)
    add("FRECPX S subnormal, UFE", FRECPX_S, UFE, {1: s(SUBNORMAL)},
        VECTOR, 0)
    add("FRSQRTE S of 2^127, UFE", FRSQRTE_S, UFE,
        {1: s(single(2.0 ** 127))}, VECTOR, 0)
    return out


def inexact_drafts() -> list[Draft]:
    out: list[Draft] = []

    def add(name, word, fpcr, v, need, expect):
        out.append(Draft("inexact", name, word, fpcr, v, need, expect))
    add("FMUL S inexact tiny, UFE", FMUL_S, UFE,
        {1: s(INEXACT), 2: s(EXACT)}, SCALAR, UFC)
    add("FMUL S inexact tiny, IXE", FMUL_S, IXE,
        {1: s(INEXACT), 2: s(EXACT)}, SCALAR, IXC)
    add("FMUL S inexact tiny, OFE", FMUL_S, OFE,
        {1: s(INEXACT), 2: s(EXACT)}, SCALAR, 0)
    add("FMUL H inexact tiny, UFE", FMUL_H, UFE,
        {1: h(H_INEXACT), 2: h(H_EXACT)}, HALF, UFC)
    # Tiny before rounding, so tiny without FPCR.AH, though it rounds to
    # the minimum normal number.
    add("FMUL S rounding to the minimum normal, UFE", FMUL_S, UFE,
        {1: s(single(1.0 + 2.0 ** -23)), 2: s(0x007F_FFFF)}, SCALAR, UFC)
    return out


def priority_drafts() -> list[Draft]:
    out: list[Draft] = []

    def add(name, word, fpcr, v, need, expect):
        out.append(Draft("priority", name, word, fpcr, v, need, expect))
    third = single(3.0)
    add("FMUL 4S UF lane 0, OF lane 1, OFE UFE", FMUL_4S, OFE | UFE,
        {1: s(EXACT, BIG, ONE, ONE), 2: s(EXACT, BIG, ONE, ONE)}, VECTOR,
        OFC)
    add("FMUL 4S OF lane 0, UF lane 1, OFE UFE", FMUL_4S, OFE | UFE,
        {1: s(BIG, EXACT, ONE, ONE), 2: s(BIG, EXACT, ONE, ONE)}, VECTOR,
        OFC)
    add("FMUL 4S IX lane 0, UF lane 1, UFE IXE", FMUL_4S, UFE | IXE,
        {1: s(STEP, EXACT, ONE, ONE), 2: s(STEP, EXACT, ONE, ONE)}, VECTOR,
        UFC)
    add("FMUL 4S IX lane 0, OF lane 1, OFE IXE", FMUL_4S, OFE | IXE,
        {1: s(STEP, BIG, ONE, ONE), 2: s(STEP, BIG, ONE, ONE)}, VECTOR, OFC)
    add("FMUL 4S UF lane 0, IO lane 1, IOE UFE", FMUL_4S, IOE | UFE,
        {1: s(EXACT, 0, ONE, ONE), 2: s(EXACT, INFINITY, ONE, ONE)}, VECTOR,
        IOC)
    add("FMUL 4S OF lane 0, IO lane 1, IOE OFE", FMUL_4S, IOE | OFE,
        {1: s(BIG, 0, ONE, ONE), 2: s(BIG, INFINITY, ONE, ONE)}, VECTOR, IOC)
    add("FDIV 4S OF lane 0, DZ lane 1, DZE OFE", FDIV_4S, DZE | OFE,
        {1: s(BIG, ONE, ONE, ONE), 2: s(single(2.0 ** -100), 0, ONE, ONE)},
        VECTOR, DZC)
    add("FDIV 4S UF lane 0, DZ lane 1, DZE UFE", FDIV_4S, DZE | UFE,
        {1: s(MIN_NORMAL, ONE, ONE, ONE), 2: s(single(4.0), 0, ONE, ONE)},
        VECTOR, DZC)
    add("FDIV 4S IX lane 0, IO lane 1, DZ lane 2, IOE DZE IXE", FDIV_4S,
        IOE | DZE | IXE,
        {1: s(ONE, 0, ONE, ONE), 2: s(third, 0, 0, ONE)}, VECTOR, IOC)
    add("FDIV 4S DZ lane 0, IO lane 1, IOE DZE", FDIV_4S, IOE | DZE,
        {1: s(ONE, 0, ONE, ONE), 2: s(0, 0, ONE, ONE)}, VECTOR, IOC)
    add("FDIV 4S IX lane 0, DZ lane 3, DZE IXE", FDIV_4S, DZE | IXE,
        {1: s(ONE, ONE, ONE, ONE), 2: s(third, ONE, ONE, 0)}, VECTOR, DZC)
    add("FDIV 4S DZ lane 0, IX lane 3, DZE IXE", FDIV_4S, DZE | IXE,
        {1: s(ONE, ONE, ONE, ONE), 2: s(0, ONE, ONE, third)}, VECTOR, DZC)
    add("FDIV 4S IO lane 0, ID lane 1, FZ IOE IDE", FDIV_4S,
        FZ | IOE | IDE,
        {1: s(0, SUBNORMAL, ONE, ONE), 2: s(0, ONE, ONE, ONE)}, VECTOR, IDC)
    add("FMUL 4S OF lane 0, ID lane 1, FZ OFE IDE", FMUL_4S,
        FZ | OFE | IDE,
        {1: s(BIG, SUBNORMAL, ONE, ONE), 2: s(BIG, ONE, ONE, ONE)}, VECTOR,
        IDC)
    add("FDIV S subnormal by zero, FZ IOE IDE", FDIV_S, FZ | IOE | IDE,
        {1: s(SUBNORMAL), 2: ZERO}, SCALAR, IDC)
    add("FMUL S subnormal by infinity, FZ IOE IDE", FMUL_S, FZ | IOE | IDE,
        {1: s(SUBNORMAL), 2: s(INFINITY)}, SCALAR, IDC)
    add("FMUL S overflow, OFE IXE", FMUL_S, OFE | IXE,
        {1: s(BIG), 2: s(BIG)}, SCALAR, OFC)
    add("FMUL S inexact tiny, UFE IXE", FMUL_S, UFE | IXE,
        {1: s(INEXACT), 2: s(EXACT)}, SCALAR, UFC)
    return out


def commit_drafts() -> list[Draft]:
    out: list[Draft] = []

    def add(name, word, fpcr, v, need, expect, fpsr):
        out.append(Draft("commit", name, word, fpcr, v, need, expect, fpsr))
    third = single(3.0)
    add("FDIV S by zero, DZE, from QC IXC IDC", FDIV_S, DZE,
        {1: s(ONE), 2: ZERO}, SCALAR, DZC, QC | IXC | IDC)
    add("FDIV S by zero, DZE, from DZC", FDIV_S, DZE,
        {1: s(ONE), 2: ZERO}, SCALAR, DZC, DZC)
    add("FDIV S inexact, IXE, from QC DZC", FDIV_S, IXE,
        {1: s(ONE), 2: s(third)}, SCALAR, IXC, QC | DZC)
    add("FDIV S inexact, DZE, from QC DZC", FDIV_S, DZE,
        {1: s(ONE), 2: s(third)}, SCALAR, 0, QC | DZC)
    add("FMUL S overflow, IXE", FMUL_S, IXE,
        {1: s(BIG), 2: s(BIG)}, SCALAR, IXC, 0)
    add("FMUL S overflow, UFE", FMUL_S, UFE,
        {1: s(BIG), 2: s(BIG)}, SCALAR, 0, 0)
    add("FMUL S inexact tiny, IXE, from QC", FMUL_S, IXE,
        {1: s(INEXACT), 2: s(EXACT)}, SCALAR, IXC, QC)
    add("FDIV 4S IX lane 0, IO lane 1, DZ lane 2, DZE", FDIV_4S, DZE,
        {0: s(ONE, ONE, ONE, ONE), 1: s(ONE, 0, ONE, ONE),
         2: s(third, 0, 0, ONE)}, VECTOR, DZC, 0)
    add("FDIV 4S IX lane 0, IO lane 1, DZ lane 2, IOE", FDIV_4S, IOE,
        {0: s(ONE, ONE, ONE, ONE), 1: s(ONE, 0, ONE, ONE),
         2: s(third, 0, 0, ONE)}, VECTOR, IOC, 0)
    add("FMLA 4S overflow lane 3, OFE", FMLA_4S, OFE,
        {0: s(ONE, ONE, ONE, ONE), 1: s(ONE, ONE, ONE, BIG),
         2: s(ONE, ONE, ONE, BIG)}, VECTOR, OFC, 0)
    return out


def afp_drafts() -> list[Draft]:
    """The cases that need FEAT_AFP: FPCR.AH, FIZ and NEP."""
    out: list[Draft] = []

    def add(name, word, fpcr, v, need, expect):
        out.append(Draft("afp", name, word, fpcr, v, need + ("afp",),
                         expect))
    add("FMAX S passes a subnormal, AH FZ UFE: unflushed", FMAX_S,
        AH | FZ | UFE, {1: s(SUBNORMAL), 2: s(single(-1.0))}, SCALAR, UFC)
    add("FMAXNM S passes a subnormal, AH FZ UFE: flushed", FMAXNM_S,
        AH | FZ | UFE, {1: s(SUBNORMAL), 2: s(single(-1.0))}, SCALAR, 0)
    add("FMAXNM S passes a subnormal, AH FZ IXE: the flush is inexact",
        FMAXNM_S, AH | FZ | IXE, {1: s(SUBNORMAL), 2: s(single(-1.0))},
        SCALAR, IXC)
    add("FMUL S exact tiny, AH FZ UFE", FMUL_S, AH | FZ | UFE,
        {1: s(EXACT), 2: s(EXACT)}, SCALAR, 0)
    add("FMUL S exact tiny, AH UFE", FMUL_S, AH | UFE,
        {1: s(EXACT), 2: s(EXACT)}, SCALAR, UFC)
    add("FMUL S rounding to the minimum normal, AH UFE: not tiny",
        FMUL_S, AH | UFE,
        {1: s(single(1.0 + 2.0 ** -23)), 2: s(0x007F_FFFF)}, SCALAR, 0)
    add("FMUL S subnormal input, AH FZ IDE: unflushed, Input Denormal",
        FMUL_S, AH | FZ | IDE, {1: s(SUBNORMAL), 2: s(ONE)}, SCALAR, IDC)
    add("FMUL S subnormal input, FIZ IDE: flushed without IDC", FMUL_S,
        FIZ | IDE, {1: s(SUBNORMAL), 2: s(ONE)}, SCALAR, 0)
    add("FRECPS S signalling NaN, AH IOE: no exceptions", FRECPS_S,
        AH | IOE, {1: s(SIGNALLING), 2: s(ONE)}, VECTOR, 0)
    add("FMUL S normal, NEP UFE: the merged upper lanes do not trap",
        FMUL_S, NEP | UFE,
        {1: s(ONE, SUBNORMAL, SUBNORMAL, SUBNORMAL), 2: s(ONE)}, SCALAR, 0)
    return out


def bf16_drafts() -> list[Draft]:
    """The BFloat16 cases, which need FEAT_BF16."""
    out: list[Draft] = []

    def add(name, word, fpcr, v, need, expect):
        out.append(Draft("bf16", name, word, fpcr, v, need + ("bf16",),
                         expect))
    tiny = single(2.0 ** -130)  # a BFloat16 subnormal, exactly
    add("BFCVT exact tiny, UFE", BFCVT, UFE, {1: s(tiny)}, SCALAR, UFC)
    add("BFCVT exact tiny, FZ UFE", BFCVT, FZ | UFE, {1: s(tiny)}, SCALAR, 0)
    add("BFCVTN 4H exact tiny, UFE", BFCVTN_4H, UFE,
        {1: s(tiny, ONE, ONE, ONE)}, VECTOR, UFC)
    add("BFMLALB subnormal accumulator, UFE", BFMLALB, UFE,
        {0: s(SUBNORMAL, ONE, ONE, ONE), 1: ZERO, 2: ZERO}, VECTOR, UFC)
    add("BFDOT 2S tiny products, UFE: BFRound raises nothing", BFDOT_2S,
        UFE, {0: ZERO, 1: h(0x0080, 0x0080, 0x0080, 0x0080),
              2: h(0x0080, 0x0080, 0x0080, 0x0080)}, VECTOR, 0)
    return out


def drafts() -> tuple[Draft, ...]:
    return tuple(flush_drafts() + exact_drafts() + inexact_drafts() +
                 priority_drafts() + commit_drafts())


def afp_host_drafts() -> tuple[Draft, ...]:
    return tuple(afp_drafts() + bf16_drafts())


# --------------------------------------------------------------------------
# The rerecorded producer cases


def rerecorded(lines: list[str]) -> list[tuple[int, int]]:
    """(line, case) indices of the cases whose outcome the host decides:
    every register-only case of the producer's trap records. The two
    trap-control cases with a data region load and store through SIMD&FP
    registers, which no enable makes transactional, and stay derived."""
    out = []
    for index, line in enumerate(lines):
        item = json.loads(line)
        if item.get("producer") in RERECORDED:
            out += [(index, case) for case, body in enumerate(item["cases"])
                    if not body.get("memory")]
    return out


def number(fields: dict, name: str, default: int = 0) -> int:
    return int(fields[name], 16) if name in fields else default


def case_wire(record: dict, case: dict):
    """A producer case as a request case: its program, and its initial
    state with X18 zero, which the oracle needs and none of these programs
    reads or writes."""
    program = record["programs"][case["program"]]
    words = tuple(int(word, 16) for _offset, word in program["words"])
    initial = case["initial"]
    if program["entry"] != 0 or words[-1] != native.SVC or \
            number(initial, "x18") or case.get("memory") or \
            set(initial) - {f"x{i}" for i in range(31)} - \
            {f"v{i}" for i in range(32)} - \
            {"sp", "nzcv", "fpcr", "fpsr", "tls"}:
        raise SystemExit(f"{record['record']}: {case['name']}: not a "
                         "register-only program")
    x = {i: number(initial, f"x{i}") for i in range(31)
         if f"x{i}" in initial}
    if "sp" in initial:
        x[31] = number(initial, "sp")
    x[18] = 0
    v = {i: bytes.fromhex(initial[f"v{i}"]) for i in range(32)
         if f"v{i}" in initial}
    state = native.WireState(x=x, nzcv=number(initial, "nzcv"),
                             fpcr=number(initial, "fpcr"),
                             fpsr=number(initial, "fpsr"), v=v)
    return native.WireCase(words=words, entry=0, state=state)


def request_cases(lines: list[str]) -> list:
    cases = []
    for index, case in rerecorded(lines):
        record = json.loads(lines[index])
        cases.append(case_wire(record, record["cases"][case]))
    return cases + [draft.wire() for draft in drafts()]


def request_bytes(lines: list[str]) -> bytes:
    return native.write_request(request_cases(lines), SVL)


def afp_request_bytes() -> bytes:
    return native.write_request([draft.wire() for draft in
                                 afp_host_drafts()], SVL)


# --------------------------------------------------------------------------
# Outcomes


class Finding(Exception):
    pass


def outcome(case, result, name: str, undefined: bool = False) -> dict:
    """The event, expected state, retired count and syndrome the host gave
    a case, which must complete at its SVC, trap with one cause, or, where
    `undefined` allows it, take an undefined instruction. A trap at the
    entry changes no state."""
    events = ("system-call", "floating-point-trap") + \
        (("illegal-instruction",) if undefined else ())
    if result.event not in events:
        raise Finding(f"{name}: {result.event} (ESR {result.esr:#010x})")
    initial = native.Dense.base(SVL).apply(case.state)
    final = initial.apply(result.state)
    completed = result.event == "system-call"
    pc_offset = result.pc_offset + (4 if completed else 0)
    if result.pc_offset % 4 or result.pc_offset // 4 >= len(case.words) or \
            completed and case.words[result.pc_offset // 4] != native.SVC:
        raise Finding(f"{name}: stopped at offset {result.pc_offset:#x}")
    expected = native.expected_fields(
        initial, final, corpus.CODE_BASE + pc_offset,
        corpus.CODE_BASE + case.entry)
    item = {"event": result.event, "expected": expected,
            "retired": pc_offset // 4}
    if result.event == "floating-point-trap":
        cause = result.esr & FLAGS
        if result.esr & ~FLAGS != SYNDROME or cause & (cause - 1) or \
                not cause:
            raise Finding(f"{name}: syndrome {result.esr:#010x} is not one "
                          "trapped cause")
        if expected and result.pc_offset == case.entry:
            raise Finding(f"{name}: the trap changed " +
                          ", ".join(sorted(expected)))
        item["syndrome"] = f"{result.esr:#010x}"
    return item


def check_expectation(draft: Draft, item: dict) -> None:
    got = int(item["syndrome"], 16) & FLAGS if "syndrome" in item else 0
    if got != draft.expect:
        raise Finding(f"{draft.name}: {CAUSES.get(got, 'completes')}, the "
                      f"architecture gives "
                      f"{CAUSES.get(draft.expect, 'completes')}")


def initial_fields(draft: Draft) -> dict:
    base = native.Dense.base(SVL)
    out = {"x18": native.hex64(0)} if base.x[18] else {}
    for i, value in draft.x.items():
        if value != base.x[i]:
            out["sp" if i == 31 else f"x{i}"] = native.hex64(value)
    for name in ("fpcr", "fpsr"):
        if getattr(draft, name):
            out[name] = native.hex64(getattr(draft, name))
    for i, value in draft.v.items():
        if value != base.v[i]:
            out[f"v{i}"] = value.hex()
    return out


def case_skeleton(draft: Draft, program: int) -> dict:
    """The record case without what the host decides."""
    return {"name": draft.name, "program": program, "ordinals": [0, 0],
            "invalid_encoding": False, "minimization": "full",
            "native": list(draft.native), "initial": initial_fields(draft)}


def build_records(items: tuple[Draft, ...], results: list
                  ) -> tuple[list[dict], dict]:
    records: dict[str, dict] = {}
    programs: dict[str, dict[int, int]] = {}
    events: dict[str, int] = {}
    findings = []
    for draft, result in zip(items, results):
        record = records.setdefault(draft.group, {
            "record": f"{PRODUCER}/{draft.group}", "producer": PRODUCER,
            "site": SITE, "profiles": ["4k"], "options": [],
            "programs": [], "cases": []})
        index = programs.setdefault(draft.group, {})
        if draft.word not in index:
            index[draft.word] = len(record["programs"])
            record["programs"].append({
                "entry": 0, "words": [[0, f"{draft.word:08x}"],
                                      [4, f"{native.SVC:08x}"]]})
        try:
            item = outcome(draft.wire(), result, draft.name)
            check_expectation(draft, item)
        except Finding as finding:
            findings.append(str(finding))
            continue
        record["cases"].append(dict(case_skeleton(draft,
                                                  index[draft.word]),
                                    **item))
        events[item["event"]] = events.get(item["event"], 0) + 1
    if findings:
        for finding in findings:
            print(f"floating-trap-native-corpus: {finding}", file=sys.stderr)
        raise SystemExit(f"floating-trap-native-corpus: {len(findings)} "
                         "cases disagree with the architecture")
    ordered = [records[group] for group in GROUPS if group in records]
    return ordered, dict(sorted(events.items()))


OUTCOME_KEYS = ("event", "expected", "retired", "syndrome")


def apply_outcomes(lines: list[str], items: list[dict]) -> int:
    """Puts each rerecorded case's outcome in place; returns how many
    changed."""
    changed = 0
    by_line: dict[int, dict] = {}
    for (index, case), item in zip(rerecorded(lines), items):
        record = by_line.setdefault(index, json.loads(lines[index]))
        target = record["cases"][case]
        before = {key: target.get(key) for key in OUTCOME_KEYS}
        for key in OUTCOME_KEYS:
            target.pop(key, None)
        target.update(item)
        target["name"] = RENAMED.get(target["name"], target["name"])
        if before != {key: target.get(key) for key in OUTCOME_KEYS}:
            changed += 1
    for index, record in by_line.items():
        lines[index] = json.dumps(record, sort_keys=True)
    return changed


# --------------------------------------------------------------------------
# Dumps


def script_hash() -> str:
    return hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()


def read_responses(paths: list[pathlib.Path], cases: list, names: list[str]
                   ) -> list:
    responses = [path.read_bytes() for path in paths]
    first = native.read_response(responses[0], cases, SVL)[1]
    for path, response in zip(paths[1:], responses[1:]):
        if response != responses[0]:
            other = native.read_response(response, cases, SVL)[1]
            for name, one, two in zip(names, first, other):
                if one != two:
                    print(f"floating-trap-native-corpus: {path}: {name} "
                          "differs", file=sys.stderr)
            raise SystemExit("floating-trap-native-corpus: the responses "
                             "differ")
    return first


def strip_native(lines: list[str]) -> list[str]:
    return [line for line in lines
            if json.loads(line).get("producer") != PRODUCER]


def write_dump(path: pathlib.Path, header: dict, lines: list[str],
               records: list[dict]) -> None:
    platform = corpus.feature_tokens()
    used = {token for record in records for case in record["cases"]
            for token in case["native"]}
    tokens = list(header["feature_tokens"])
    header["feature_tokens"] = tokens + [token for token in platform
                                         if token in used and
                                         token not in tokens]
    with path.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in lines:
            sink.write(line + "\n")
        for record in records:
            sink.write(json.dumps(record, sort_keys=True) + "\n")


def afp_records(options: argparse.Namespace) -> tuple[list[dict], dict]:
    items = afp_host_drafts()
    cases = [draft.wire() for draft in items]
    results = read_responses(options.afp_response, cases,
                             [draft.name for draft in items])
    for draft, result in zip(items, results):
        # FPCR.AH, FIZ and NEP are RES0 without FEAT_AFP, and the
        # BFloat16 forms UNDEFINED without FEAT_BF16.
        if result.state.fpcr != draft.fpcr or \
                result.event == "illegal-instruction":
            raise SystemExit(f"floating-trap-native-corpus: {draft.name}: "
                             "the response is not from a FEAT_AFP and "
                             "FEAT_BF16 host")
    records, events = build_records(items, results)
    return records, {"request_sha256":
                     hashlib.sha256(afp_request_bytes()).hexdigest(),
                     "cases": len(items), "events": events}


def append(options: argparse.Namespace) -> int:
    header, lines = native.read_dump(options.input)
    lines = strip_native(lines)
    cases = request_cases(lines)
    rerecords = len(rerecorded(lines))
    items = drafts()
    names = [f"rerecorded case {index}" for index in range(rerecords)] + \
        [draft.name for draft in items]
    results = read_responses(options.response, cases, names)
    try:
        outcomes = [outcome(case, result, name, undefined=True)
                    for case, result, name in
                    zip(cases[:rerecords], results[:rerecords], names)]
    except Finding as finding:
        raise SystemExit(f"floating-trap-native-corpus: {finding}")
    changed = apply_outcomes(lines, outcomes)
    records, events = build_records(items, results[rerecords:])
    meta = {"script": SITE, "sha256": script_hash(),
            "request_sha256": hashlib.sha256(
                native.write_request(cases, SVL)).hexdigest(),
            "rerecorded": {"cases": rerecords, "changed": changed},
            "cases": len(items), "events": events}
    if options.afp_response:
        afp, meta["afp"] = afp_records(options)
        records += afp
        records.sort(key=lambda record: GROUPS.index(
            record["record"].split("/", 1)[1]))
    header["meta"]["floating_trap_native"] = meta
    write_dump(options.output, header, lines, records)
    print(f"rerecorded {rerecords} cases ({changed} changed) and appended "
          f"{sum(len(record['cases']) for record in records)} native "
          f"floating-point trap cases in {len(records)} records")
    return 0


def carry(options: argparse.Namespace) -> int:
    header, lines = native.read_dump(options.input)
    lines = strip_native(lines)
    pack, tokens = corpus.decode_pack(options.carry.read_bytes())
    meta = pack.meta.get("floating_trap_native")
    if meta is None or meta["request_sha256"] != \
            hashlib.sha256(request_bytes(lines)).hexdigest() or \
            ("afp" in meta and meta["afp"]["request_sha256"] !=
             hashlib.sha256(afp_request_bytes()).hexdigest()):
        raise SystemExit("floating-trap-native-corpus: the request changed "
                         "since the pack's native records; run the oracle "
                         "again")
    kept = [record for record in pack.records
            if record.producer == PRODUCER or record.producer in RERECORDED]
    dumped = {item["record"]: item for item in map(json.loads,
              corpus.dump_corpus(corpus.Corpus(meta=pack.meta, blobs={},
                                               records=kept), tokens))
              if "cases" in item}
    outcomes = []
    for index, case in rerecorded(lines):
        record = json.loads(lines[index])
        source = dumped[record["record"]]["cases"][case]
        outcomes.append({key: source[key] for key in OUTCOME_KEYS
                         if key in source})
    apply_outcomes(lines, outcomes)
    records = [item for item in dumped.values()
               if item["producer"] == PRODUCER]
    header["meta"]["floating_trap_native"] = dict(meta, sha256=script_hash())
    write_dump(options.output, header, lines, records)
    print(f"carried {len(outcomes)} rerecorded outcomes and "
          f"{sum(len(item['cases']) for item in records)} native "
          f"floating-point trap cases")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("input", type=pathlib.Path, nargs="?")
    parser.add_argument("output", type=pathlib.Path, nargs="?")
    parser.add_argument("--request", type=pathlib.Path, nargs=2,
                        metavar=("DUMP", "REQUEST"),
                        help="write the request for DUMP's cases and exit")
    parser.add_argument("--afp-request", type=pathlib.Path,
                        help="write the FEAT_AFP and FEAT_BF16 host's "
                        "request and exit")
    parser.add_argument("--response", type=pathlib.Path, action="append",
                        default=[], help="a response to the request; "
                        "several must be identical")
    parser.add_argument("--afp-response", type=pathlib.Path,
                        action="append", default=[],
                        help="a FEAT_AFP and FEAT_BF16 host's response")
    parser.add_argument("--carry", type=pathlib.Path,
                        help="copy the outcomes and records of this pack")
    options = parser.parse_args()
    if options.request is not None:
        _header, lines = native.read_dump(options.request[0])
        data = request_bytes(strip_native(lines))
        options.request[1].write_bytes(data)
        print(f"wrote {len(request_cases(strip_native(lines)))} cases, "
              f"{len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}")
        return 0
    if options.afp_request is not None:
        data = afp_request_bytes()
        options.afp_request.write_bytes(data)
        print(f"wrote {len(afp_host_drafts())} cases, {len(data)} bytes, "
              f"sha256 {hashlib.sha256(data).hexdigest()}")
        return 0
    if options.input is None or options.output is None or \
            bool(options.response) == (options.carry is not None) or \
            (options.afp_response and options.carry is not None):
        parser.error("give INPUT OUTPUT and either --response or --carry")
    return carry(options) if options.carry is not None else append(options)


if __name__ == "__main__":
    sys.exit(main())
