# SPDX-License-Identifier: GPL-2.0-or-later
"""Decide the invalid-encoding bit of an A64 illegal-instruction case.

The bit follows the convention in tests/unit/arm64/corpus/README.md. It is
set when no virtual-CPU configuration decodes the word as an instruction:

- a word the decode partitions leave unallocated, a reserved bitmask
  immediate, or a reserved operand value;
- a CONSTRAINED UNPREDICTABLE operand combination or a permanently
  undefined word, which the engines make UNDEFINED in every state; and
- an encoding of a feature the virtual CPU implements in no configuration
  (FEAT_PAuth, FEAT_MTE, FEAT_GCS, FEAT_D128, the SVE and SME rows nothing
  executes, and the other features of the README's absent-feature section).
  An SVE row executes only in streaming mode, where the SME profile does, so
  an SVE row the SME profile does not execute in either mode is such a word.

It is clear when the word is a valid encoding that traps for its exception
level, mode, or state, or because a feature that some configuration
implements (SME) is absent from this one.

decide() answers from the checked-in tables alone: the decode partitions,
the allocation table, and the generated SVE/SME encoding table. It returns
None for a word those tables do not settle (a reserved operand value or a
CONSTRAINED UNPREDICTABLE combination inside an allocated encoding, or an
EL0 trap of a named system instruction); the producer's own bit stands there.
invalid-encoding-corpus.py applies it to every case of a dump, the
absent-feature and sme-native scripts write their own cases through it, and
tests/tooling/arm64/test_arm64_instruction_corpus_invalid_encoding.py
requires every case of the pack to agree with it.
"""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import importlib.util
import pathlib
import re
import sys
from typing import Iterable, NamedTuple

HERE = pathlib.Path(__file__).resolve().parent


def _load(name: str, filename: str):
    """The sibling module, shared with the scripts that load it by the same
    name."""
    module = sys.modules.get(name)
    if module is None:
        spec = importlib.util.spec_from_file_location(name, HERE / filename)
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        spec.loader.exec_module(module)
    return module


corpus = _load("build_unit_corpus", "build-unit-corpus.py")
sme_table = _load("sme_encoding_table", "sme_encoding_table.py")

SITE = "tools/arm64-tests/invalid_encoding.py"

# Records of this option run on a virtual CPU without the feature that the
# option removes (the runner translates for the removal), so their words
# execute in another configuration.
WITHOUT_FEATURE_OPTIONS = frozenset({"without_sme"})

# The SVE/SME table features no virtual-CPU configuration implements: every
# SVE feature, since the virtual CPU executes SVE only in streaming mode, as
# the SME profile (sme, sme2, sme_f64f64, sme_i16i64, and the bf16 and i8mm
# its rows share with the host). A row's non-streaming requirement names
# "sve" wherever SVE alone would execute it outside streaming mode.
UNIMPLEMENTED_PROFILE_FEATURES = frozenset({
    "sve", "sve2", "sve_aes", "sve_pmull128", "sve_sha3", "sve_bitperm",
    "sve_sm4"})

BITMASK_ROWS = ("and_z_zi_", "eor_z_zi_", "orr_z_zi_", "dupm_z_i_")

