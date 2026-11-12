#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Remove unimplemented architecture features from an A64 corpus dump.

usage: absent-feature-corpus.py [--features LIST] [--failing TSV]
                                [--report TSV] DUMP OUTPUT
       absent-feature-corpus.py --authored-only DUMP OUTPUT
       absent-feature-corpus.py --collect-failing TSV --source DUMP
                                [--previous TSV] [--features LIST]
                                DUMP LOG...

The virtual CPU implements none of FEAT_MTE, FEAT_GCS, FEAT_CPA, FEAT_PAuth,
and FEAT_EBF16, nor the memory extensions FEAT_LSE128, FEAT_THE
and FEAT_D128, FEAT_LRCPC3, FEAT_LSCP, FEAT_LS64 with its _V and _ACCDATA
forms, FEAT_LSFE, FEAT_LSUI, and FEAT_LOR, nor the FP/SIMD extensions FEAT_FP8
with its dot-product, multiply-add, and matrix forms and FPMR,
FEAT_F16F32DOT, FEAT_F16MM and FEAT_F16F32MM, FEAT_LUT, FEAT_FAMINMAX,
FEAT_SM3, FEAT_SM4, and FEAT_FPRCVT, nor FEAT_CMPBR, FEAT_HBC, FEAT_SSBS,
FEAT_XS, FEAT_WFxT, or any SVE or SME feature outside the SVE and SME
profiles: their non-hint instructions are UNDEFINED, their hint-space
instructions are NOPs, FPCR.EBF is RES0, and the ID registers report them
absent. The producer checks predate that choice, and their harness enabled
these features in most cases' state or ran their instructions, so this script
rewrites a dump after export. For each named feature it

- drops every case that exercises the feature: one that uses its harness
  option, terminal event, or validator relation, or needs it natively without
  expecting the undefined-instruction trap, which for FEAT_EBF16 means a
  case whose FPCR sets EBF;
- drops every case whose program holds one of the feature's instruction
  encodings, including the hint-space ones whose result the enabled feature
  set, but does not expect it to be undefined;
- strips the feature's state fields, FPCR bits, native requirement, harness
  option, and allocation tags from every other case and record, and drops
  the cases the --failing file names, which fail once that state is gone;
- appends the arm64-absent-<feature> records, which expect every encoding
  class of the feature to be undefined and its hints to be NOPs, and whose
  cases take their invalid-encoding bit from invalid_encoding.py: a word of
  an omitted feature is an invalid encoding, but a generic system word that
  names its register and a word that another virtual-CPU configuration
  executes are not; and
- replaces the coverage that the MTE cases of the --failing file took from
  each allocation with arm64-absent-mte-tbi cases.

FEAT_SME is two-sided, like FEAT_AFP below: the virtual CPU implements the
SME profile only where the host does, and sme-native-corpus.py appends the
cases observed on a host with it. FEAT_SVE is absent everywhere: no host
implements it and the virtual CPU presents none. The producer's SME model is
retired, so its SME cases and its one-byte PSTATE.SM and ZA state are dropped
and stripped as for an absent feature. The witness word of every row of the
generated encoding table (darwin/cpu/arm64/detail/sme_encodings.hpp), and the
evidence-binding witness of each SVE or SME entry without a row, is pinned
UNDEFINED at PSTATE.SM = 0 in the record whose option makes it so, which the
runner translates for on every host:

- the arm64-absent-sme record, under without_sme, holds SMSTART, SMSTOP,
  MRS and MSR SVCR and the other rows of the SME profile;
- the arm64-absent-sve record under without_sme holds the SM = 0
  vector-length family (CNT, INC, DEC, RDVL, ADDVL and ADDPL, and the LDR and
  STR of Z and P), which a virtual CPU with FEAT_SME executes outside
  streaming mode for a vector length of 16 bytes, a deliberate departure
  from the architecture (darwin/cpu/arm64/detail/sm0_vector_length.hpp;
  sm0-vl-corpus.py appends those cases), so it is UNDEFINED only where SME
  is;
- the unconditional arm64-absent-sve record holds every other row: the SVE
  rows that execute only in streaming mode or nowhere, which are UNDEFINED
  outside it on every virtual CPU, and the rows outside the SME profile.

A native FEAT_EBF16 requirement without FPCR.EBF set came from the engines'
old host gates, not from the case, so such a case stays and loses the
requirement. So did every FEAT_CSSC and FEAT_MOPS requirement: both engines
implement those features on every host, so the rewrite removes them from
every case, including one that expects the undefined-instruction trap, and
the JIT lane runs those cases on hosts without the features.

FEAT_AFP is different: the virtual CPU implements it exactly where the host
does (with the host features implements_afp() in
darwin/cpu/arm64/floating_controls.hpp also needs), so its cases stay. A case
whose FPCR sets FIZ, AH, or NEP keeps them and needs FEAT_AFP natively,
gaining the requirement where the producer's harness left it out, and the
runner runs it only where the virtual CPU implements the feature. A native
FEAT_AFP requirement without those controls set came from the old host
gates, like the FEAT_CSSC and FEAT_MOPS ones, and is removed. A case whose
entry MSR FPCR writes one of the controls that its FPCR state never holds
pins them RES0, as a virtual CPU without FEAT_AFP does; it moves to a
sibling record with the without_afp option, which the runner translates for
such a CPU on every host.

A failing case is one whose tagged address relied on the top-byte ignore that
enabled MTE implied. Each replacement takes a kept two-word case of the same
SIMD&FP load/store allocation that completes through a general-register base
with a zero top byte, sets tbi_enabled, and puts a nonzero tag in the base's
top byte. TBI excludes the top byte from the address and the base is the
instruction's only general register, so the architectural result is the kept
one, except that a written-back base keeps its tag.

Run it on the output of lse2-alignment-corpus.py and --repack the result. A
change of the encoding table or of the authored records alone needs no
export: --authored-only rewrites the arm64-absent-<feature> records of a
finished dump in place (the producers' cases, their stripped state, and the
TBI replacements stay) and refreshes the step's hashes. To
refresh the failing list, run the rewrite without --failing, run the corpus
test on that pack with --max-failures 0, and pass its log to --collect-failing
with the rewritten dump, the dump it was rewritten from as --source, and the
failing list in use as --previous. A failing case that list names keeps its
feature; any other is attributed to the last named feature whose state the
rewrite stripped from it, since features join the list in the order they are
retired and a newly failing case lost the newest state.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import csv
import functools
import hashlib
import importlib.util
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "build_unit_corpus", HERE / "build-unit-corpus.py")
corpus = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = corpus
_spec.loader.exec_module(corpus)
_spec = importlib.util.spec_from_file_location(
    "sme_encoding_table", HERE / "sme_encoding_table.py")
sme_table = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = sme_table
_spec.loader.exec_module(sme_table)
_spec = importlib.util.spec_from_file_location(
    "invalid_encoding", HERE / "invalid_encoding.py")
invalid_encoding = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = invalid_encoding
_spec.loader.exec_module(invalid_encoding)

SITE = "tools/arm64-tests/absent-feature-corpus.py"
DEFAULT_FEATURES = ("mte", "gcs", "cpa", "pauth", "ebf16", "lse128",
                    "the", "lrcpc3", "lscp", "ls64", "lsfe", "lsui", "lor",
                    "fp8", "f16f32dot", "f16mm", "lut", "faminmax", "sm3",
                    "sm4", "fprcvt", "cmpbr", "hbc", "ssbs", "sme", "xs",
                    "wfxt", "sve")
SVC = 0xD400_0001
# Native requirements that were only the producer JIT's host gates: both
# engines implement these features on every host.
HOST_GATES = ("cssc", "mops")
# FPCR.FIZ, AH, and NEP: FEAT_AFP's controls, which exist where the host
# implements the feature.
AFP_CONTROLS = 0x7
WITHOUT_AFP = "without_afp"
MSR_FPCR = (0xFFFF_FFE0, 0xD51B_4400)


class Feature:
    def __init__(self, tokens, conditions, fields, validators=(), events=(),
                 options=(), tags=False, encodings=(),
                 stripped_options=(), fpcr_bits=0, allocation=True) -> None:
        # Host and allocation feature tokens, and decode-partition conditions.
        self.tokens = frozenset(tokens)
        # False when the allocation table names the feature for a whole
        # baseline class: MSR (immediate) is filed under FEAT_SSBS, so the
        # SSBS form is named in encodings instead.
        self.allocation = allocation
        self.conditions = frozenset(conditions)
        self.fields = frozenset(fields)
        self.validators = frozenset(validators)
        self.events = frozenset(events)
        self.options = frozenset(options)
        # Harness options that nearly every producer check enabled. They are
        # removed from the records rather than dropping them.
        self.stripped_options = frozenset(stripped_options)
        self.tags = tags
        # (mask, value): system encodings that the allocation and partition
        # tables file under a baseline instruction (DC, MRS, MSR).
        self.encodings = tuple(encodings)
        # FPCR controls the feature defines; RES0 without it.
        self.fpcr_bits = fpcr_bits


