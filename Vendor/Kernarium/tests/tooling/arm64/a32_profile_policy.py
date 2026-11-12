#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The AArch32 EL0 profile's choices on top of Arm's AArch32 XML.

The XML describes every AArch32 instruction of an Armv8-A processor. The
virtual CPU runs a smaller profile (docs/a32-instruction-coverage.md): EL0
only, no SWP or SETEND, no coprocessor access but a few CP15 registers, and
the Armv8 extensions only where the host has the A64 feature the engines
lower them onto. This module is the one place where those choices are made,
so a reader sees the whole profile at once and the generator can assert that
every name it mentions exists in the pinned XML.

Page identifiers below are the XML's form IDs (the instruction page, for
example LDM_u); encoding names are the XML's encoding names (VCVTB_A1_DH).
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True


# Pages whose every encoding is UNDEFINED at EL0, with the reason. The XML
# pseudocode of each is privileged (a mode change, a banked register, SPSR, a
# halting-debug instruction) or the profile leaves it out by decision (D6).
UNDEFINED_AT_EL0 = {
    "DCPS1_a32": "halting-debug",
    "DCPS2_a32": "halting-debug",
    "DCPS3_a32": "halting-debug",
    "ERET_a32": "exception-return",
    "HLT_a32": "halting-debug",
    "HVC_a32": "hypervisor-call",
    "SMC_a32": "secure-monitor-call",
    "LDM_e": "exception-return",
    "LDM_u": "user-registers",
    "STM_u": "user-registers",
    "MRS_br": "banked-register",
    "MSR_br": "banked-register",
    "RFE": "exception-return",
    "SRS": "exception-return",
    "SETEND": "decision-d6",
    "SETPAN": "privileged-state",
    "LDC_i": "coprocessor",
    "LDC_l": "coprocessor",
    "STC": "coprocessor",
}

# Pages that stay decoded but need a check the encoding table cannot make: the
# register or coprocessor operation a field selects. decode.cpp resolves them.
CHECKED_PAGES = {
    "MCR": "coprocessor",
    "MRC": "coprocessor",
    "MCRR": "coprocessor",
    "MRRC": "coprocessor",
    "MRS_a32": "apsr",
    "MSR_r": "apsr",
    "MSR_i": "apsr",
    "VMRS": "fpscr",
    "VMSR": "fpscr",
}

# Encodings the table decodes but whose every word the profile rejects: the
# coprocessor has no 64-bit register an EL0 program may write, and a Thumb
# exception return is UNPREDICTABLE at EL0 for every operand. The inventory
# marks them absent, and the corpus expects an illegal instruction.
NEVER_EXECUTED = {
    "MCRR_A1": "no 64-bit coprocessor register is writable at EL0",
    "MCRR_T1": "no 64-bit coprocessor register is writable at EL0",
    "SUBS_PC_T5_AS": "an exception return is UNPREDICTABLE at EL0",
}

# Pages that execute as a NOP at EL0 once decoded (CPS changes only mode and
# interrupt masks, which EL0 cannot).
NOP_AT_EL0 = {"CPS"}

# Pages that are Armv8 additions to the AArch32 profile, level complete and
# not simd. The load-acquire and store-release group (LDA, STL, LDAEX, STLEX
# and their sizes) and SEVL are not here: every Armv8 AArch32 EL0 has them,
# and arm64 Linux's compat kuser helpers run STLEX and STLEXD, so they are
# level base.
ARMV8_PAGES = {
    "VSEL", "VMAXNM", "VMINNM",
    "VRINTA_asimd", "VRINTM_asimd", "VRINTN_asimd", "VRINTP_asimd",
    "VRINTX_asimd", "VRINTZ_asimd",
    "VRINTA_vfp", "VRINTM_vfp", "VRINTN_vfp", "VRINTP_vfp",
    "VRINTR_vfp", "VRINTX_vfp", "VRINTZ_vfp",
    "VCVTA_asimd", "VCVTM_asimd", "VCVTN_asimd", "VCVTP_asimd",
    "VCVTA_vfp", "VCVTM_vfp", "VCVTN_vfp", "VCVTP_vfp",
}
# Individual encodings that are Armv8 additions on pages that also hold
# older encodings: the double-precision half-precision conversions.
ARMV8_ENCODINGS = {
    "VCVTB_A1_DH", "VCVTB_A1_HD", "VCVTB_T1_DH", "VCVTB_T1_HD",
    "VCVTT_A1_DH", "VCVTT_A1_HD", "VCVTT_T1_DH", "VCVTT_T1_HD",
}

# XML features the profile can have, and the Feature name decode.hpp uses. A
# form that names one decodes only when the caller's profile has it.
FEATURES = {
    "FEAT_FP16": "fp16",
    "FEAT_AES": "aes",
    "FEAT_SHA1": "sha1",
    "FEAT_SHA256": "sha2",
    "FEAT_CRC32": "crc32",
    "FEAT_DotProd": "dotprod",
    "FEAT_FHM": "fhm",
    "FEAT_AA32BF16": "bf16",
    "FEAT_AA32I8MM": "i8mm",
    "FEAT_SB": "sb",
    "FEAT_RDM": "rdm",
    "FEAT_FCMA": "fcma",
    "FEAT_JSCVT": "jscvt",
}
# FEAT_PMULL is not an encoding feature in the XML: VMULL.P64 checks it in
# its Decode pseudocode, and decode.hpp carries the same feature.
PROGRAM_FEATURES = {"FEAT_PMULL": "pmull"}
# XML features the profile never has. A form that names one executes as the
# hint NOP it replaces (ESB, TSB CSYNC, CLRBHB) or is outside the profile
# (SETPAN is also UNDEFINED_AT_EL0).
ABSENT_FEATURES = {"FEAT_RAS", "FEAT_TRF", "FEAT_CLRBHB", "FEAT_PAN"}

# Feature order of decode.hpp's Feature enum.
FEATURE_ORDER = (
    "fp16", "aes", "pmull", "sha1", "sha2", "crc32", "dotprod", "fhm",
    "bf16", "i8mm", "sb", "rdm", "fcma", "jscvt",
)