# Allocation-table feature tokens of the features the virtual CPU never
# implements (tests/unit/arm64/corpus/README.md, "Absent-feature cases", and
# FEAT_PAuth_LR, which stays unimplemented). A word that matches an
# allocation row with one of them, or a decode partition that needs one, is
# UNDEFINED in every configuration.
OMITTED_TOKENS = frozenset({
    "cmpbr", "cpa", "d128", "f16f32dot", "f16f32mm", "f16mm", "f8f16mm",
    "f8f32mm", "faminmax", "fp8", "fp8dot2", "fp8dot4", "fp8fma", "fprcvt",
    "gcs", "hbc", "lor", "lrcpc3", "ls64", "ls64_accdata", "ls64_v", "lscp",
    "lse128", "lsfe", "lsui", "lut", "mte", "mte2", "mtetc", "pauth",
    "pauth_lr", "sm3", "sm4", "ssbs", "sysinstr128", "sysreg128", "the",
    "tlbid", "wfxt", "xs",
})
# The allocation tokens outside the SVE and SME profiles that the virtual
# CPU does implement, or that name only system and hint space, whose
# encodings are valid and trap or act as NOPs.
IMPLEMENTED_TOKENS = frozenset({
    "advanced_simd", "aes", "asmv8p2", "baseline", "bf16", "brbe", "bti",
    "chk", "clrbhb", "cmh", "crc32", "cssc", "dgh", "dotprod", "f64mm",
    "fcma", "fhm", "flag_m", "flag_m2", "floating_point", "fp16", "frintts",
    "gcie", "hinte", "i8mm", "ite", "jscvt", "lrcpc", "lrcpc2", "lse",
    "mops", "pcdphint", "ras", "rdm", "rme_gpc3", "rprfm", "sb", "sha1",
    "sha256", "sha3", "sha512", "spe", "specres", "specres2", "trf",
})
OMITTED_CONDITIONS = frozenset({
    "FEAT_CMPBR", "FEAT_CPA", "FEAT_D128", "FEAT_F16F32DOT", "FEAT_F16F32MM",
    "FEAT_F16MM", "FEAT_F8F16MM", "FEAT_F8F32MM", "FEAT_FAMINMAX",
    "FEAT_FP8", "FEAT_FP8DOT2", "FEAT_FP8DOT4", "FEAT_FP8FMA", "FEAT_FPRCVT",
    "FEAT_GCS", "FEAT_HBC", "FEAT_LOR", "FEAT_LRCPC3", "FEAT_LS64",
    "FEAT_LS64_ACCDATA", "FEAT_LS64_V", "FEAT_LSCP", "FEAT_LSE128",
    "FEAT_LSFE", "FEAT_LSUI", "FEAT_LUT", "FEAT_MTE", "FEAT_MTE2",
    "FEAT_PAuth", "FEAT_PAuth_LR", "FEAT_SM3", "FEAT_SM4", "FEAT_SSBS",
    "FEAT_SYSINSTR128", "FEAT_SYSREG128", "FEAT_THE", "FEAT_TLBID",
    "FEAT_WFxT", "FEAT_XS",
})
# (mask, value, constraint) of omitted-feature encodings the tables file
# elsewhere or not at all: MSR (immediate) is filed under FEAT_SSBS for its
# whole class, so the SSBS and TCO fields are named here, and the
# FEAT_PAuth_LR returns have no allocation row.
OMITTED_ENCODINGS = (
    (0xFFFF_F0FF, 0xD503_409F, None),  # MSR TCO, #imm
    (0xFFFF_F0FF, 0xD503_403F, None),  # MSR SSBS, #imm
    (0xFFC0_001F, 0x5500_001F, None),  # RETAASPPC, RETABSPPC <label>
    (0xFFFF_FBE0, 0xD65F_0BE0, "Rm"),  # RETAASPPCR, RETABSPPCR Xm (Xm != 31)
)

# Allocation forms of generic system and exception-generating words. They
# decode for every operand, and EL0 takes the trap for the privileged ones.
GENERIC_SYSTEM_FORMS = frozenset({
    "MRS", "MSR (register)", "SYS", "SYSL", "HVC", "SMC", "HLT", "DCPS1",
    "DCPS2", "DCPS3", "ERET", "DRPS",
})
# The FEAT_D128 pair forms: the feature is omitted whatever the operands.
SYSTEM_PAIR_FORMS = frozenset({"SYSP", "MRRS", "MSRR"})
MSR_IMMEDIATE = "MSR (immediate)"
SYSTEM_TOP_BYTES = frozenset({0xD4, 0xD5, 0xD6})
FEATURE_NAME = re.compile(r"FEAT_[A-Za-z0-9_]+")


class Decision(NamedTuple):
    invalid: bool
    reason: str


def reserved_bitmask(word: int) -> bool:
    """Whether DecodeBitMasks reserves a logical-immediate word's imm13 (N at
    bit 17, imms at bits 10:5): N:NOT(imms) selects no element size, or imms
    sets every bit of its element size."""
    n, imms = word >> 17 & 1, word >> 5 & 0x3F
    size = (n << 6 | ~imms & 0x3F).bit_length() - 1
    if size < 1:
        return True
    ones = (1 << size) - 1
    return imms & ones == ones


def omitted_condition(text: str) -> bool:
    """Whether a partition's feature expression needs a feature that is
    omitted in every alternative of its `||`."""
    return all(OMITTED_CONDITIONS.intersection(FEATURE_NAME.findall(part))
               for part in text.split("||"))