FEATURES = {
    "mte": Feature(
        tokens=("mte", "mte2", "mtetc"),
        conditions=("FEAT_MTE", "FEAT_MTE2"),
        fields=("mte_enabled", "mte_exclude", "mte_tag_choices",
                "mte_random_tags", "mte_seed", "mte_tag", "mte_random_state",
                "pstate_tco"),
        validators=("permitted-tag-generator-choice/v1",),
        tags=True,
        encodings=(
            (0xFFFF_F0FF, 0xD503_409F),  # MSR TCO, #imm
            (0xFFDF_FFE0, 0xD51B_42E0),  # MRS and MSR TCO
            *((0xFFFF_FFE0, value) for value in (
                0xD50B_7460, 0xD50B_7480,  # DC GVA, GZVA
                0xD50B_7A60, 0xD50B_7AA0,  # DC CGVAC, CGDVAC
                0xD50B_7C60, 0xD50B_7CA0,  # DC CGVAP, CGDVAP
                0xD50B_7D60, 0xD50B_7DA0,  # DC CGVADP, CGDVADP
                0xD50B_7E60, 0xD50B_7EA0)),  # DC CIGVAC, CIGDVAC
        )),
    "gcs": Feature(
        tokens=("gcs",),
        conditions=("FEAT_GCS",),
        fields=("gcs_pointer", "gcs_implemented", "gcs_push_enabled",
                "gcs_store_enabled"),
        validators=("guarded-control-call-fault-atomicity/v1",
                    "guarded-control-return-fault/v1",
                    "guarded-control-zero-target-fault/v1"),
        events=("gcs-fault",),
        options=("enable_guarded_control_flow",),
        encodings=((0xFFDF_FFE0, 0xD51B_2520),)),  # MRS and MSR GCSPR_EL0
    "cpa": Feature(
        tokens=("cpa",),
        conditions=("FEAT_CPA",),
        fields=("cpa_enabled", "cpa_multiply_enabled")),
    # FEAT_PAuth_LR stays as it was: unimplemented, with its non-hint
    # encodings UNDEFINED and PACM a NOP, so its cases stay valid.
    "pauth": Feature(
        tokens=("pauth",),
        conditions=("FEAT_PAuth",),
        fields=("pointer_auth_keys", "pointer_authentication_enabled"),
        validators=("permitted-transformed-branch-failure/v1",
                    "permitted-transformed-code/v1",
                    "permitted-transformed-generic/v1",
                    "transformed-address-fault-state/v1"),
        stripped_options=("enable_pointer_authentication",),
        # The hint-space PAC*, AUT*, and XPACLRI forms signed, authenticated,
        # or stripped in the producer state.
        encodings=tuple((0xFFFF_FFFF, 0xD503_201F | immediate << 5)
                        for immediate in (7, 8, 10, 12, 14, *range(24, 32)))),
    # FEAT_EBF16 changes no encoding. Its FPCR.EBF control is RES0, so BFDOT
    # and BFMMLA always take the standard behavior.
    "ebf16": Feature(
        tokens=("ebf16",),
        conditions=("FEAT_EBF16",),
        fields=(),
        fpcr_bits=0x2000),
    # The memory extensions below add instructions and, for FEAT_LS64, the
    # ACCDATA_EL1 value ST64BV0 stores.
    "lse128": Feature(
        tokens=("lse128",),
        conditions=("FEAT_LSE128",),
        fields=()),
    # The read-check-write pairs need FEAT_D128 as well; the other FEAT_D128
    # instructions are EL1 system instructions, UNDEFINED at EL0 either way.
    "the": Feature(
        tokens=("the", "d128"),
        conditions=("FEAT_THE", "FEAT_D128"),
        fields=()),
    "lrcpc3": Feature(
        tokens=("lrcpc3",),
        conditions=("FEAT_LRCPC3",),
        fields=()),
    "lscp": Feature(
        tokens=("lscp",),
        conditions=("FEAT_LSCP",),
        fields=()),
    "ls64": Feature(
        tokens=("ls64", "ls64_v", "ls64_accdata"),
        conditions=("FEAT_LS64", "FEAT_LS64_V", "FEAT_LS64_ACCDATA"),
        fields=("accdata",)),
    "lsfe": Feature(
        tokens=("lsfe",),
        conditions=("FEAT_LSFE",),
        fields=()),
    "lsui": Feature(
        tokens=("lsui",),
        conditions=("FEAT_LSUI",),
        fields=()),
    "lor": Feature(
        tokens=("lor",),
        conditions=("FEAT_LOR",),
        fields=()),
    "fp8": Feature(
        tokens=("fp8", "fp8dot2", "fp8dot4", "fp8fma", "f8f16mm", "f8f32mm"),
        conditions=("FEAT_FP8", "FEAT_FP8DOT2", "FEAT_FP8DOT4",
                    "FEAT_FP8FMA", "FEAT_F8F16MM", "FEAT_F8F32MM"),
        fields=("fpmr",),
        encodings=((0xFFDF_FFE0, 0xD51B_4440),)),  # MRS and MSR FPMR
    "f16f32dot": Feature(
        tokens=("f16f32dot",),
        conditions=("FEAT_F16F32DOT",),
        fields=()),
    "f16mm": Feature(
        tokens=("f16mm", "f16f32mm"),
        conditions=("FEAT_F16MM", "FEAT_F16F32MM"),
        fields=()),
    "lut": Feature(
        tokens=("lut",),
        conditions=("FEAT_LUT",),
        fields=()),
    "faminmax": Feature(
        tokens=("faminmax",),
        conditions=("FEAT_FAMINMAX",),
        fields=()),
    "sm3": Feature(
        tokens=("sm3",),
        conditions=("FEAT_SM3",),
        fields=()),
    "sm4": Feature(
        tokens=("sm4",),
        conditions=("FEAT_SM4",),
        fields=()),
    "fprcvt": Feature(
        tokens=("fprcvt",),
        conditions=("FEAT_FPRCVT",),
        fields=()),
    "cmpbr": Feature(
        tokens=("cmpbr",),
        conditions=("FEAT_CMPBR",),
        fields=()),
    "hbc": Feature(
        tokens=("hbc",),
        conditions=("FEAT_HBC",),
        fields=()),
    "ssbs": Feature(
        tokens=("ssbs",),
        conditions=("FEAT_SSBS",),
        fields=("pstate_ssbs",),
        allocation=False,
        encodings=((0xFFFF_F0FF, 0xD503_403F),)),  # MSR SSBS, #imm
    "sme": Feature(
        tokens=("sme",),
        conditions=("FEAT_SME",),
        fields=("pstate_sm", "pstate_za"),
        encodings=((0xFFDF_FFE0, 0xD51B_4240),)),  # MRS and MSR SVCR
    "xs": Feature(
        tokens=("xs",),
        conditions=("FEAT_XS",),
        fields=()),
    "wfxt": Feature(
        tokens=("wfxt",),
        conditions=("FEAT_WFxT",),
        fields=()),
    # The producer never ran an SVE instruction, and the encoding table
    # supplies the words (table_records()). The virtual CPU executes none
    # outside streaming mode but the SM = 0 family.
    "sve": Feature(
        tokens=("sve",),
        conditions=(),
        fields=()),
}

# Illegal-word records: one case per encoding class and operand extreme.
# Each word was checked against llvm-mc with the feature enabled (22.1.8 for
# MTE, GCS, and CPA; 23.1.1 for PAuth and the memory, FP/SIMD, branch, and
# system extensions). Every inventory entry of a feature needs a word that
# build-unit-corpus.py attributes to it, and an alias is credited only through
# its own allocation: CBLE and CBLS through the W forms of CBLT and CBLO
# (immediate) and of CBGE and CBHS (register), STTCLR through STTCLR w.
UNDEFINED = {
    "mte": (
        ("ADDG", 0x9181_0420), ("ADDG SP", 0x91BF_3FFF),
        ("SUBG", 0xD181_0420), ("SUBG SP", 0xD1BF_3FFF),
        ("IRG", 0x9AC2_1020), ("IRG SP XZR", 0x9ADF_13FF),
        ("GMI", 0x9AC2_1420), ("GMI SP XZR", 0x9ADF_17FF),
        ("SUBP", 0x9AC2_0020), ("SUBP SP", 0x9ADF_03FF),
        ("SUBPS", 0xBAC2_0020), ("CMPP", 0xBAC2_003F),
        ("CMPP SP", 0xBADF_03FF),
        ("LDG", 0xD960_1020), ("LDG SP", 0xD97F_F3FF),
        ("STG post", 0xD920_1420), ("STG post SP", 0xD93F_F7FF),
        ("STG offset", 0xD920_1820), ("STG offset SP", 0xD93F_FBFF),
        ("STG pre", 0xD920_1C20), ("STG pre SP", 0xD93F_FFFF),
        ("STZG post", 0xD960_1420), ("STZG post SP", 0xD97F_F7FF),
        ("STZG offset", 0xD960_1820), ("STZG offset SP", 0xD97F_FBFF),
        ("STZG pre", 0xD960_1C20), ("STZG pre SP", 0xD97F_FFFF),
        ("ST2G post", 0xD9A0_1420), ("ST2G post SP", 0xD9BF_F7FF),
        ("ST2G offset", 0xD9A0_1820), ("ST2G offset SP", 0xD9BF_FBFF),
        ("ST2G pre", 0xD9A0_1C20), ("ST2G pre SP", 0xD9BF_FFFF),
        ("STZ2G post", 0xD9E0_1420), ("STZ2G post SP", 0xD9FF_F7FF),
        ("STZ2G offset", 0xD9E0_1820), ("STZ2G offset SP", 0xD9FF_FBFF),
        ("STZ2G pre", 0xD9E0_1C20), ("STZ2G pre SP", 0xD9FF_FFFF),
        ("LDGM", 0xD9E0_0020), ("LDGM SP", 0xD9E0_03FF),
        ("STGM", 0xD9A0_0020), ("STGM SP", 0xD9A0_03FF),
        ("STZGM", 0xD920_0020), ("STZGM SP", 0xD920_03FF),
        ("STGP post", 0x6880_8820), ("STGP post SP", 0x68BF_FFFF),
        ("STGP offset", 0x6900_8820), ("STGP offset SP", 0x693F_FFFF),
        ("STGP pre", 0x6980_8820), ("STGP pre SP", 0x69BF_FFFF),
        ("SETGP", 0x1DC2_0420), ("SETGM", 0x1DC2_4420),
        ("SETGE", 0x1DC2_8420), ("SETGPT", 0x1DC2_1420),
        ("SETGMT", 0x1DC2_5420), ("SETGET", 0x1DC2_9420),
        ("SETGPN", 0x1DC2_2420), ("SETGMN", 0x1DC2_6420),
        ("SETGEN", 0x1DC2_A420), ("SETGPTN", 0x1DC2_3420),
        ("SETGMTN", 0x1DC2_7420), ("SETGETN", 0x1DC2_B420),
        ("SETGP XZR", 0x1DDF_07BE),
        ("MSR TCO #1", 0xD503_419F), ("MSR TCO #0", 0xD503_409F),
        ("MRS TCO", 0xD53B_42E0), ("MSR TCO register", 0xD51B_42E0),
        ("DC GVA", 0xD50B_7460), ("DC GZVA", 0xD50B_7480),
        ("DC CGVAC", 0xD50B_7A60), ("DC CGDVAC", 0xD50B_7AA0),
        ("DC CGVAP", 0xD50B_7C60), ("DC CGDVAP", 0xD50B_7CA0),
        ("DC CGVADP", 0xD50B_7D60), ("DC CGDVADP", 0xD50B_7DA0),
        ("DC CIGVAC", 0xD50B_7E60), ("DC CIGDVAC", 0xD50B_7EA0),
    ),
    "gcs": (
        ("GCSPUSHM", 0xD50B_7700), ("GCSPUSHM XZR", 0xD50B_771F),
        ("GCSPOPM", 0xD52B_7720), ("GCSPOPM XZR", 0xD52B_773F),
        ("GCSSS1", 0xD50B_7740), ("GCSSS1 XZR", 0xD50B_775F),
        ("GCSSS2", 0xD52B_7760), ("GCSSS2 XZR", 0xD52B_777F),
        ("GCSPUSHX", 0xD508_779F), ("GCSPOPX", 0xD508_77DF),
        ("GCSPOPCX", 0xD508_77BF),
        ("GCSSTR", 0xD91F_0C20), ("GCSSTR SP", 0xD91F_0FFF),
        ("GCSSTTR", 0xD91F_1C20), ("GCSSTTR SP", 0xD91F_1FFF),
        ("MRS GCSPR_EL0", 0xD53B_2520), ("MSR GCSPR_EL0", 0xD51B_2520),
    ),
    "cpa": (
        ("ADDPT", 0x9A02_2420), ("ADDPT SP", 0x9A1F_3FFF),
        ("SUBPT", 0xDA02_2420), ("SUBPT SP", 0xDA1F_3FFF),
        ("MADDPT", 0x9B62_0C20), ("MADDPT XZR", 0x9B7F_7FFF),
        ("MSUBPT", 0x9B62_8C20), ("MSUBPT XZR", 0x9B7F_FFFF),
    ),
    "pauth": (
        ("PACIA", 0xDAC1_0020), ("PACIA XZR SP", 0xDAC1_03FF),
        ("PACIB", 0xDAC1_0420), ("PACIB SP", 0xDAC1_07FE),
        ("PACDA", 0xDAC1_0820), ("PACDA XZR SP", 0xDAC1_0BFF),
        ("PACDB", 0xDAC1_0C20), ("PACDB SP", 0xDAC1_0FFE),
        ("AUTIA", 0xDAC1_1020), ("AUTIA XZR SP", 0xDAC1_13FF),
        ("AUTIB", 0xDAC1_1420), ("AUTIB SP", 0xDAC1_17FE),
        ("AUTDA", 0xDAC1_1820), ("AUTDA XZR SP", 0xDAC1_1BFF),
        ("AUTDB", 0xDAC1_1C20), ("AUTDB SP", 0xDAC1_1FFE),
        ("PACIZA", 0xDAC1_23E0), ("PACIZA XZR", 0xDAC1_23FF),
        ("PACIZB", 0xDAC1_27E0), ("PACDZA", 0xDAC1_2BE0),
        ("PACDZB", 0xDAC1_2FE0), ("AUTIZA", 0xDAC1_33E0),
        ("AUTIZA XZR", 0xDAC1_33FF), ("AUTIZB", 0xDAC1_37E0),
        ("AUTDZA", 0xDAC1_3BE0), ("AUTDZB", 0xDAC1_3FE0),
        ("AUTDZB XZR", 0xDAC1_3FFF),
        ("XPACI", 0xDAC1_43E0), ("XPACI XZR", 0xDAC1_43FF),
        ("XPACD", 0xDAC1_47E0), ("XPACD XZR", 0xDAC1_47FF),
        ("PACGA", 0x9AC2_3020), ("PACGA XZR SP", 0x9ADF_33FF),
        ("BRAA", 0xD71F_0801), ("BRAA SP", 0xD71F_0BDF),
        ("BRAB", 0xD71F_0C01), ("BLRAA", 0xD73F_0801),
        ("BLRAA SP", 0xD73F_0BDF), ("BLRAB", 0xD73F_0C01),
        ("BRAAZ", 0xD61F_081F), ("BRAAZ X30", 0xD61F_0BDF),
        ("BRABZ", 0xD61F_0C1F), ("BLRAAZ", 0xD63F_081F),
        ("BLRABZ X30", 0xD63F_0FDF),
        ("RETAA", 0xD65F_0BFF), ("RETAB", 0xD65F_0FFF),
        ("ERETAA", 0xD69F_0BFF), ("ERETAB", 0xD69F_0FFF),
        ("LDRAA", 0xF820_0420), ("LDRAA minimum", 0xF860_0420),
        ("LDRAA XZR SP maximum", 0xF83F_F7FF),
        ("LDRAA pre", 0xF820_1C20), ("LDRAA pre SP minimum", 0xF860_0FE0),
        ("LDRAB", 0xF8A0_0420), ("LDRAB XZR SP maximum", 0xF8BF_F7FF),
        ("LDRAB pre", 0xF8FF_FC20),
    ),
    "lse128": (
        ("LDCLRP", 0x1921_1040), ("LDCLRPAL SP", 0x19FE_13FD),
        ("LDSETP", 0x1921_3040), ("LDSETPA SP", 0x19BE_33FD),
        ("SWPP", 0x1921_8040), ("SWPPL SP", 0x197E_83FD),
    ),
    "the": (
        ("RCWCAS", 0x1920_0841), ("RCWCASAL XZR SP", 0x19FF_0BFF),
        ("RCWSCAS", 0x5920_0841), ("RCWCASP", 0x1920_0C82),
        ("RCWSCASPAL SP", 0x59FC_0FFE), ("RCWCLR", 0x3820_9041),
        ("RCWSCLRA XZR SP", 0x78BF_93FF), ("RCWSET", 0x3820_B041),
        ("RCWSSETL", 0x7860_B041), ("RCWSWP", 0x3820_A041),
        ("RCWSSWPAL XZR SP", 0x78FF_A3FF), ("RCWCLRP", 0x1921_9040),
        ("RCWSCLRPAL SP", 0x59FE_93FD), ("RCWSETP", 0x1921_B040),
        ("RCWSSETPA SP", 0x59BE_B3FD), ("RCWSWPP", 0x1921_A040),
        ("RCWSSWPPL SP", 0x597E_A3FD),
    ),
    "lrcpc3": (
        ("LDIAPP w", 0x9941_1840), ("LDIAPP w post", 0x9941_0840),
        ("LDIAPP x", 0xD941_1840), ("LDIAPP x post SP", 0xD95E_0BFD),
        ("STILP w", 0x9901_1840), ("STILP w pre", 0x9901_0840),
        ("STILP x", 0xD901_1840), ("STILP x pre SP", 0xD91F_0BFF),
        ("LDAPR w post", 0x99C0_0820), ("LDAPR x post SP", 0xD9C0_0BFF),
        ("STLR w pre", 0x9980_0820), ("STLR x pre SP", 0xD980_0BFF),
        ("LDAP1 D0", 0x0D41_8420), ("LDAP1 D1 SP", 0x4D41_87FF),
        ("STL1 D0", 0x0D01_8420), ("STL1 D1 SP", 0x4D01_87FF),
        ("LDAPUR B", 0x1D40_0820), ("LDAPUR H", 0x5D50_0820),
        ("LDAPUR S", 0x9D4F_F820), ("LDAPUR D", 0xDD40_0820),
        ("LDAPUR Q SP", 0x1DDF_FBFF), ("STLUR B", 0x1D00_0820),
        ("STLUR H", 0x5D10_0820), ("STLUR S", 0x9D0F_F820),
        ("STLUR D", 0xDD00_0820), ("STLUR Q SP", 0x1D9F_FBFF),
    ),
    "lscp": (
        ("LDAP", 0xD941_5840), ("LDAP SP", 0xD95E_5BFD),
        ("LDAPP", 0xD941_7840), ("LDAPP SP", 0xD95E_7BFD),
        ("STLP", 0xD901_5840), ("STLP XZR SP", 0xD91F_5BFF),
    ),
    "ls64": (
        ("LD64B", 0xF83F_D020), ("LD64B SP", 0xF83F_D3F6),
        ("ST64B", 0xF83F_9020), ("ST64B SP", 0xF83F_93F6),
        ("ST64BV", 0xF822_B020), ("ST64BV XZR SP", 0xF83F_B3F6),
        ("ST64BV0", 0xF822_A020), ("ST64BV0 XZR SP", 0xF83F_A3F6),
    ),
    "lsfe": (
        ("LDFADD H", 0x7C20_0041), ("LDFADDA S", 0xBCA0_0041),
        ("LDFADDAL D SP", 0xFCFF_03FE), ("LDFADDL H", 0x7C60_0041),
        ("LDFMAX S", 0xBC20_4041), ("LDFMAXNM D", 0xFC20_6041),
        ("LDFMIN H", 0x7C20_5041), ("LDFMINNM S", 0xBC20_7041),
        ("LDBFADD", 0x3C20_0041), ("LDBFMAX", 0x3C20_4041),
        ("LDBFMAXNMAL SP", 0x3CFF_63FE), ("LDBFMIN", 0x3C20_5041),
        ("LDBFMINNM", 0x3C20_7041), ("STFADD H", 0x7C20_805F),
        ("STFADDL D SP", 0xFC7F_83FF), ("STFMAX S", 0xBC20_C05F),
        ("STFMAXNM D", 0xFC20_E05F), ("STFMIN H", 0x7C20_D05F),
        ("STFMINNM S", 0xBC20_F05F), ("STBFADD", 0x3C20_805F),
        ("STBFMAX", 0x3C20_C05F), ("STBFMAXNM", 0x3C20_E05F),
        ("STBFMIN", 0x3C20_D05F), ("STBFMINNML SP", 0x3C7F_F3FF),
    ),
    "lsui": (
        ("LDTADD w", 0x1920_0441), ("LDTADDAL x SP", 0x59FD_07FE),
        ("LDTCLR w", 0x1920_1441), ("LDTCLRA x", 0x59A0_1441),
        ("LDTSET w", 0x1920_3441), ("LDTSETL x", 0x5960_3441),
        ("STTADD w", 0x1920_045F), ("STTCLRL x SP", 0x597E_17FF),
        ("STTCLR w", 0x1920_145F),
        ("STTSET w", 0x1920_345F), ("SWPT w", 0x1920_8441),
        ("SWPTAL x SP", 0x59FD_87FE), ("CAST", 0xC980_7C41),
        ("CASALT SP", 0xC9DD_FFFE), ("CASPT", 0x4980_7C82),
        ("CASPALT SP", 0x49DC_FFFE), ("LDTXR w", 0x895F_7C20),
        ("LDTXR x SP", 0xC95F_7FFE), ("LDATXR x", 0xC95F_FC20),
        ("STTXR w", 0x8902_7C20), ("STTXR x SP", 0xC902_7FFE),
        ("STLTXR x", 0xC902_FC20), ("LDTP", 0xE940_0440),
        ("LDTP post SP", 0xE8E0_7BFD), ("LDTP pre", 0xE9DF_8440),
        ("STTP", 0xE900_0440), ("STTP pre SP", 0xE9BF_FFFF),
        ("LDTNP", 0xE840_0440), ("STTNP SP", 0xE81F_FBFD),
        ("LDTP Q", 0xED40_0440), ("LDTP Q post SP", 0xECE0_7FFE),
        ("STTP Q pre", 0xED9F_8440), ("LDTNP Q", 0xEC40_0440),
        ("STTNP Q SP", 0xEC3F_FFFE),
    ),
    "lor": (
        ("LDLARB", 0x08DF_7C20), ("LDLARH SP", 0x48DF_7FFE),
        ("LDLAR w", 0x88DF_7C20), ("LDLAR x SP", 0xC8DF_7FFF),
        ("STLLRB", 0x089F_7C20), ("STLLRH SP", 0x489F_7FFF),
        ("STLLR w", 0x889F_7C20), ("STLLR x SP", 0xC89F_7FFE),
    ),
    "fp8": (
        ("F1CVTL", 0x2E21_7820), ("F1CVTL2 V31", 0x6E21_7BFF),
        ("F2CVTL", 0x2E61_7820), ("BF1CVTL", 0x2EA1_7820),
        ("BF2CVTL2 V31", 0x6EE1_7BFF), ("FCVTN half", 0x0E42_F420),
        ("FCVTN half Q", 0x4E42_F420), ("FCVTN single", 0x0E02_F420),
        ("FCVTN2 single V31", 0x4E1F_F7FF), ("FSCALE 4H", 0x2EC2_3C20),
        ("FSCALE 8H V31", 0x6EDF_3FFF), ("FSCALE 2S", 0x2EA2_FC20),
        ("FSCALE 4S", 0x6EA2_FC20), ("FSCALE 2D V31", 0x6EFF_FFFF),
        ("FDOT half", 0x0E42_FC20), ("FDOT half Q", 0x4E42_FC20),
        ("FDOT half indexed", 0x0F42_0020),
        ("FDOT half indexed V31", 0x4F7F_0BFF), ("FDOT single", 0x0E02_FC20),
        ("FDOT single indexed", 0x0F02_0020),
        ("FDOT single indexed V31", 0x4F3F_0BFF), ("FMLALB", 0x0EC2_FC20),
        ("FMLALT V31", 0x4EDF_FFFF), ("FMLALB indexed", 0x0FC2_0020),
        ("FMLALT indexed V31", 0x4FFF_0BFF), ("FMLALLBB", 0x0E02_C420),
        ("FMLALLBT", 0x0E42_C420), ("FMLALLTB", 0x4E02_C420),
        ("FMLALLTT V31", 0x4E5F_C7FF), ("FMLALLBB indexed", 0x2F02_8020),
        ("FMLALLTT indexed V31", 0x6F7F_8BFF), ("FMMLA half", 0x6E02_EC20),
        ("FMMLA single V31", 0x6E9F_EFFF), ("MRS FPMR", 0xD53B_4440),
        ("MSR FPMR XZR", 0xD51B_445F),
    ),
    "f16f32dot": (
        ("FDOT 2S", 0x0E82_FC20), ("FDOT 4S V31", 0x4E9F_FFFF),
        ("FDOT indexed", 0x0F42_9020), ("FDOT indexed V31", 0x4F7F_9BFF),
    ),
    "f16mm": (
        ("FMMLA half", 0x4EC2_EC20), ("FMMLA half V31", 0x4EDF_EFFF),
        ("FMMLA widening", 0x4E42_EC20), ("FMMLA widening V31", 0x4E5F_EFFF),
    ),
    "lut": (
        ("LUTI2 B", 0x4E82_1020), ("LUTI2 B V31", 0x4E9F_73FF),
        ("LUTI2 H", 0x4EC2_0020), ("LUTI2 H V31", 0x4EDF_73FF),
        ("LUTI4 B", 0x4E42_2020), ("LUTI4 B V31", 0x4E5F_63FF),
        ("LUTI4 H", 0x4E43_1020), ("LUTI4 H V31 wrap", 0x4E5F_73FF),
    ),
    "faminmax": (
        ("FAMAX 4H", 0x0EC2_1C20), ("FAMAX 8H V31", 0x4EDF_1FFF),
        ("FAMAX 2S", 0x0EA2_DC20), ("FAMAX 4S", 0x4EA2_DC20),
        ("FAMAX 2D V31", 0x4EFF_DFFF), ("FAMIN 4H", 0x2EC2_1C20),
        ("FAMIN 8H V31", 0x6EDF_1FFF), ("FAMIN 2S", 0x2EA2_DC20),
        ("FAMIN 4S", 0x6EA2_DC20), ("FAMIN 2D V31", 0x6EFF_DFFF),
    ),
    "sm3": (
        ("SM3PARTW1", 0xCE62_C020), ("SM3PARTW2 V31", 0xCE7F_C7FF),
        ("SM3SS1", 0xCE42_0C20), ("SM3SS1 V31", 0xCE5F_7FFF),
        ("SM3TT1A", 0xCE42_8020), ("SM3TT1B", 0xCE42_9420),
        ("SM3TT2A", 0xCE42_A820), ("SM3TT2B V31", 0xCE5F_BFFF),
    ),
    "sm4": (
        ("SM4E", 0xCEC0_8420), ("SM4E V31", 0xCEC0_87FF),
        ("SM4EKEY", 0xCE62_C820), ("SM4EKEY V31", 0xCE7F_CBFF),
    ),
    "fprcvt": (
        ("FCVTAS S D", 0x1E7A_0020), ("FCVTAS D S", 0x9E3A_0020),
        ("FCVTAS S H", 0x1EFA_0020), ("FCVTAS D H V31", 0x9EFA_03FF),
        ("FCVTAU S D", 0x1E7B_0020), ("FCVTMS D S", 0x9E34_0020),
        ("FCVTMU S H", 0x1EF5_0020), ("FCVTNS D H", 0x9EEA_0020),
        ("FCVTNU S D", 0x1E6B_0020), ("FCVTPS D S", 0x9E32_0020),
        ("FCVTPU S H", 0x1EF3_0020), ("FCVTZS D H", 0x9EF6_0020),
        ("FCVTZU S D V31", 0x1E77_03FF), ("SCVTF S D", 0x9E3C_0020),
        ("SCVTF D S", 0x1E7C_0020), ("SCVTF H S", 0x1EFC_0020),
        ("SCVTF H D V31", 0x9EFC_03FF), ("UCVTF S D", 0x9E3D_0020),
        ("UCVTF D S V31", 0x1E7D_03FF),
    ),
    "cmpbr": (
        ("CBGT W", 0x7401_0040), ("CBGE X", 0xF423_3FE2),
        ("CBGE W", 0x742B_3FEA),
        ("CBHI W", 0x7445_1FE4), ("CBHS X", 0xF467_2006),
        ("CBHS W WZR", 0x746D_003F),
        ("CBEQ W", 0x74C9_0028), ("CBNE X XZR", 0xF4FE_003F),
        ("CBBGT", 0x7401_8040), ("CBBGE", 0x7423_8042), ("CBBHI", 0x7445_8044),
        ("CBBHS", 0x7467_8046), ("CBBEQ", 0x74C9_8048),
        ("CBBNE WZR", 0x74FF_805E), ("CBHGT", 0x7401_C040),
        ("CBHGE", 0x7423_C042), ("CBHHI", 0x7445_C044), ("CBHHS", 0x7467_C046),
        ("CBHEQ", 0x74C9_C048), ("CBHNE", 0x74FD_C05E),
        ("CBGT W imm", 0x7500_0040), ("CBLT X imm", 0xF53F_8041),
        ("CBLT W imm", 0x7520_BFE5),
        ("CBHI W imm", 0x7542_8042), ("CBLO X imm", 0xF560_8043),
        ("CBLO W imm", 0x757F_A007),
        ("CBEQ W imm", 0x75C0_2004), ("CBNE X imm", 0xF5FF_9FFE),
    ),
    "hbc": (
        ("BC.EQ", 0x5400_0030), ("BC.NE back", 0x54FF_FFF1),
        ("BC.HS far", 0x547F_FFF2), ("BC.LT far back", 0x5480_001B),
        ("BC.GT", 0x5400_005C), ("BC.AL", 0x5400_005E), ("BC.NV", 0x5400_005F),
    ),
    "ssbs": (
        ("MSR SSBS 0", 0xD503_403F), ("MSR SSBS 1", 0xD503_413F),
    ),
    "sme": (
        ("SMSTART", 0xD503_477F), ("SMSTART SM", 0xD503_437F),
        ("SMSTART ZA", 0xD503_457F), ("SMSTOP", 0xD503_467F),
        ("SMSTOP SM", 0xD503_427F), ("SMSTOP ZA", 0xD503_447F),
        ("MRS SVCR", 0xD53B_4240), ("MSR SVCR", 0xD51B_425E),
    ),
    "xs": (
        ("DSB OSHNXS", 0xD503_323F), ("DSB NSHNXS", 0xD503_363F),
        ("DSB ISHNXS", 0xD503_3A3F), ("DSB SYNXS", 0xD503_3E3F),
    ),
    "wfxt": (
        ("WFET", 0xD503_1000), ("WFET X30", 0xD503_101E),
        ("WFIT", 0xD503_1023), ("WFIT XZR", 0xD503_103F),
    ),
}
# The records whose words come from the encoding table: one witness word per
# table row, checked against LLVM MC by its generator, each at PSTATE.SM = 0
# in the record whose option makes it UNDEFINED there (table_records()).
TABLE_RECORDS = {"sme": ("sme",), "sve": ("sve", "sm0_family")}
# The options each such record runs under; the unconditional record has none.
TABLE_OPTIONS = {"sme": ("without_sme",), "sve": (),
                 "sm0_family": ("without_sme",)}