class WordClassifier:
    def __init__(self) -> None:
        self.partitions = corpus.Classifier()
        self.table = sme_table.load()
        self.rows_by_top = [
            [row for row in self.table.rows
             if (top << 24 ^ row.value) & row.mask >> 24 << 24 == 0]
            for top in range(256)]
        # (pattern, stable_id, form, tokens), indexed by top byte.
        allocations = []
        with corpus.ALLOCATIONS.open(encoding="utf-8") as source:
            for row in csv.DictReader(source, delimiter="\t"):
                tokens = frozenset(re.split(
                    r"[|+]", row["architecture_native_feature_alternatives"]))
                allocations.append((corpus.Pattern(
                    row["architecture_mask"], row["architecture_value"],
                    row["architecture_constraints"]),
                    row["stable_id"], row["architectural_form"], tokens))
        self.allocations_by_top = [
            [item for item in allocations if item[0].admits_top_byte(top)]
            for top in range(256)]
        omitted = []
        with corpus.PARTITIONS.open(encoding="utf-8") as source:
            for row in csv.DictReader(source, delimiter="\t"):
                if row["partition_kind"] == "allocated" and \
                        omitted_condition(row["feature_condition"]):
                    omitted.append(corpus.Pattern(
                        row["architecture_mask"], row["architecture_value"],
                        row["architecture_constraints"]))
        self.omitted_by_top = [
            [pattern for pattern in omitted if pattern.admits_top_byte(top)]
            for top in range(256)]
        self.tokens = frozenset().union(*(
            item[3] for items in self.allocations_by_top for item in items))

    # ------------------------------------------------------------------
    # Table facts

    def row_for(self, word: int):
        return next((row for row in self.rows_by_top[word >> 24]
                     if row.matches(word)), None)

    @staticmethod
    def implemented(requirement: Iterable[str]) -> bool:
        """Whether some virtual-CPU configuration meets a row's
        requirement: it names features and none is unimplemented."""
        requirement = set(requirement)
        return bool(requirement) and \
            not requirement & UNIMPLEMENTED_PROFILE_FEATURES

    def executes(self, row) -> bool:
        """Whether some mode of some configuration executes a table row."""
        return self.implemented(row.requirement) or \
            self.implemented(row.non_streaming_requirement)

    def leaf(self, word: int) -> tuple[str, str] | None:
        """The (kind, decode class) of the one hierarchy partition that
        holds the word."""
        leaves = [item for item in self.partitions.hierarchy_by_top[word >> 24]
                  if item[0].matches(word)]
        if len(leaves) != 1:
            return None
        return leaves[0][1], leaves[0][2]

    def partition_unallocated(self, word: int) -> bool:
        """Whether the decode partitions leave the word unallocated."""
        leaf = self.leaf(word)
        if leaf is None:
            return False
        kind, decode_class = leaf
        if kind == "unallocated":
            return True
        rows = [item for item in self.partitions.tables[decode_class]
                if item[0].matches(word)]
        return len(rows) == 1 and rows[0][1] == "unallocated"

    def reserved_arrangement(self, word: int) -> bool:
        """Whether an Advanced SIMD shift by immediate names the 64-bit
        element arrangement with Q = 0 (immh<3> = 1), which every
        instruction of the class reserves."""
        leaf = self.leaf(word)
        return leaf is not None and leaf[1] == "asimdshf" and \
            not word >> 30 & 1 and bool(word >> 22 & 1)

    def allocations(self, word: int) -> list[tuple]:
        return [item for item in self.allocations_by_top[word >> 24]
                if item[0].matches(word)]

    def omitted_encoding(self, word: int, matches: list[tuple]) -> bool:
        """Whether the word is an encoding of an omitted feature."""
        tokens = frozenset().union(*(item[3] for item in matches))
        if tokens & OMITTED_TOKENS or any(
                pattern.matches(word)
                for pattern in self.omitted_by_top[word >> 24]):
            return True
        for mask, value, constraint in OMITTED_ENCODINGS:
            if word & mask == value and \
                    (constraint is None or word & 31 != 31):
                return True
        return False

    # ------------------------------------------------------------------

    def decide(self, word: int,
               options: Iterable[str] = ()) -> Decision | None:
        """The bit of an illegal-instruction case whose trapping word is
        `word` in a record of `options`, or None when the tables do not
        settle it."""
        row = self.row_for(word)
        if row is not None:
            if row.encoding in BITMASK_ROWS and reserved_bitmask(word):
                return Decision(True, "reserved bitmask immediate")
            if not self.executes(row):
                return Decision(True, "SVE/SME row no configuration executes")
            return Decision(False, "SVE/SME row some mode or configuration "
                                   "executes")
        if self.partition_unallocated(word):
            return Decision(True, "unallocated word")
        if self.reserved_arrangement(word):
            return Decision(True, "reserved arrangement")
        if WITHOUT_FEATURE_OPTIONS.intersection(options):
            return Decision(False, "word of a feature another configuration "
                                   "implements")
        matches = self.allocations(word)
        forms = {item[2] for item in matches}
        if forms & SYSTEM_PAIR_FORMS:
            return Decision(True, "FEAT_D128 system pair form")
        if word >> 24 in SYSTEM_TOP_BYTES and forms & GENERIC_SYSTEM_FORMS:
            return Decision(False, "generic system word that traps at EL0")
        if forms and forms <= {MSR_IMMEDIATE}:
            # The class is filed under FEAT_SSBS: only the omitted fields
            # are decided; a reserved PSTATE field is left to the producer.
            if self.omitted_encoding(word, []):
                return Decision(True, "MSR (immediate) of an omitted field")
            return None
        if self.omitted_encoding(word, matches):
            return Decision(True, "encoding of a feature never implemented")
        return None

    def trapping_word(self, record: dict, case: dict) -> int | None:
        """The word an illegal-instruction case traps on, when the case
        starts there: its entry word. A case that retires instructions
        first traps further on, which the tables do not decide."""
        if case["event"] != "illegal-instruction" or case["retired"]:
            return None
        program = record["programs"][case["program"]]
        entry = program["entry"]
        for offset, word in program["words"]:
            if offset == entry:
                return int(word, 16)
        return None

    def decide_case(self, record: dict, case: dict) -> Decision | None:
        word = self.trapping_word(record, case)
        if word is None:
            return None
        return self.decide(word, record["options"])


_classifier: WordClassifier | None = None


def classifier() -> WordClassifier:
    """The shared classifier; building it reads the tables once."""
    global _classifier
    if _classifier is None:
        _classifier = WordClassifier()
    return _classifier