HINTS = {
    "gcs": (("GCSB DSYNC", 0xD503_227F), ("CHKFEAT X16", 0xD503_251F)),
    "pauth": (
        ("XPACLRI", 0xD503_20FF), ("PACIA1716", 0xD503_211F),
        ("PACIB1716", 0xD503_215F), ("AUTIA1716", 0xD503_219F),
        ("AUTIB1716", 0xD503_21DF), ("PACIAZ", 0xD503_231F),
        ("PACIASP", 0xD503_233F), ("PACIBZ", 0xD503_235F),
        ("PACIBSP", 0xD503_237F), ("AUTIAZ", 0xD503_239F),
        ("AUTIASP", 0xD503_23BF), ("AUTIBZ", 0xD503_23DF),
        ("AUTIBSP", 0xD503_23FF),
    ),
}


def read_failing(path: pathlib.Path | None) -> dict[tuple, str]:
    """(record, case name, 16 KiB ordinal, 4 KiB ordinal) -> feature."""
    if path is None:
        return {}
    failing = {}
    with path.open(encoding="utf-8") as source:
        rows = (line for line in source if not line.startswith("#"))
        for row in csv.DictReader(rows, delimiter="\t"):
            key = (row["record"], row["case"], int(row["ordinal_16k"]),
                   int(row["ordinal_4k"]))
            failing[key] = row["feature"]
    return failing


def is_hint(value: int) -> bool:
    return (value & 0xFFFF_F01F) == 0xD503_201F


class Rewriter:
    def __init__(self, features: tuple[str, ...]) -> None:
        self.features = {name: FEATURES[name] for name in features}
        self.classifier = corpus.Classifier()
        # Every non-hint encoding the features allocate: the allocation
        # predicates, each encoding of the decode partitions, and the system
        # encodings filed under baseline instructions.
        self.encodings: list[tuple[corpus.Pattern, str]] = []
        with corpus.ALLOCATIONS.open(encoding="utf-8") as source:
            for row in csv.DictReader(source, delimiter="\t"):
                tokens = set(row["architecture_native_feature_alternatives"]
                             .replace("|", "+").split("+"))
                self.add(row, lambda feature: feature.allocation and
                         bool(tokens & feature.tokens))
        with corpus.PARTITIONS.open(encoding="utf-8") as source:
            for row in csv.DictReader(source, delimiter="\t"):
                if row["partition_kind"] != "allocated":
                    continue
                conditions = set(row["feature_condition"].split(" && "))
                if "FEAT_SVE" in conditions:
                    continue  # SVE is absent, and with it these encodings.
                self.add(row, lambda feature: bool(conditions &
                                                   feature.conditions))
        for name, feature in self.features.items():
            for mask, value in feature.encodings:
                self.encodings.append((corpus.Pattern(
                    f"{mask:08x}", f"{value:08x}", "none"), name))
        self.stripped_options = frozenset().union(*(
            feature.stripped_options for feature in self.features.values()))

    def add(self, row: dict, owned) -> None:
        if is_hint(int(row["architecture_value"], 16)):
            return
        for name, feature in self.features.items():
            if owned(feature):
                self.encodings.append((corpus.Pattern(
                    row["architecture_mask"], row["architecture_value"],
                    row["architecture_constraints"]), name))
                return

    def encoding_feature(self, word: int) -> str | None:
        """The removed feature a word's encoding belongs to, if any."""
        for pattern, name in self.encodings:
            if pattern.matches(word):
                return name
        return None

    def exercised_feature(self, record: dict, case: dict) -> str | None:
        """The removed feature whose behaviour the case checks, if any. A
        case that expects the undefined-instruction trap does not check it
        through its native requirement: without the feature, every encoding
        stays undefined."""
        undefined = case["event"] == "illegal-instruction"
        for name, feature in self.features.items():
            native = not undefined and set(case["native"]) & feature.tokens
            if native and feature.fpcr_bits and not any(
                    fpcr_value(state) & feature.fpcr_bits
                    for state in fpcr_states(case)):
                native = False
            if native or \
                    set(record["options"]) & feature.options or \
                    case["event"] in feature.events or \
                    case.get("validator") in feature.validators:
                return name
        return None

    def strip(self, case: dict) -> list[str]:
        """Removes the features' state; returns the features that had any."""
        stripped = []
        for name, feature in self.features.items():
            present = False
            for state in (case["initial"], case["expected"]):
                for field in feature.fields:
                    present |= state.pop(field, None) is not None
            if feature.tags:
                for region in case.get("memory", []):
                    present |= bool(region["tags"] or region["expected_tags"])
                    region["tags"] = []
                    region["expected_tags"] = []
            if feature.fpcr_bits:
                present |= strip_fpcr(case, feature.fpcr_bits)
                case["native"] = [token for token in case["native"]
                                  if token not in feature.tokens]
            if present:
                stripped.append(name)
        return stripped


def fpcr_value(state: dict | None) -> int:
    return 0 if state is None else int(state.get("fpcr", "0x0"), 16)


def fpcr_states(case: dict) -> tuple[dict, dict | None]:
    """The initial FPCR and, if it differs, the expected one. The dump stores
    initial fields that differ from the base state (FPCR 0) and expected
    fields that differ from the initial state."""
    return case["initial"], (case["expected"] if "fpcr" in case["expected"]
                             else None)


def strip_fpcr(case: dict, bits: int) -> bool:
    """Clears bits from the case's FPCR and keeps the sparse encoding
    canonical; returns whether any was set."""
    initial = fpcr_value(case["initial"])
    expected = fpcr_value(case["expected"]) if "fpcr" in case["expected"] \
        else initial
    if not (initial | expected) & bits:
        return False
    initial &= ~bits
    expected &= ~bits
    case["initial"].pop("fpcr", None)
    case["expected"].pop("fpcr", None)
    if initial:
        case["initial"]["fpcr"] = corpus.hex64(initial)
    if expected != initial:
        case["expected"]["fpcr"] = corpus.hex64(expected)
    return True


def afp_rule(case: dict, entry: int) -> str:
    """Applies the FEAT_AFP rule to a kept case and names its outcome:
    "controls" when its FPCR sets one of the controls, or "added" when it
    also gains the native requirement; otherwise the case loses any native
    requirement and is a "pin" when its entry MSR FPCR writes a control that
    its FPCR state never holds, a "gate" when it lost the requirement, and
    "none" else."""
    controls = any(fpcr_value(state) & AFP_CONTROLS
                   for state in fpcr_states(case))
    if controls:
        if "afp" in case["native"]:
            return "controls"
        order = {token: index
                 for index, token in enumerate(corpus.feature_tokens())}
        case["native"] = sorted([*case["native"], "afp"],
                                key=lambda token: order.get(token, len(order)))
        return "added"
    gate = "afp" in case["native"]
    case["native"] = [token for token in case["native"] if token != "afp"]
    mask, value = MSR_FPCR
    register = entry & 31
    if entry & mask == value and register != 31 and \
            int(case["initial"].get(f"x{register}", "0x0"), 16) & AFP_CONTROLS:
        return "pin"
    return "gate" if gate else "none"


def without_afp_record(item: dict, cases: list[dict]) -> dict:
    """A sibling of item holding the cases that pin FEAT_AFP's controls RES0,
    under the without_afp option and the name build-unit-corpus.py gives a
    record with those options."""
    options = sorted({*item["options"], WITHOUT_AFP},
                     key=corpus.OPTION_NAMES.index)
    bits = sum(1 << corpus.OPTION_NAMES.index(name) for name in options)
    name = item["record"]
    profile = ""
    for suffix in ("/16k", "/4k"):
        if name.endswith(suffix):
            name, profile = name[:-len(suffix)], suffix
    name = re.sub(r"/options=[0-9a-f]+$", "", name)
    used = sorted({case["program"] for case in cases})
    renumber = {old: new for new, old in enumerate(used)}
    sibling = {**item, "record": f"{name}/options={bits:x}{profile}",
               "options": options,
               "programs": [item["programs"][index] for index in used],
               "cases": [{**case, "program": renumber[case["program"]]}
                         for case in cases]}
    return sibling


def table_records(row) -> tuple[str, ...]:
    """The records that pin a table row's witness, or the base row's of an
    alias, UNDEFINED at PSTATE.SM = 0 (None: a form only Debug state
    executes, which needs FEAT_SME2):

    - "sm0_family", under without_sme: the SM = 0 family (the
      vector-length words and the Z and P spill words), which a virtual CPU
      with SME executes there (sm0-vl-corpus.py), so only one without it
      takes it as UNDEFINED;
    - "sme", under without_sme: the other rows of the SME profile that
      execute at SM = 0, so a virtual CPU without SME takes them as
      UNDEFINED there;
    - "sve", unconditional: the SVE rows that execute only in streaming mode
      or nowhere (the SCLAMP class, which needs both FEAT_SVE and FEAT_SME
      outside streaming mode, included), and every row outside the SME
      profile."""
    if row is None:
        return ("sme",)
    outside = set(row.non_streaming_requirement)
    if sme_table.sm0_family_word(row.witness):
        return ("sm0_family",)
    if "sve" in outside:
        return ("sve",)
    if row.in_profile and (outside or row.form & sme_table.SME_SECTION):
        return ("sme",)
    return ("sve",)


@functools.cache
def table_words(record: str) -> tuple[tuple[str, int], ...]:
    table = sme_table.load()
    words = [(f"{row.stable_id} {row.encoding} {row.witness:08x}",
              row.witness)
             for row in table.rows if record in table_records(row)]
    # The evidence-binding witness of each SVE or SME entry without a row:
    # an alias, which its base instruction's row decodes, or a form only
    # Debug state executes (MOVT between ZT0 and a general register), which
    # needs FEAT_SME2.
    rows = {row.stable_id for row in table.rows}
    with corpus.BINDINGS.open(encoding="utf-8", newline="") as source:
        for binding in csv.DictReader(source, delimiter="\t"):
            if binding["section"] not in ("SVE", "SME") or \
                    binding["stable_id"] in rows:
                continue
            word = int(binding["encoding"], 16)
            if record in table_records(table.row_for(word)):
                words.append((f"{binding['stable_id']} "
                              f"{binding['architectural_form']} {word:08x}",
                              word))
    return tuple(words)


def undefined_records(feature: str) -> list[dict]:
    """The feature's arm64-absent-<feature> records."""
    records = []
    for table in TABLE_RECORDS.get(feature, (None,)):
        record = undefined_record(feature, table)
        if record is not None:
            records.append(record)
    return records


def undefined_record(feature: str, table: str | None) -> dict | None:
    programs: list[list] = []
    cases = []
    words = () if table not in (None, feature) else UNDEFINED.get(feature, ())
    if table is not None:
        words = (*words, *table_words(table))
    options = list(TABLE_OPTIONS[table]) if table is not None else []
    for name, word in words:
        programs.append({"entry": 0, "words": [[0, f"{word:08x}"],
                                               [4, f"{SVC:08x}"]]})
        # Whether the word is an invalid encoding is the shared classifier's
        # answer, not the case's being undefined: a generic system word
        # that names an omitted feature's register decodes, and a word of a
        # feature that another configuration implements does too.
        decision = invalid_encoding.classifier().decide(word, options)
        if decision is None:
            raise SystemExit(f"{name} {word:08x}: invalid_encoding.py does "
                             "not decide this absent-feature word")
        cases.append({
            "name": f"{name} is undefined", "program": len(programs) - 1,
            "event": "illegal-instruction", "ordinals": [0, 0],
            "invalid_encoding": decision.invalid, "minimization": "full",
            "native": [],
            "initial": {}, "expected": {}, "retired": 0,
        })
    for name, word in HINTS.get(feature, ()) if table is None else ():
        programs.append({"entry": 0, "words": [[0, f"{word:08x}"],
                                               [4, f"{SVC:08x}"]]})
        cases.append({
            "name": f"{name} is a NOP", "program": len(programs) - 1,
            "event": "system-call", "ordinals": [0, 0],
            "invalid_encoding": False, "minimization": "full", "native": [],
            "initial": {},
            "expected": {"pc": corpus.hex64(corpus.CODE_BASE + 8)},
            "retired": 2,
        })
    if not cases:
        return None
    name = f"arm64-absent-{feature}"
    if options:
        bits = sum(1 << corpus.OPTION_NAMES.index(option)
                   for option in options)
        name += f"/options={bits:x}"
    return {
        "record": name, "producer": f"arm64-absent-{feature}",
        "site": SITE, "profiles": ["4k"], "options": options,
        "programs": programs, "cases": cases,
    }


REPLACEMENT_PRODUCER = "arm64-absent-mte-tbi"
FAILURE = re.compile(r'^FAIL (\S+) case (\d+) ')


def base_register_fields(word: int) -> tuple[int, int]:
    """(Rn, register fields other than Rn) of a load/store word."""
    return (word >> 5) & 31, (word & 31, (word >> 10) & 31, (word >> 16) & 31)


def tbi_variant(case: dict, word: int, tag: int) -> dict | None:
    """The case with TBI enabled and a tagged base, or None if the rule
    does not apply to it."""
    if case["event"] != "system-call" or "validator" in case or \
            "fault" in case or "tbi_enabled" in case["initial"]:
        return None
    base, others = base_register_fields(word)
    key = f"x{base}"
    if base == 31 or base in others or key not in case["initial"]:
        return None
    address = int(case["initial"][key], 16)
    written = case["expected"].get(key)
    if address >> 56 or (written is not None and int(written, 16) >> 56):
        return None
    variant = json.loads(json.dumps(case))
    variant["initial"][key] = corpus.hex64(address | tag << 56)
    variant["initial"]["tbi_enabled"] = corpus.hex64(1)
    if written is not None:
        variant["expected"][key] = corpus.hex64(int(written, 16) | tag << 56)
    variant["name"] = f"{case['name']} with TBI tag 0x{tag:02x}"
    variant["ordinals"] = [0, 0]
    return variant


def replacements(kept: list[tuple[dict, dict, int]], lost: collections.Counter,
                 classifier) -> list[dict]:
    """Replaces each allocation's lost cases from its kept cases."""
    candidates: dict[str, list[int]] = collections.defaultdict(list)
    for index, (_record, _case, word) in enumerate(kept):
        for stable_id in classifier.classify(word):
            # Only SIMD&FP transfers: the base is their one general register.
            if stable_id in lost and stable_id.startswith("FPSIMD-"):
                candidates[stable_id].append(index)
    chosen: dict[int, None] = {}
    for stable_id in sorted(lost):
        pool = [index for index in candidates[stable_id]
                if tbi_variant(kept[index][1], kept[index][2], 1) is not None]
        count = min(lost[stable_id], len(pool))
        for step in range(count):
            chosen.setdefault(pool[step * len(pool) // count])
    records: dict[tuple, dict] = {}
    for serial, index in enumerate(sorted(chosen)):
        record, case, word = kept[index]
        tag = 1 + serial * 37 % 255
        variant = tbi_variant(case, word, tag)
        profiles = tuple(record["profiles"])
        options = tuple(record["options"])
        item = records.get((profiles, options))
        if item is None:
            name = f"{REPLACEMENT_PRODUCER}/{'+'.join(profiles)}"
            if options:
                name += "/" + "+".join(options)
            item = records[(profiles, options)] = {
                "record": name, "producer": REPLACEMENT_PRODUCER,
                "site": SITE, "profiles": list(profiles),
                "options": list(options), "programs": [], "cases": [],
            }
        program = record["programs"][case["program"]]
        if program not in item["programs"]:
            item["programs"].append(program)
        variant["program"] = item["programs"].index(program)
        item["cases"].append(variant)
    return [records[key] for key in sorted(records)]


def stripped_features(source: pathlib.Path,
                      features: tuple[str, ...]) -> dict[tuple, str]:
    """(record, case name, ordinals) -> the last feature whose state the
    rewrite strips from the source dump's case."""
    rewriter = Rewriter(features)
    owners = {}
    with source.open(encoding="utf-8") as lines:
        next(lines)
        for line in lines:
            item = json.loads(line)
            if "cases" not in item:
                continue
            for case in item["cases"]:
                stripped = rewriter.strip(json.loads(json.dumps(case)))
                if stripped:
                    owners[(item["record"], case["name"],
                            *case["ordinals"])] = stripped[-1]
    return owners


def collect_failing(output: pathlib.Path, dump: pathlib.Path,
                    logs: list[pathlib.Path], source: pathlib.Path,
                    previous: pathlib.Path | None,
                    features: tuple[str, ...]) -> int:
    """Writes the --failing list from corpus-test logs of a rewritten dump."""
    owners = stripped_features(source, features)
    owners.update((key, feature) for key, feature in
                  read_failing(previous).items() if feature in features)
    failing = set()
    for log in logs:
        for line in log.read_text(encoding="utf-8").splitlines():
            match = FAILURE.match(line)
            if match:
                failing.add((match.group(1), int(match.group(2))))
    rows = set()
    with dump.open(encoding="utf-8") as source:
        for line in source:
            item = json.loads(line)
            if "cases" not in item:
                continue
            for index, case in enumerate(item["cases"]):
                if (item["record"], index) in failing:
                    if item["producer"].startswith("arm64-absent-"):
                        corpus.fail(f"{item['record']}: authored case "
                                    f"{case['name']} fails")
                    key = (item["record"], case["name"], *case["ordinals"])
                    if key not in owners:
                        corpus.fail(f"{item['record']}: {case['name']} fails "
                                    "but the rewrite stripped no state")
                    rows.add((*key, owners[key]))
                    failing.discard((item["record"], index))
    if failing:
        corpus.fail(f"logs name cases the dump lacks: {sorted(failing)[0]}")
    with output.open("w", encoding="utf-8") as sink:
        sink.write("# Cases that fail once the absent features' state is "
                   "stripped; see absent-feature-corpus.py.\n")
        sink.write("record\tcase\tordinal_16k\tordinal_4k\tfeature\n")
        for record, name, first, second, feature in sorted(rows):
            sink.write(f"{record}\t{name}\t{first}\t{second}\t{feature}\n")
    print(f"wrote {len(rows)} failing cases to {output}")
    return 0


def authored_only(dump: pathlib.Path, output: pathlib.Path) -> int:
    """Rewrites a finished dump's arm64-absent-<feature> records in place,
    from the encoding table and this script's constants, and refreshes the
    step's hashes. Every other record, including the TBI replacements, is
    the dump's own, and so are the records of the steps that followed."""
    lines = dump.read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    step = header["meta"].get("absent_features")
    if step is None:
        corpus.fail(f"{dump}: not a dump the absent-feature step rewrote")
    fresh = [record for feature in step["features"]
             for record in undefined_records(feature)]
    kept = []
    position = None
    replaced = 0
    for line in lines[1:]:
        if not line.strip():
            continue
        if line.startswith('{"cases"'):
            item = json.loads(line)
            if item["producer"].startswith("arm64-absent-") and \
                    item["producer"] != REPLACEMENT_PRODUCER:
                if position is None:
                    position = len(kept)
                replaced += len(item["cases"])
                continue
        kept.append(line)
    if position is None:
        position = len(kept)
    kept[position:position] = [json.dumps(record, sort_keys=True)
                               for record in fresh]
    added = sum(len(record["cases"]) for record in fresh)
    step["sha256"] = hashlib.sha256(
        pathlib.Path(__file__).read_bytes()).hexdigest()
    step["sme_table"] = {"file": sme_table.TABLE_PATH,
                         "sha256": sme_table.load().sha256}
    step["added_cases"] = added
    with output.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in kept:
            sink.write(line + "\n")
    print(f"replaced {replaced} absent-feature cases with {added}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path, nargs="+",
                        help="rewritten dump, or corpus-test logs with "
                        "--collect-failing")
    parser.add_argument("--authored-only", action="store_true",
                        help="rewrite only the arm64-absent-<feature> "
                        "records of a finished dump")
    parser.add_argument("--collect-failing", type=pathlib.Path, metavar="TSV",
                        help="write the failing list from corpus-test logs")
    parser.add_argument("--source", type=pathlib.Path, metavar="DUMP",
                        help="with --collect-failing, the dump the logs' "
                        "rewrite was made from")
    parser.add_argument("--previous", type=pathlib.Path, metavar="TSV",
                        help="with --collect-failing, the failing list whose "
                        "attributions stand")
    parser.add_argument("--features", default=",".join(DEFAULT_FEATURES),
                        help="comma-separated features (default: %(default)s)")
    parser.add_argument("--failing", type=pathlib.Path,
                        help="cases that fail once the features are stripped")
    parser.add_argument("--report", type=pathlib.Path,
                        help="write per-allocation drop counts as TSV")
    options = parser.parse_args()
    features = tuple(options.features.split(","))
    if not features or set(features) - set(FEATURES) or \
            len(set(features)) != len(features):
        parser.error(f"--features takes names from {', '.join(FEATURES)}")
    if options.collect_failing is not None:
        if options.source is None:
            parser.error("--collect-failing requires --source")
        return collect_failing(options.collect_failing, options.dump,
                               options.output, options.source,
                               options.previous, features)
    if len(options.output) != 1:
        parser.error("the rewrite takes one output dump")
    options.output = options.output[0]
    if options.authored_only:
        return authored_only(options.dump, options.output)
    failing = read_failing(options.failing)
    rewriter = Rewriter(features)

    lines = options.dump.read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    blobs: list[str] = []
    records: list[dict] = []
    dropped = collections.Counter()
    by_entry: dict[str, collections.Counter] = collections.defaultdict(
        collections.Counter)
    kept_by_entry = collections.Counter()
    matched_failing = set()
    stripped = 0
    ungated = collections.Counter()
    kept_cases: list[tuple[dict, dict, int]] = []
    lost = collections.Counter()
    afp = collections.Counter()
    for line in lines[1:]:
        if not line.strip():
            continue
        item = json.loads(line)
        if "blob" in item:
            blobs.append(line)
            continue
        if item["producer"].startswith("arm64-absent-"):
            continue
        item["options"] = [name for name in item["options"]
                           if name not in rewriter.stripped_options]
        kept = []
        pinned = []
        for case in item["cases"]:
            program = item["programs"][case["program"]]
            words = [int(word, 16) for _offset, word in program["words"]]
            entry = int(dict(program["words"])[program["entry"]], 16)
            key = (item["record"], case["name"], *case["ordinals"])
            reason = None
            feature = rewriter.exercised_feature(item, case)
            if feature is not None:
                reason = "feature-case"
            if reason is None and case["event"] != "illegal-instruction":
                feature = next((owner for owner in map(
                    rewriter.encoding_feature, words) if owner), None)
                if feature is not None:
                    reason = "removed-encoding"
            if reason is None:
                stripped += bool(rewriter.strip(case))
                if key in failing:
                    feature = failing[key]
                    reason = "fails-stripped"
                    matched_failing.add(key)
            if reason is not None:
                dropped[(feature, reason)] += 1
                for stable_id in rewriter.classifier.classify(entry) or ("none",):
                    by_entry[stable_id][reason] += 1
                    if reason == "fails-stripped" and \
                            rewriter.features[feature].tags:
                        lost[stable_id] += 1
                continue
            for stable_id in rewriter.classifier.classify(entry) or ("none",):
                kept_by_entry[stable_id] += 1
            for token in set(case["native"]) & set(HOST_GATES):
                ungated[token] += 1
            case["native"] = [token for token in case["native"]
                              if token not in HOST_GATES]
            rule = afp_rule(case, entry)
            afp[rule] += 1
            if rule == "pin":
                pinned.append(case)
                continue
            kept.append(case)
            if len(words) == 2 and words[1] == SVC and program["entry"] == 0:
                kept_cases.append((item, case, entry))
        sibling = without_afp_record(item, pinned) if pinned else None
        if kept:
            used = sorted({case["program"] for case in kept})
            renumber = {old: new for new, old in enumerate(used)}
            item["programs"] = [item["programs"][index] for index in used]
            for case in kept:
                case["program"] = renumber[case["program"]]
            item["cases"] = kept
            records.append(item)
        if sibling is not None:
            records.append(sibling)
    unmatched = set(failing) - matched_failing
    if unmatched:
        corpus.fail(f"--failing names {len(unmatched)} cases the dump lacks "
                    f"or drops otherwise, e.g. {sorted(unmatched)[0]}")
    added = 0
    for feature in features:
        for record in undefined_records(feature):
            records.append(record)
            added += len(record["cases"])
    replaced = collections.Counter()
    for record in replacements(kept_cases, lost, rewriter.classifier):
        records.append(record)
        for case in record["cases"]:
            program = record["programs"][case["program"]]
            word = int(dict(program["words"])[program["entry"]], 16)
            for stable_id in rewriter.classifier.classify(word):
                replaced[stable_id] += 1
    replacement_cases = sum(len(record["cases"]) for record in records
                            if record["producer"] == REPLACEMENT_PRODUCER)
    header["meta"]["absent_features"] = {
        "input_sha256": corpus.sha256(options.dump.read_bytes()),
        "script": SITE,
        "sha256": hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
        "features": list(features),
        **({"failing": {
            "file": options.failing.resolve().relative_to(
                corpus.REPOSITORY).as_posix(),
            "sha256": corpus.sha256(options.failing.read_bytes())}}
           if options.failing is not None else {}),
        "dropped_cases": {
            f"{feature}/{reason}": count
            for (feature, reason), count in sorted(dropped.items())},
        "stripped_cases": stripped,
        "host_gates": {token: ungated[token] for token in HOST_GATES},
        "afp": {"controls": afp["controls"], "added_requirement": afp["added"],
                "removed_requirement": afp["gate"],
                "without_afp_cases": afp["pin"]},
        "sme_table": {"file": sme_table.TABLE_PATH,
                      "sha256": sme_table.load().sha256},
        "added_cases": added,
        "tbi_replacement_cases": replacement_cases,
    }
    with options.output.open("w", encoding="utf-8") as sink:
        sink.write(json.dumps(header, sort_keys=True) + "\n")
        for line in blobs:
            sink.write(line + "\n")
        for record in records:
            sink.write(json.dumps(record, sort_keys=True) + "\n")
    if options.report is not None:
        reasons = ("feature-case", "removed-encoding", "fails-stripped")
        with options.report.open("w", encoding="utf-8") as sink:
            sink.write("stable_id\t" + "\t".join(reasons) +
                       "\tkept\ttbi_replacements\n")
            for stable_id in sorted(set(by_entry) | set(kept_by_entry)):
                counts = by_entry[stable_id]
                sink.write("\t".join([stable_id, *(str(counts[reason])
                                                   for reason in reasons),
                                      str(kept_by_entry[stable_id]),
                                      str(replaced[stable_id])]) + "\n")
    print(f"dropped {sum(dropped.values())} cases "
          f"({', '.join(f'{f}/{r} {n}' for (f, r), n in sorted(dropped.items()))}), "
          f"stripped {stripped}, removed host gates "
          f"({', '.join(f'{t} {ungated[t]}' for t in HOST_GATES)}), "
          f"kept {afp['controls'] + afp['added']} FEAT_AFP cases "
          f"({afp['added']} gained the requirement, {afp['gate']} lost it), "
          f"moved {afp['pin']} RES0 pins to without_afp records, "
          f"added {added} absent-feature cases and "
          f"{replacement_cases} TBI replacements")
    return 0


if __name__ == "__main__":
    sys.exit(main())
