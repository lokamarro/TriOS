#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the gadget engine's AArch32 gadgets and form plans.

The gadget engine runs A32 and T32 programs as it runs A64 ones: a
translated block is a stream of gadget pointers and inline operand words, and
a gadget is a few host instructions that work on CpuState and the guest's
NZCV in x27. The shared decoder (cpu/arm64/aarch32/decode.hpp) says what each
word is; this generator writes, at build time from the decoder's table
(cpu/arm64/aarch32/a32_encodings.{hpp,cpp}), the gadgets for the families
whose lowering is a template, and a header that maps every form the profile's
base level implements to the family and operation the builder lowers it as.

CpuState holds R0-R14 in registers[0..14], each a 64-bit slot whose low 32
bits are the register and whose upper 32 bits are always zero (a gadget only
stores 32-bit values). registers[15] and registers[16] are the part's scratch
slots: the builder puts the value of PC read as an operand into one of them
and names it in place of R15, and a gadget that writes PC writes the slot for
an indirect-branch gadget to read. A register operand in a stream is its
byte offset in CpuState, so an operand needs eight bits.

Every family's operand layout is below its generator function. The builder
(gadgets/a32/program_builder_a32.cpp) emits exactly these layouts; the
instruction corpus and the differential tests check the two together.

usage: generate_a32.py --table a32_encodings.hpp --assembly OUT.inc
                       --header OUT.hpp
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import a32_armv8  # noqa: E402
import a32_neon  # noqa: E402  (the complete level; this file's sibling)
import a32_parallel  # noqa: E402  (parallel add and subtract, SEL)
import a32_simd  # noqa: E402  (VFP and Advanced SIMD; this file's sibling)
import a32_structure  # noqa: E402  (VLDn and VSTn)


class GenerationError(Exception):
    pass


# ---------------------------------------------------------------------------
# The decoder's table

@dataclass(frozen=True)
class FormRow:
    index: int
    name: str
    mnemonic: str
    isa: str
    level: str
    availability: str
    check: str
    traits: str
    program: str


FORM_ROW = re.compile(
    r'^\t\{"(?P<name>[^"]*)", "(?P<mnemonic>[^"]*)", '
    r'(?P<witness>0x[0-9a-f]+u), (?P<mask>0x[0-9a-f]+u), '
    r'(?P<value>0x[0-9a-f]+u), (?P<program>[^,]+), (?P<traits>[^,]+), '
    r'Isa::(?P<isa>\w+), Level::(?P<level>\w+), '
    r'Availability::(?P<availability>\w+), Check::(?P<check>\w+), ',
    re.M)


def load_forms(table: Path) -> tuple[list[str], list[FormRow]]:
    header = table.read_text()
    enum = re.search(r"enum class Form : std::uint16_t \{(.*?)\};", header, re.S)
    if enum is None:
        raise GenerationError("no Form enumeration in " + str(table))
    names = [n.strip() for n in enum.group(1).split(",") if n.strip()]
    source = table.with_suffix(".cpp").read_text()
    start = source.index("const FormInfo kForms[kFormCount] = {")
    rows = []
    for match in FORM_ROW.finditer(source, start):
        rows.append(FormRow(len(rows), match["name"], match["mnemonic"],
                            match["isa"], match["level"],
                            match["availability"], match["check"],
                            match["traits"].strip(), match["program"]))
    if len(rows) != len(names):
        raise GenerationError(
            f"{len(rows)} form rows for {len(names)} forms")
    for name, row in zip(names, rows):
        if name != row.name:
            raise GenerationError(f"form {name} is row {row.name}")
    return names, rows


# ---------------------------------------------------------------------------
# What the builder lowers a form as

# The families, in the C++ enumeration's order (a32_forms.hpp).
FAMILIES = [
    "none",          # not at the base level, or a form the profile removes
    "dp",            # data processing; sub = operation, shape from the name
    "movw",          # MOVW and the A32/T32 16-bit immediate moves
    "movt",
    "adr",           # ADR: a constant from the PC
    "mul",           # MUL, MLA, MLS (S forms by operand)
    "mull",          # UMULL SMULL UMLAL SMLAL UMAAL
    "mulh",          # SMMUL SMMLA SMMLS (+R)
    "mulxy",         # SMULxy SMLAxy SMULWy SMLAWy SMLALxy
    "div",           # SDIV UDIV
    "extend",        # SXTB SXTH UXTB UXTH and the accumulating forms
    "bits",          # REV REV16 REVSH RBIT CLZ
    "field",         # UBFX SBFX BFI BFC
    "sat",           # SSAT USAT
    "pkh",           # PKHBT PKHTB
    "library",       # the forms the AArch32 semantic library executes
    "load",          # single loads
    "store",         # single stores
    "loadd",         # LDRD
    "stored",        # STRD
    "ldm",           # LDM and its variants, POP
    "stm",           # STM and its variants, PUSH
    "exclusive",     # LDREX* STREX*, LDAEX* STLEX*
    "ordered",       # LDA LDAB LDAH STL STLB STLH
    "branch",        # B
    "bl",            # BL BLX (immediate)
    "bx",            # BX BXJ
    "blx",           # BLX (register)
    "cbz",           # CBZ CBNZ
    "tb",            # TBB TBH
    "it",            # IT
    "svc",
    "bkpt",
    "udf",
    "hint",          # NOP and the hints that execute as one
    "barrier",       # DMB DSB ISB
    "clrex",
    "mrs",
    "msr",
    "cp15",          # MCR MRC MRRC with the sysreg the decoder names
] + a32_simd.FAMILIES + a32_armv8.FAMILIES + a32_parallel.FAMILIES

DP_MNEMONICS = {
    "AND": "and", "EOR": "eor", "SUB": "sub", "RSB": "rsb", "ADD": "add",
    "ADC": "adc", "SBC": "sbc", "RSC": "rsc", "TST": "tst", "TEQ": "teq",
    "CMP": "cmp", "CMN": "cmn", "ORR": "orr", "MOV": "mov", "BIC": "bic",
    "MVN": "mvn", "ORN": "orn",
}
# The mnemonics the decoder gives the flag-setting forms and the 12-bit
# immediate forms of the same operations.
DP_ALIASES = {m + "S": m for m in
              ("AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC", "ORR",
               "MOV", "BIC", "MVN", "ORN")}
DP_ALIASES.update({"ADDW": "ADD", "SUBW": "SUB"})

# Operation numbering shared with a32_forms.hpp.
DP_ORDER = ["and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst",
            "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn", "orn"]
LOGICAL = {"and", "eor", "tst", "teq", "orr", "mov", "bic", "mvn", "orn"}
COMPARE = {"tst", "teq", "cmp", "cmn"}
UNARY = {"mov", "mvn"}

LOAD_STORE = {
    "LDR": ("load", 0), "LDRB": ("load", 1), "LDRH": ("load", 2),
    "LDRSB": ("load", 3), "LDRSH": ("load", 4),
    "STR": ("store", 0), "STRB": ("store", 1), "STRH": ("store", 2),
    "LDRT": ("load", 0), "LDRBT": ("load", 1), "LDRHT": ("load", 2),
    "LDRSBT": ("load", 3), "LDRSHT": ("load", 4),
    "STRT": ("store", 0), "STRBT": ("store", 1), "STRHT": ("store", 2),
}

MULL = {"UMULL": 0, "SMULL": 1, "UMLAL": 2, "SMLAL": 3, "UMAAL": 4,
        "UMULLS": 0, "SMULLS": 1, "UMLALS": 2, "SMLALS": 3}
MULH = {"SMMUL": 0, "SMMULR": 1, "SMMLA": 2, "SMMLAR": 3, "SMMLS": 4,
        "SMMLSR": 5}
MULXY = {"SMULBB": 0, "SMULBT": 1, "SMULTB": 2, "SMULTT": 3,
         "SMLABB": 4, "SMLABT": 5, "SMLATB": 6, "SMLATT": 7,
         "SMULWB": 8, "SMULWT": 9, "SMLAWB": 10, "SMLAWT": 11,
         "SMLALBB": 12, "SMLALBT": 13, "SMLALTB": 14, "SMLALTT": 15}
EXTEND = {"SXTB": 0, "SXTH": 1, "UXTB": 2, "UXTH": 3,
          "SXTAB": 4, "SXTAH": 5, "UXTAB": 6, "UXTAH": 7}
BITS = {"REV": 0, "REV16": 1, "REVSH": 2, "RBIT": 3, "CLZ": 4}
FIELD = {"UBFX": 0, "SBFX": 1, "BFI": 2, "BFC": 3}

# The forms the engine sends to the shared AArch32 semantic library
# (cpu/arm64/aarch32/semantics.hpp) instead of lowering: USAD8, the dual
# 16-bit multiplies, the 16-bit saturates and extends, and the saturating
# arithmetic of QADD and its relatives. The library-exit count gates name any
# of them that turns hot.
def library_mnemonics() -> list[str]:
    return ["USAD8", "USADA8", "SSAT16", "USAT16",
            "SMUAD", "SMUADX", "SMUSD", "SMUSDX", "SMLAD",
            "SMLADX", "SMLSD", "SMLSDX", "SMLALD", "SMLALDX",
            "SMLSLD", "SMLSLDX", "SXTB16", "UXTB16", "SXTAB16",
            "UXTAB16", "QADD", "QSUB", "QDADD", "QDSUB"]


def plan_of(row: FormRow) -> tuple[str, int, int]:
    """(family, sub, aux) for a form of the base or simd level, or
    ("none", 0, 0)."""
    if row.availability != "implemented":
        return ("none", 0, 0)
    if row.level == "simd":
        return a32_simd.plan_of_simd(row.name, row.mnemonic) or \
            ("none", 0, 0)
    if row.level == "complete":
        # The Armv8 additions and the optional extensions: lowered where
        # a32_armv8.py has a gadget, the shared semantic library otherwise.
        return a32_armv8.plan_of_complete(row.name, row.mnemonic) or \
            ("library", 0, 0)
    if row.level != "base":
        return ("none", 0, 0)
    name, mnemonic = row.name, row.mnemonic
    if name in ("Undefined",):
        return ("none", 0, 0)
    if name == "ReservedHint":
        return ("hint", 0, 0)
    base = DP_ALIASES.get(mnemonic, mnemonic)
    if base in DP_MNEMONICS and mnemonic not in ("ADR",):
        shape = 2 if "_rr_" in name else 1 if "_r_" in name else 0
        # The SP-relative spellings are the same operations with R13 as the
        # first operand, which the encoding does not name (aux bit 2).
        if "_SP_r_" in name:
            shape = 1
        if "_SP_" in name:
            shape |= 4
        return ("dp", DP_ORDER.index(DP_MNEMONICS[base]), shape)
    if mnemonic == "MOVW":
        return ("movw", 0, 0)
    if mnemonic == "MOVT":
        return ("movt", 0, 0)
    if mnemonic == "ADR":
        return ("adr", 0, 0)
    if mnemonic in ("MUL", "MULS", "MLA", "MLAS", "MLS"):
        return ("mul", {"MUL": 0, "MULS": 0, "MLA": 1, "MLAS": 1,
                        "MLS": 2}[mnemonic], 0)
    if mnemonic in MULL:
        return ("mull", MULL[mnemonic], 0)
    if mnemonic in MULH:
        return ("mulh", MULH[mnemonic], 0)
    if mnemonic in MULXY:
        return ("mulxy", MULXY[mnemonic], 0)
    if mnemonic in ("SDIV", "UDIV"):
        return ("div", 0 if mnemonic == "SDIV" else 1, 0)
    if mnemonic in EXTEND:
        return ("extend", EXTEND[mnemonic], 0)
    if mnemonic in BITS:
        return ("bits", BITS[mnemonic], 0)
    if mnemonic in FIELD:
        return ("field", FIELD[mnemonic], 0)
    if mnemonic in ("SSAT", "USAT"):
        return ("sat", 0 if mnemonic == "SSAT" else 1, 0)
    if mnemonic in ("PKHBT", "PKHTB"):
        return ("pkh", 0 if mnemonic == "PKHBT" else 1, 0)
    parallel = a32_parallel.plan_of_parallel(mnemonic)
    if parallel is not None:
        return parallel
    if mnemonic in library_mnemonics():
        return ("library", 0, 0)
    if mnemonic in LOAD_STORE:
        family, sub = LOAD_STORE[mnemonic]
        aux = (1 if "_l_" in name else 0) | (2 if "_r_" in name or
                                              name.endswith("_A2") and
                                              mnemonic.endswith("T") else 0)
        if mnemonic.endswith("T") and mnemonic not in ("LDRT",):
            aux |= 4
        if mnemonic == "LDRT" or mnemonic == "STRT":
            aux |= 4
        return (family, sub, aux)
    if mnemonic == "LDRD":
        return ("loadd", 0, 1 if "_l_" in name else 0)
    if mnemonic == "STRD":
        return ("stored", 0, 0)
    if mnemonic in ("LDM", "STM", "POP", "PUSH"):
        mode = 0  # increment after
        if "DA_" in name:
            mode = 1
        elif "DB_" in name or mnemonic == "PUSH":
            mode = 2
        elif "IB_" in name:
            mode = 3
        return ("ldm" if mnemonic in ("LDM", "POP") else "stm", mode,
                1 if mnemonic in ("POP", "PUSH") else 0)
    if mnemonic in ("LDREX", "LDREXB", "LDREXH", "LDREXD", "STREX",
                    "STREXB", "STREXH", "STREXD", "LDAEX", "LDAEXB",
                    "LDAEXH", "LDAEXD", "STLEX", "STLEXB", "STLEXH",
                    "STLEXD"):
        size = {"": 2, "B": 0, "H": 1, "D": 3}[mnemonic[5:]]
        # aux: bit 0 store, bit 1 acquire (load) or release (store)
        return ("exclusive", size,
                (1 if mnemonic[:3] in ("STR", "STL") else 0) |
                (2 if mnemonic[2] in ("A", "L") else 0))
    if mnemonic in ("LDA", "LDAB", "LDAH", "STL", "STLB", "STLH"):
        return ("ordered", {"": 2, "B": 0, "H": 1}[mnemonic[3:]],
                1 if mnemonic.startswith("STL") else 0)
    if mnemonic == "B":
        return ("branch", 0, 0)
    if mnemonic in ("BL", "BLX") and "_i_" in name:
        return ("bl", 0, 0)
    if mnemonic in ("BX", "BXJ"):
        return ("bx", 0, 0)
    if mnemonic == "BLX":
        return ("blx", 0, 0)
    if mnemonic in ("CBZ", "CBNZ"):
        return ("cbz", 0 if mnemonic == "CBZ" else 1, 0)
    if mnemonic in ("TBB", "TBH"):
        return ("tb", 0 if mnemonic == "TBB" else 1, 0)
    if mnemonic == "IT":
        return ("it", 0, 0)
    if mnemonic == "SVC":
        return ("svc", 0, 0)
    if mnemonic == "BKPT":
        return ("bkpt", 0, 0)
    if mnemonic == "UDF":
        return ("udf", 0, 0)
    if mnemonic in ("NOP", "YIELD", "WFE", "WFI", "SEV", "SEVL", "DBG", "CSDB",
                    "ESB", "TSB", "CLRBHB", "SSBB", "PSSBB", "PLD", "PLDW",
                    "PLI", "CPS", "CPSID", "CPSIE"):
        return ("hint", 0, 1 if mnemonic in ("YIELD", "WFE", "WFI") else 0)
    if mnemonic in ("DMB", "DSB", "ISB"):
        return ("barrier", {"DMB": 0, "DSB": 1, "ISB": 2}[mnemonic], 0)
    if mnemonic == "CLREX":
        return ("clrex", 0, 0)
    if mnemonic == "MRS":
        return ("mrs", 0, 0)
    if mnemonic == "MSR":
        return ("msr", 0, 0)
    if mnemonic in ("MCR", "MRC", "MRRC", "MCRR"):
        return ("cp15", 0, 0)
    return ("none", 0, 1)


# ---------------------------------------------------------------------------
# Assembly

class Assembly:
    def __init__(self) -> None:
        self.text: list[str] = []
        self.names: list[str] = []

    def line(self, text: str = "") -> None:
        self.text.append(text)

    def gadget(self, name: str, body: str) -> None:
        if name in self.names:
            raise GenerationError("duplicate gadget " + name)
        self.names.append(name)
        self.text.append(f"gadget a32_{name}")
        for raw in body.strip("\n").splitlines():
            stripped = raw.strip()
            if not stripped:
                continue
            if stripped.endswith(":") or stripped.startswith("."):
                self.text.append(stripped)
            else:
                self.text.append("\t" + stripped)
        self.text.append("")


# The flag registers: x27 holds NZCV in the host format. A logical result
# updates N and Z and (with a shifter carry) C, and keeps V.
def nz_update(result: str, scratch: str = "w14") -> str:
    return f"""
        tst {result}, {result}
        mrs {scratch.replace('w', 'x')}, nzcv
        and {scratch}, {scratch}, #0xc0000000
        and w27, w27, #0x30000000
        orr w27, w27, {scratch}
    """


def dp_shape_body(op: str, s: bool, shape: str) -> str:
    """One data-processing gadget. The operand word is

      imm shapes     rd[0:8] rn[8:16] carry[16] imm32[32:64]
      reg shapes     rd[0:8] rn[8:16] rm[16:24] rs[24:32] aux[32:]
      aux (shifts by an immediate) amount[0:6] carry-bit-index[6:12]

    rd and rn are CpuState offsets of R0-R14 or the part's scratch slots.
    The comparison forms have no rd, and MOV and MVN no rn. A logical
    operation with S set updates N and Z from the result and C from the
    shifter's carry-out (for a shape that has one), and keeps V.
    """
    lines = ["ldr x8, [_stream], #8"]
    has_rn = op not in UNARY
    has_rd = op not in COMPARE
    logical = op in LOGICAL
    carry_out = s and logical and shape not in ("imm", "reg")
    if has_rn:
        lines += ["ubfx x9, x8, #8, #8", "ldr w9, [_vcpu, x9]"]
    op2 = "w12"
    if shape in ("imm", "imc"):
        lines.append("lsr x12, x8, #32")
        if shape == "imc":
            lines.append("ubfx x13, x8, #16, #1")
    else:
        lines += ["ubfx x10, x8, #16, #8", "ldr w10, [_vcpu, x10]"]
        if shape == "reg":
            op2 = "w10"
        elif shape in ("lsl", "lsr", "asr", "ror", "rrx"):
            if shape != "rrx":
                lines.append("ubfx x14, x8, #32, #6")
            if shape == "lsl":
                lines.append("lsl w12, w10, w14")
            elif shape == "lsr":
                lines.append("lsr x12, x10, x14")
            elif shape == "asr":
                lines += ["sxtw x12, w10", "asr x12, x12, x14"]
            elif shape == "ror":
                lines.append("ror w12, w10, w14")
            else:
                lines += ["ubfx x13, x27, #29, #1", "lsr w12, w10, #1",
                          "bfi w12, w13, #31, #1"]
            if carry_out:
                lines += ["ubfx x15, x8, #38, #6", "lsr w13, w10, w15",
                          "and w13, w13, #1"]
        else:
            kind = shape[1:]
            lines += ["ubfx x14, x8, #24, #8", "ldr w14, [_vcpu, x14]",
                      "and w14, w14, #0xff"]
            if carry_out:
                lines.append("ubfx x17, x27, #29, #1")
            if kind == "lsl":
                if not carry_out:
                    lines += ["cmp w14, #32", "lsl w12, w10, w14",
                              "csel w12, wzr, w12, hs"]
                else:
                    lines += ["mov w15, #63", "cmp w14, w15",
                              "csel w15, w14, w15, lo", "lsl x16, x10, x15",
                              "lsr x13, x16, #32", "and w13, w13, #1",
                              "mov w12, w16", "cmp w14, #0",
                              "csel w13, w17, w13, eq"]
            elif kind == "lsr":
                if not carry_out:
                    lines += ["mov w15, #63", "cmp w14, w15",
                              "csel w15, w14, w15, lo", "lsr x12, x10, x15"]
                else:
                    lines += ["sub w15, w14, #1", "mov w16, #63",
                              "cmp w15, w16", "csel w15, w15, w16, lo",
                              "lsr x16, x10, x15", "and w13, w16, #1",
                              "lsr x12, x16, #1", "cmp w14, #0",
                              "csel w12, w10, w12, eq",
                              "csel w13, w17, w13, eq"]
            elif kind == "asr":
                if not carry_out:
                    lines += ["mov w15, #63", "cmp w14, w15",
                              "csel w15, w14, w15, lo", "sxtw x12, w10",
                              "asr x12, x12, x15"]
                else:
                    lines += ["sub w15, w14, #1", "mov w16, #63",
                              "cmp w15, w16", "csel w15, w15, w16, lo",
                              "sxtw x16, w10", "asr x16, x16, x15",
                              "and w13, w16, #1", "asr x12, x16, #1",
                              "cmp w14, #0", "csel w12, w10, w12, eq",
                              "csel w13, w17, w13, eq"]
            else:
                lines.append("ror w12, w10, w14")
                if carry_out:
                    lines += ["lsr w13, w12, #31", "cmp w14, #0",
                              "csel w13, w17, w13, eq"]
    # The operation.
    a, b = "w9", op2
    sets = s or op in COMPARE
    carry_in = op in ("adc", "sbc", "rsc") or (shape == "rrx" and not
                                               carry_out and False)
    if op in ("adc", "sbc", "rsc"):
        lines.append("msr nzcv, x27")
    suffix = "s" if sets and not logical else ""
    if op in ("add", "cmn"):
        lines.append(f"add{suffix} w11, {a}, {b}")
    elif op == "adc":
        lines.append(f"adc{suffix} w11, {a}, {b}")
    elif op in ("sub", "cmp"):
        lines.append(f"sub{suffix} w11, {a}, {b}")
    elif op == "sbc":
        lines.append(f"sbc{suffix} w11, {a}, {b}")
    elif op == "rsb":
        lines.append(f"sub{suffix} w11, {b}, {a}")
    elif op == "rsc":
        lines.append(f"sbc{suffix} w11, {b}, {a}")
    elif op in ("and", "tst"):
        lines.append(f"and w11, {a}, {b}")
    elif op in ("eor", "teq"):
        lines.append(f"eor w11, {a}, {b}")
    elif op == "orr":
        lines.append(f"orr w11, {a}, {b}")
    elif op == "bic":
        lines.append(f"bic w11, {a}, {b}")
    elif op == "orn":
        lines.append(f"orn w11, {a}, {b}")
    elif op == "mov":
        lines.append(f"mov w11, {b}")
    elif op == "mvn":
        lines.append(f"mvn w11, {b}")
    else:
        raise GenerationError(op)
    if sets:
        if logical:
            lines.append(nz_update("w11"))
            if carry_out or shape == "imc":
                lines.append("bfi w27, w13, #29, #1")
        else:
            lines.append("mrs x27, nzcv")
    if has_rd:
        lines += ["and x8, x8, #0xff", "str w11, [_vcpu, x8]"]
    lines.append("next_gadget")
    return "\n".join(lines)


# In-place ADD and SUB of an immediate (Rd == Rn, an ordinary register) with
# the CpuState displacement baked into the gadget, one gadget per register.
# The ordinary gadget reads and writes the register through a byte offset the
# stream word carries, "ldr w9, [_vcpu, x9]" and "str w11, [_vcpu, x8]", and
# the core forwards a dependent store-to-load pair at rename only when both
# accesses name a base register and a constant displacement; the induction
# variables and accumulators this shape is are what waits. The stream word
# keeps the ordinary layout, so its two register fields are dead here.
INPLACE_REGISTERS = 15


def dp_inplace_body(subtract: bool, s: bool, register: int) -> str:
    operation = ("sub" if subtract else "add") + ("s" if s else "")
    lines = ["ldr x8, [_stream], #8",
             f"ldr w9, [_vcpu, #{8 * register}]", "lsr x12, x8, #32",
             f"{operation} w11, w9, w12"]
    if s:
        lines.append("mrs x27, nzcv")
    lines += [f"str w11, [_vcpu, #{8 * register}]", "next_gadget"]
    return "\n".join(lines)


def inplace_name(subtract: bool, s: bool, register: int) -> str:
    return f"dp_{'sub' if subtract else 'add'}{'s' if s else ''}_imm_r{register}"


def emit_dp_inplace(asm: Assembly) -> None:
    for subtract in (False, True):
        for s in (False, True):
            for register in range(INPLACE_REGISTERS):
                asm.gadget(inplace_name(subtract, s, register),
                           dp_inplace_body(subtract, s, register))


def emit_dp(asm: Assembly) -> list[tuple[str, str, bool, str]]:
    """Every data-processing gadget; returns (op, shape, s) of each."""
    produced = []
    shifts = ["lsl", "lsr", "asr", "ror", "rrx"]
    regshifts = ["rlsl", "rlsr", "rasr", "rror"]
    for op in DP_ORDER:
        s_values = [True] if op in COMPARE else [False, True]
        for s in s_values:
            shapes = ["imm"]
            if op in LOGICAL and s:
                shapes.append("imc")
            shapes += ["reg"] + shifts + regshifts
            for shape in shapes:
                name = f"dp_{op}{'s' if s and op not in COMPARE else ''}_{shape}"
                asm.gadget(name, dp_shape_body(op, s, shape))
                produced.append((op, shape, s, name))
    return produced


# Conditional data processing that sets no flags, as a select instead of a
# skip: the result, or the register's old value where the condition fails.
# The skip gadget's branch follows the guest's condition, which the image
# kernels compute from their data, so it mispredicts where the select does
# not branch at all. The operand word holds the condition as a truth table
# over NZCV (bit n is whether the condition passes with NZCV = n), where an
# ordinary data-processing word keeps its carry and register fields:
#
#   imm    table[0:16] rd[16:24] rn[24:32] imm32[32:64]
#   reg    table[0:16] rd[16:24] rn[24:32] rm[32:40]
SELECT_OPS = ["mov", "mvn", "add", "sub", "rsb", "and", "orr", "eor", "bic"]
SELECT_SHAPES = ["imm", "reg"]


def select_name(op: str, shape: str) -> str:
    return f"dp_{op}_{shape}_select"


def select_body(op: str, shape: str) -> str:
    lines = ["ldr x8, [_stream], #8"]
    if op not in UNARY:
        lines += ["ubfx x9, x8, #24, #8", "ldr w9, [_vcpu, x9]"]
    if shape == "imm":
        lines.append("lsr x12, x8, #32")
    else:
        lines += ["ubfx x12, x8, #32, #8", "ldr w12, [_vcpu, x12]"]
    a, b = "w9", "w12"
    lines.append({"mov": f"mov w11, {b}", "mvn": f"mvn w11, {b}",
                  "add": f"add w11, {a}, {b}", "sub": f"sub w11, {a}, {b}",
                  "rsb": f"sub w11, {b}, {a}", "and": f"and w11, {a}, {b}",
                  "orr": f"orr w11, {a}, {b}", "eor": f"eor w11, {a}, {b}",
                  "bic": f"bic w11, {a}, {b}"}[op])
    lines += ["ubfx x10, x8, #16, #8", "ldr w13, [_vcpu, x10]",
              "lsr w14, w27, #28", "lsr x15, x8, x14", "tst x15, #1",
              "csel w11, w11, w13, ne", "str w11, [_vcpu, x10]",
              "next_gadget"]
    return "\n".join(lines)


def emit_select(asm: Assembly) -> None:
    for op in SELECT_OPS:
        for shape in SELECT_SHAPES:
            asm.gadget(select_name(op, shape), select_body(op, shape))


# ---------------------------------------------------------------------------
# Memory

# A memory gadget's stream is [gadget][operand word ...]. The operand holds
# the instruction's position in its block at bit 24 (a32_layout.h), and x19
# keeps it through the translation helpers: a fault exits through
# La32_position_fault, which makes the position the PC.
ADDRESS_MODES = ["i", "ipre", "ipost", "ra", "rs", "rapre", "rspre",
                 "rapost", "rspost"]


def address_lines(mode: str) -> tuple[list[str], bool]:
    """Compute the guest address into w7 (and the writeback value into w20)
    from the operand word in x19; returns (lines, writes_back).

      imm modes   rt[0:8] rn[8:16] imm32[32:64]
      reg modes   rt[0:8] rn[8:16] rm[16:24] lsl-amount[32:38]
      abs         rt[0:8] address[32:64]
    """
    if mode == "abs":
        return (["lsr x7, x19, #32"], False)
    lines = ["ubfx x9, x19, #8, #8", "ldr w9, [_vcpu, x9]"]
    if mode.startswith("i"):
        lines.append("lsr x10, x19, #32")
        if mode == "i":
            lines.append("add w7, w9, w10")
            return (lines, False)
        if mode == "ipre":
            lines += ["add w7, w9, w10", "mov w20, w7"]
        else:
            lines += ["mov w7, w9", "add w20, w9, w10"]
        return (lines, True)
    lines += ["ubfx x10, x19, #16, #8", "ldr w10, [_vcpu, x10]",
              "ubfx x11, x19, #32, #6", "lsl w10, w10, w11"]
    operation = "add" if mode[1] == "a" else "sub"
    if mode in ("ra", "rs"):
        lines.append(f"{operation} w7, w9, w10")
        return (lines, False)
    if mode.endswith("pre"):
        lines += [f"{operation} w7, w9, w10", "mov w20, w7"]
    else:
        lines += ["mov w7, w9", f"{operation} w20, w9, w10"]
    return (lines, True)


def writeback_lines() -> list[str]:
    return ["ubfx x9, x19, #8, #8", "str w20, [_vcpu, x9]"]


LOAD_SIZES = {
    "w": (4, "ldr w0, [x7]", "fmov w0, s0", None),
    "b": (1, "ldrb w0, [x7]", None, None),
    "h": (2, "ldrh w0, [x7]", "umov w0, v0.h[0]", None),
    "sb": (1, "ldrsb w0, [x7]", None, None),
    "sh": (2, "ldrsh w0, [x7]", "umov w0, v0.h[0]", "sxth w0, w0"),
}
STORE_SIZES = {
    "w": (4, "str w0, [x7]", "fmov s0, w0"),
    "b": (1, "strb w0, [x7]", None),
    "h": (2, "strh w0, [x7]", "fmov s0, w0"),
}


def load_single(name: str, size: str, mode: str) -> str:
    byte_count, load, cross_extract, extend = LOAD_SIZES[size]
    address, writes_back = address_lines(mode)
    label = name
    lines = ["ldr x19, [_stream], #8"] + address
    lines.append(
        f"a32_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, {byte_count},"
        f" x7, Lcold_{label}, Lresume_{label}, Lcross_{label}, 0,"
        f" fault=La32_position_fault")
    lines += [load, "and x8, x19, #0xff", "str w0, [_vcpu, x8]"]
    if writes_back:
        lines += writeback_lines()
    lines.append("next_gadget")
    lines.append(
        f"a32_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE,"
        f" {byte_count}, x7, Lcold_{label}, Lresume_{label}, Lcross_{label},"
        f" 0, fault=La32_position_fault")
    if byte_count > 1:
        lines.append(f"Lcross_{label}:")
        lines.append(f"transfer_slow 0, x7, {byte_count},"
                     f" fault=La32_position_fault")
        lines.append(cross_extract)
        if extend:
            lines.append(extend)
        lines += ["and x8, x19, #0xff", "str w0, [_vcpu, x8]"]
        if writes_back:
            lines += writeback_lines()
        lines.append("next_gadget")
    return "\n".join(lines)


def store_single(name: str, size: str, mode: str) -> str:
    byte_count, store, cross_pack = STORE_SIZES[size]
    address, writes_back = address_lines(mode)
    label = name
    lines = ["ldr x19, [_stream], #8"] + address
    lines.append(
        f"a32_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE,"
        f" {byte_count}, x7, Lcold_{label}, Lresume_{label}, Lcross_{label},"
        f" 0, fault=La32_position_fault")
    lines += ["and x8, x19, #0xff", "ldr w0, [_vcpu, x8]", store]
    if writes_back:
        lines += writeback_lines()
    lines.append("next_gadget")
    lines.append(
        f"a32_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE,"
        f" {byte_count}, x7, Lcold_{label}, Lresume_{label}, Lcross_{label},"
        f" 0, fault=La32_position_fault")
    if byte_count > 1:
        lines.append(f"Lcross_{label}:")
        lines += ["and x8, x19, #0xff", "ldr w0, [_vcpu, x8]", cross_pack,
                  f"transfer_slow 1, x7, {byte_count},"
                  f" fault=La32_position_fault"]
        if writes_back:
            lines += writeback_lines()
        lines.append("next_gadget")
    return "\n".join(lines)


def emit_single(asm: Assembly) -> None:
    for size in LOAD_SIZES:
        for mode in ADDRESS_MODES + ["abs"]:
            name = f"ld_{size}_{mode}"
            asm.gadget(name, load_single(name, size, mode))
    for size in STORE_SIZES:
        for mode in ADDRESS_MODES:
            name = f"st_{size}_{mode}"
            asm.gadget(name, store_single(name, size, mode))


# LDRD and STRD move two words that must be word aligned (a32lsmd): the
# stream is as a single transfer's, with rt[0:8] and rt2[8:16] in place of
# the data register, and rn at [16:24], rm at [32:40] (lsl amount 0 only).
DOUBLE_MODES = ["i", "ipre", "ipost", "ra", "rs", "rapre", "rspre",
                "rapost", "rspost", "abs"]


def double_address_lines(mode: str) -> tuple[list[str], bool]:
    """As address_lines, for the doubleword layout:

      imm modes   rt[0:8] rt2[8:16] rn[16:24] imm32[32:64]
      reg modes   rt[0:8] rt2[8:16] rn[16:24] rm[32:40]
      abs         rt[0:8] rt2[8:16] address[32:64]
    """
    if mode == "abs":
        return (["lsr x7, x19, #32"], False)
    lines = ["ubfx x9, x19, #16, #8", "ldr w9, [_vcpu, x9]"]
    if mode.startswith("i"):
        lines.append("lsr x10, x19, #32")
        if mode == "i":
            lines.append("add w7, w9, w10")
            return (lines, False)
        if mode == "ipre":
            lines += ["add w7, w9, w10", "mov w20, w7"]
        else:
            lines += ["mov w7, w9", "add w20, w9, w10"]
        return (lines, True)
    lines += ["ubfx x10, x19, #32, #8", "ldr w10, [_vcpu, x10]"]
    operation = "add" if mode[1] == "a" else "sub"
    if mode in ("ra", "rs"):
        lines.append(f"{operation} w7, w9, w10")
        return (lines, False)
    if mode.endswith("pre"):
        lines += [f"{operation} w7, w9, w10", "mov w20, w7"]
    else:
        lines += ["mov w7, w9", f"{operation} w20, w9, w10"]
    return (lines, True)


def double_writeback() -> list[str]:
    return ["ubfx x9, x19, #16, #8", "str w20, [_vcpu, x9]"]


def load_double(name: str, mode: str) -> str:
    address, writes_back = double_address_lines(mode)
    lines = ["ldr x19, [_stream], #8"] + address
    lines += ["tst w7, #3", "b.ne Lalign_" + name]
    lines.append(
        f"a32_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, 8, x7,"
        f" Lcold_{name}, Lresume_{name}, Lcross_{name}, 0,"
        f" fault=La32_position_fault")
    lines += ["ldp w0, w15, [x7]", "and x8, x19, #0xff",
              "ubfx x9, x19, #8, #8", "str w0, [_vcpu, x8]",
              "str w15, [_vcpu, x9]"]
    if writes_back:
        lines += double_writeback()
    lines.append("next_gadget")
    lines.append(
        f"a32_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, 8, x7,"
        f" Lcold_{name}, Lresume_{name}, Lcross_{name}, 0,"
        f" fault=La32_position_fault")
    lines.append(f"Lcross_{name}:")
    lines += ["transfer_slow 0, x7, 8, fault=La32_position_fault",
              "umov x15, v0.d[0]", "and x8, x19, #0xff",
              "ubfx x9, x19, #8, #8", "str w15, [_vcpu, x8]",
              "lsr x15, x15, #32", "str w15, [_vcpu, x9]"]
    if writes_back:
        lines += double_writeback()
    lines.append("next_gadget")
    lines += [f"Lalign_{name}:", "b La32_position_alignment_fault_read"]
    return "\n".join(lines)


def store_double(name: str, mode: str) -> str:
    address, writes_back = double_address_lines(mode)
    lines = ["ldr x19, [_stream], #8"] + address
    lines += ["tst w7, #3", "b.ne Lalign_" + name]
    lines.append(
        f"a32_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, 8, x7,"
        f" Lcold_{name}, Lresume_{name}, Lcross_{name}, 0,"
        f" fault=La32_position_fault")
    lines += ["and x8, x19, #0xff", "ubfx x9, x19, #8, #8",
              "ldr w0, [_vcpu, x8]", "ldr w15, [_vcpu, x9]",
              "stp w0, w15, [x7]"]
    if writes_back:
        lines += double_writeback()
    lines.append("next_gadget")
    lines.append(
        f"a32_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, 8, x7,"
        f" Lcold_{name}, Lresume_{name}, Lcross_{name}, 0,"
        f" fault=La32_position_fault")
    lines.append(f"Lcross_{name}:")
    lines += ["and x8, x19, #0xff", "ubfx x9, x19, #8, #8",
              "ldr w0, [_vcpu, x8]", "ldr w15, [_vcpu, x9]",
              "bfi x0, x15, #32, #32", "fmov d0, x0",
              "transfer_slow 1, x7, 8, fault=La32_position_fault"]
    if writes_back:
        lines += double_writeback()
    lines.append("next_gadget")
    lines += [f"Lalign_{name}:", "b La32_position_alignment_fault_write"]
    return "\n".join(lines)


def emit_double(asm: Assembly) -> None:
    for mode in DOUBLE_MODES:
        name = f"ldrd_{mode}"
        asm.gadget(name, load_double(name, mode))
    for mode in DOUBLE_MODES[:-1]:
        name = f"strd_{mode}"
        asm.gadget(name, store_double(name, mode))


# LDM and STM, for each count of registers. The stream is
#   [gadget][word0][registers seven to sixteen, if any][library operand]
# A register is its CpuState offset over four (scaled back by the access):
# word0: Rn [0:5], writeback [5], the start address's displacement from Rn
# (signed) [8:16], the written-back Rn's displacement (signed) [16:24], the
# instruction's position (a32_layout.h) [24:32], the first six registers
# [32:62] five bits each; the next word holds the rest five bits each. The
# registers are in ascending order, which is the order of ascending
# addresses. The start address must be word aligned; a transfer within one
# page runs from the translated host address, and one that crosses a page is
# the semantic library's, run as the whole instruction through the operand
# the stream ends with (a32_library_call) and the position in place of the
# key, since no transfer here reads the PC.
BLOCK_WORD_REGISTERS = 6


def block_rest_words(count: int) -> int:
    return 1 if count > BLOCK_WORD_REGISTERS else 0


def block_registers(count: int, access_load: bool, base: str) -> list[str]:
    """The transfers of `count` registers between the host address in `base`
    and CpuState; registers past the sixth are in the word at [_stream]."""
    lines = []
    if block_rest_words(count):
        lines.append("ldr x13, [_stream]")
    for index in range(count):
        if index < BLOCK_WORD_REGISTERS:
            source, low = "x19", 32 + 5 * index
        else:
            source, low = "x13", 5 * (index - BLOCK_WORD_REGISTERS)
        lines.append(f"ubfx x17, {source}, #{low}, #5")
        if access_load:
            lines += [f"ldr w0, [{base}, #{4 * index}]",
                      "str w0, [_vcpu, x17, lsl #2]"]
        else:
            lines += ["ldr w0, [_vcpu, x17, lsl #2]",
                      f"str w0, [{base}, #{4 * index}]"]
    return lines


def block_prologue(label: str) -> list[str]:
    return ["ldr x19, [_stream], #8",
            "ubfx x9, x19, #0, #5", "ldr w9, [_vcpu, x9, lsl #2]",
            "sbfx x10, x19, #8, #8", "add w7, w9, w10",
            "sbfx x11, x19, #16, #8", "add w22, w9, w11",
            "tst w7, #3", "b.ne Lalign_" + label]


def block_epilogue(count: int) -> list[str]:
    # The register word, if any, then the library operand the crossing path
    # runs the whole instruction with.
    words = block_rest_words(count) + 1
    return [f"add _stream, _stream, #{8 * words}",
            "tbz x19, #5, 1f", "ubfx x9, x19, #0, #5",
            "str w22, [_vcpu, x9, lsl #2]", "1:", "next_gadget"]


def block_gadget(count: int, access_load: bool) -> str:
    size = 4 * count
    name = f"{'ldm' if access_load else 'stm'}_{count}"
    page = "UML_GADGET_TLB_ENTRY_READ_PAGE" if access_load else \
        "UML_GADGET_TLB_ENTRY_WRITE_PAGE"
    direction = 0 if access_load else 1
    lines = block_prologue(name)
    lines.append(f"a32_translate {direction}, {page}, {size}, x7,"
                 f" Lcold_{name}, Lresume_{name}, Lcross_{name}, 0,"
                 f" fault=La32_position_fault")
    lines += block_registers(count, access_load, "x7")
    lines += block_epilogue(count)
    lines.append(f"a32_translate_cold {direction}, {page}, {size}, x7,"
                 f" Lcold_{name}, Lresume_{name}, Lcross_{name}, 0,"
                 f" fault=La32_position_fault")
    # A transfer that crosses a page is the semantic library's: it
    # translates every page before it changes a register or, for a load,
    # any state, and an STM stores the words ahead of the one that faults.
    # Nothing has changed by here, so the library runs the whole
    # instruction.
    lines.append(f"Lcross_{name}:")
    if block_rest_words(count):
        lines.append("add _stream, _stream, #8")
    lines += ["ldr x8, [_stream], #8", "a32_position_pc",
              "a32_library_call", "next_gadget"]
    lines += [f"Lalign_{name}:",
              "b La32_position_alignment_fault_" +
              ("read" if access_load else "write")]
    return "\n".join(lines)


def emit_blocks(asm: Assembly) -> None:
    for count in range(1, 17):
        asm.gadget(f"ldm_{count}", block_gadget(count, True))
        asm.gadget(f"stm_{count}", block_gadget(count, False))


# ---------------------------------------------------------------------------
# Multiplies, division, bit manipulation and the other short integer forms

Q_UPDATE = """
        ldrb w14, [_vcpu, #UML_ARM64_CPU_A32_Q]
        orr w14, w14, {flag}
        strb w14, [_vcpu, #UML_ARM64_CPU_A32_Q]
"""


def load_regs(*fields: tuple[str, str, int]) -> list[str]:
    """Load CpuState registers named by byte fields of x8: (host register,
    base offset in the word, ...) -> ubfx/ldr pairs."""
    lines = []
    for register, _, low in fields:
        lines += [f"ubfx x{register[1:]}, x8, #{low}, #8",
                  f"ldr {register}, [_vcpu, x{register[1:]}]"]
    return lines


def mul_gadget(kind: str, s: bool) -> str:
    """MUL, MLA, MLS: rd[0:8] rn[8:16] rm[16:24] ra[24:32]."""
    lines = ["ldr x8, [_stream], #8"]
    lines += load_regs(("w9", "", 8), ("w10", "", 16))
    if kind == "mul":
        lines.append("mul w11, w9, w10")
    else:
        lines += load_regs(("w12", "", 24))
        lines.append("madd w11, w9, w10, w12" if kind == "mla"
                     else "msub w11, w9, w10, w12")
    if s:
        lines.append(nz_update("w11"))
    lines += ["and x8, x8, #0xff", "str w11, [_vcpu, x8]", "next_gadget"]
    return "\n".join(lines)


def mull_gadget(kind: str, s: bool) -> str:
    """UMULL SMULL UMLAL SMLAL UMAAL: lo[0:8] hi[8:16] rn[16:24] rm[24:32]."""
    lines = ["ldr x8, [_stream], #8"]
    lines += load_regs(("w9", "", 16), ("w10", "", 24))
    lines.append("smull x11, w9, w10" if kind in ("smull", "smlal")
                 else "umull x11, w9, w10")
    if kind in ("umlal", "smlal", "umaal"):
        lines += ["ubfx x12, x8, #0, #8", "ldr w12, [_vcpu, x12]",
                  "ubfx x13, x8, #8, #8", "ldr w13, [_vcpu, x13]"]
        if kind == "umaal":
            lines += ["add x11, x11, x12", "add x11, x11, x13"]
        else:
            lines += ["orr x12, x12, x13, lsl #32", "add x11, x11, x12"]
    if s:
        lines.append("tst x11, x11")
        lines += ["mrs x14, nzcv", "and w14, w14, #0xc0000000",
                  "and w27, w27, #0x30000000", "orr w27, w27, w14"]
    lines += ["ubfx x12, x8, #0, #8", "ubfx x13, x8, #8, #8",
              "str w11, [_vcpu, x12]", "lsr x11, x11, #32",
              "str w11, [_vcpu, x13]", "next_gadget"]
    return "\n".join(lines)


def mulh_gadget(index: int) -> str:
    """SMMUL SMMULR SMMLA SMMLAR SMMLS SMMLSR: rd[0:8] rn[8:16] rm[16:24]
    ra[24:32]."""
    accumulate = index >= 2
    subtract = index >= 4
    rounding = index % 2 == 1
    lines = ["ldr x8, [_stream], #8"]
    lines += load_regs(("w9", "", 8), ("w10", "", 16))
    lines.append("smull x11, w9, w10")
    if accumulate:
        lines += load_regs(("w12", "", 24))
        lines.append("lsl x12, x12, #32")
        lines.append("sub x11, x12, x11" if subtract
                     else "add x11, x12, x11")
    if rounding:
        lines += ["mov x13, #0x80000000", "add x11, x11, x13"]
    lines += ["lsr x11, x11, #32", "and x8, x8, #0xff",
              "str w11, [_vcpu, x8]", "next_gadget"]
    return "\n".join(lines)


def mulxy_gadget(index: int) -> str:
    """The halfword multiplies; names in MULXY. rd[0:8] rn[8:16] rm[16:24]
    ra[24:32]; SMLALxy: lo[0:8] hi[8:16] rn[16:24] rm[24:32]."""
    names = list(MULXY)
    mnemonic = names[index]
    long_form = mnemonic.startswith("SMLAL")
    word = mnemonic[len("SMLAL" if long_form else
                        "SMUL" if mnemonic.startswith("SMUL") else "SMLA"):]
    lines = ["ldr x8, [_stream], #8"]
    n_field, m_field = (16, 24) if long_form else (8, 16)
    lines += load_regs(("w9", "", n_field), ("w10", "", m_field))
    if mnemonic.startswith(("SMULW", "SMLAW")):
        top = mnemonic.endswith("T")
        lines.append("asr w10, w10, #16" if top else "sxth w10, w10")
        lines += ["smull x11, w9, w10", "asr x11, x11, #16"]
    else:
        n_top, m_top = word[0] == "T", word[1] == "T"
        lines.append("asr w9, w9, #16" if n_top else "sxth w9, w9")
        lines.append("asr w10, w10, #16" if m_top else "sxth w10, w10")
        lines.append("smull x11, w9, w10")
    if long_form:
        lines += ["ubfx x12, x8, #0, #8", "ldr w12, [_vcpu, x12]",
                  "ubfx x13, x8, #8, #8", "ldr w13, [_vcpu, x13]",
                  "orr x12, x12, x13, lsl #32", "add x11, x11, x12",
                  "ubfx x12, x8, #0, #8", "ubfx x13, x8, #8, #8",
                  "str w11, [_vcpu, x12]", "lsr x11, x11, #32",
                  "str w11, [_vcpu, x13]", "next_gadget"]
        return "\n".join(lines)
    if mnemonic.startswith(("SMLA", "SMLAW")):
        lines += load_regs(("w12", "", 24))
        lines += ["adds w11, w11, w12", "cset w13, vs"]
        lines.append(Q_UPDATE.format(flag="w13"))
    lines += ["and x8, x8, #0xff", "str w11, [_vcpu, x8]", "next_gadget"]
    return "\n".join(lines)


def div_gadget(signed: bool) -> str:
    """SDIV UDIV: rd[0:8] rn[8:16] rm[16:24]."""
    lines = ["ldr x8, [_stream], #8"]
    lines += load_regs(("w9", "", 8), ("w10", "", 16))
    lines.append("sdiv w11, w9, w10" if signed else "udiv w11, w9, w10")
    lines += ["and x8, x8, #0xff", "str w11, [_vcpu, x8]", "next_gadget"]
    return "\n".join(lines)


EXTENDS = {"sxtb": "sxtb w10, w10", "sxth": "sxth w10, w10",
           "uxtb": "uxtb w10, w10", "uxth": "uxth w10, w10"}


def extend_gadget(kind: str, accumulate: bool) -> str:
    """SXTB SXTH UXTB UXTH and the accumulating forms: rd[0:8] rn[8:16]
    rm[16:24] rotation in bits[24:32]."""
    lines = ["ldr x8, [_stream], #8"]
    lines += load_regs(("w10", "", 16))
    lines += ["ubfx x11, x8, #24, #8", "ror w10, w10, w11", EXTENDS[kind]]
    if accumulate:
        lines += load_regs(("w9", "", 8))
        lines.append("add w10, w9, w10")
    lines += ["and x8, x8, #0xff", "str w10, [_vcpu, x8]", "next_gadget"]
    return "\n".join(lines)


BIT_OPS = {"rev": ["rev w11, w10"], "rev16": ["rev16 w11, w10"],
           "revsh": ["rev16 w11, w10", "sxth w11, w11"],
           "rbit": ["rbit w11, w10"], "clz": ["clz w11, w10"]}


def bits_gadget(kind: str) -> str:
    """REV REV16 REVSH RBIT CLZ: rd[0:8] rm[16:24]."""
    lines = ["ldr x8, [_stream], #8"]
    lines += load_regs(("w10", "", 16))
    lines += BIT_OPS[kind]
    lines += ["and x8, x8, #0xff", "str w11, [_vcpu, x8]", "next_gadget"]
    return "\n".join(lines)


def field_gadget(kind: str) -> str:
    """UBFX: rd[0:8] rn[8:16] lsb[16:24], mask in bits[32:64].
    SBFX: rd[0:8] rn[8:16] left shift[16:24] right shift[24:32].
    BFI: rd[0:8] rn[8:16] lsb[16:24], mask << lsb in bits[32:64]. BFC: the
    same without rn."""
    lines = ["ldr x8, [_stream], #8"]
    if kind == "ubfx":
        lines += load_regs(("w9", "", 8))
        lines += ["ubfx x11, x8, #16, #8", "lsr w9, w9, w11",
                  "lsr x12, x8, #32", "and w11, w9, w12"]
    elif kind == "sbfx":
        lines += load_regs(("w9", "", 8))
        lines += ["ubfx x11, x8, #16, #8", "lsl w9, w9, w11",
                  "ubfx x11, x8, #24, #8", "asr w11, w9, w11"]
    elif kind == "bfi":
        lines += load_regs(("w9", "", 8))
        lines += ["ubfx x11, x8, #16, #8", "lsl w9, w9, w11",
                  "lsr x12, x8, #32", "and w9, w9, w12",
                  "and x13, x8, #0xff", "ldr w11, [_vcpu, x13]",
                  "bic w11, w11, w12", "orr w11, w11, w9"]
    else:
        lines += ["lsr x12, x8, #32", "and x13, x8, #0xff",
                  "ldr w11, [_vcpu, x13]", "bic w11, w11, w12"]
    lines += ["and x8, x8, #0xff", "str w11, [_vcpu, x8]", "next_gadget"]
    return "\n".join(lines)


def sat_gadget(unsigned: bool, shift: str) -> str:
    """SSAT USAT: rd[0:8] rn[8:16] amount[16:22]; then the second word holds
    the maximum in [0:32] and the minimum in [32:64]."""
    lines = ["ldp x8, x13, [_stream], #16"]
    lines += load_regs(("w9", "", 8))
    lines.append("ubfx x14, x8, #16, #6")
    if shift == "asr":
        lines += ["sxtw x9, w9", "asr x9, x9, x14"]
    else:
        lines.append("lsl w9, w9, w14")
    lines += ["lsr x15, x13, #32", "cmp w9, w13", "csel w11, w13, w9, gt",
              "cmp w11, w15", "csel w11, w15, w11, lt", "cmp w11, w9",
              "cset w12, ne", Q_UPDATE.format(flag="w12"),
              "and x8, x8, #0xff", "str w11, [_vcpu, x8]", "next_gadget"]
    return "\n".join(lines)


def pkh_gadget(top_bottom: bool) -> str:
    """PKHBT PKHTB: rd[0:8] rn[8:16] rm[16:24] amount[24:30]."""
    lines = ["ldr x8, [_stream], #8"]
    lines += load_regs(("w9", "", 8), ("w10", "", 16))
    lines.append("ubfx x14, x8, #24, #6")
    if not top_bottom:
        lines += ["lsl w10, w10, w14", "and w10, w10, #0xffff0000",
                  "and w9, w9, #0xffff"]
    else:
        lines += ["sxtw x10, w10", "asr x10, x10, x14",
                  "and w10, w10, #0xffff", "and w9, w9, #0xffff0000"]
    lines += ["orr w11, w9, w10", "and x8, x8, #0xff",
              "str w11, [_vcpu, x8]", "next_gadget"]
    return "\n".join(lines)


def emit_integer(asm: Assembly) -> None:
    for kind in ("mul", "mla", "mls"):
        for s in (False, True):
            if kind == "mls" and s:
                continue
            asm.gadget(f"{kind}{'s' if s else ''}", mul_gadget(kind, s))
    for kind in ("umull", "smull", "umlal", "smlal"):
        for s in (False, True):
            asm.gadget(f"{kind}{'s' if s else ''}", mull_gadget(kind, s))
    asm.gadget("umaal", mull_gadget("umaal", False))
    for index, name in enumerate(("smmul", "smmulr", "smmla", "smmlar",
                                  "smmls", "smmlsr")):
        asm.gadget(name, mulh_gadget(index))
    for index, name in enumerate(MULXY):
        asm.gadget(name.lower(), mulxy_gadget(index))
    asm.gadget("sdiv", div_gadget(True))
    asm.gadget("udiv", div_gadget(False))
    for kind in EXTENDS:
        asm.gadget(kind, extend_gadget(kind, False))
        asm.gadget(kind + "a", extend_gadget(kind, True))
    for kind in BIT_OPS:
        asm.gadget(kind, bits_gadget(kind))
    for kind in ("ubfx", "sbfx", "bfi", "bfc"):
        asm.gadget(kind, field_gadget(kind))
    for unsigned in (False, True):
        for shift in ("lsl", "asr"):
            asm.gadget(f"{'usat' if unsigned else 'ssat'}_{shift}",
                       sat_gadget(unsigned, shift))
    asm.gadget("pkhbt", pkh_gadget(False))
    asm.gadget("pkhtb", pkh_gadget(True))
    asm.gadget("movt", """
        ldr x8, [_stream], #8
        and x9, x8, #0xff
        ldr w10, [_vcpu, x9]
        lsr x11, x8, #48
        bfi w10, w11, #16, #16
        str w10, [_vcpu, x9]
        next_gadget
    """)


# ---------------------------------------------------------------------------
# The outputs

DP_SHAPES = ["imm", "imc", "reg", "lsl", "lsr", "asr", "ror", "rrx", "rlsl",
             "rlsr", "rasr", "rror"]
LOAD_ORDER = ["w", "b", "h", "sb", "sh"]
STORE_ORDER = ["w", "b", "h"]
SINGLE_MODES = ADDRESS_MODES + ["abs"]


# The conditions in encoding order, and the run lengths in words that a skip
# gadget holds as its distance (a32.inc's a32_skip).
CONDITIONS = ("eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge",
              "lt", "gt", "le")
SKIP_WORDS = range(2, 10)

# The gadgets a32.inc defines through macros, which its text does not name.
MACRO_GADGETS = (
    [name for cc in CONDITIONS
     for name in [f"skip_{cc}"] + [f"skip_{cc}_{n}" for n in SKIP_WORDS]] +
    ["cbz_inline", "cbz_linked", "cbz_plain", "cbnz_inline", "cbnz_linked",
     "cbnz_plain",
     "bx_cached", "bx_return_cached", "bx", "bw_cached", "bw_return_cached",
     "bw", "tbb", "tbh", "cp15_dsb", "cp15_dmb", "cp15_isb"])


def hand_written(engine_dir: Path) -> list[str]:
    names = []
    for line in (engine_dir / "a32.inc").read_text().splitlines():
        match = re.match(r"^gadget a32_(\w+)\s*$", line)
        if match:
            names.append(match.group(1))
    return names + MACRO_GADGETS


def build_assembly(asm: Assembly) -> None:
    emit_dp(asm)
    emit_dp_inplace(asm)
    emit_single(asm)
    emit_double(asm)
    emit_blocks(asm)
    emit_integer(asm)
    a32_simd.emit(asm)
    a32_armv8.emit(asm)
    a32_parallel.emit(asm)
    emit_select(asm)
    a32_structure.emit_fused(asm)
    a32_structure.emit_hinted(asm)
    a32_simd.emit_compare_apsr(asm)
    a32_simd.emit_move_select(asm)


def write_assembly(path: Path, asm: Assembly, hand: list[str]) -> None:
    out = ["/* SPDX-License-Identifier: GPL-2.0-or-later */",
           "/* Generated by engine/generate_a32.py from the AArch32 decoder",
           " * table. Do not edit. */", "",
           "/* The widening half-precision multiply-adds (FEAT_FHM) are not in",
           " * the architecture level the rest of the engine assembles at. */",
           ".arch_extension fp16fml", ""]
    out += asm.text
    out += [".pushsection __DATA_CONST,__gadget_a32", ".p2align 3",
            ".globl CSYM(uml_arm64_a32_gadgets)",
            "CSYM(uml_arm64_a32_gadgets):"]
    for name in hand + asm.names:
        out.append(f"\t.quad CSYM(uml_arm64_gadget_a32_{name})")
    out += [".popsection", ""]
    path.write_text("\n".join(out))


def write_header(path: Path, asm: Assembly, hand: list[str],
                 forms: list[FormRow]) -> None:
    names = hand + asm.names
    index = {name: i for i, name in enumerate(names)}
    absent = "G::count"

    def g(name: str) -> str:
        return f"G::{name}" if name in index else absent

    out = ["// SPDX-License-Identifier: GPL-2.0-or-later",
           "// Generated by engine/generate_a32.py from the AArch32 decoder",
           "// table. Do not edit.",
           "#pragma once", "",
           '#include "cpu/arm64/aarch32/a32_encodings.hpp"', "",
           "#include <cstddef>", "#include <cstdint>", "",
           "namespace uml::cpu::arm64::gadgets::a32 {", ""]
    out.append("/* Every gadget of the AArch32 part, in the order of the table "
               "engine.S")
    out.append(" * defines (uml_arm64_a32_gadgets). */")
    out.append("enum class G : std::uint16_t {")
    for name in names:
        out.append(f"\t{name},")
    out += ["\tcount,", "};", "",
            'extern "C" const std::uintptr_t uml_arm64_a32_gadgets[];', "",
            "[[nodiscard]] inline std::uintptr_t gadget(G which) noexcept",
            "{",
            "\treturn uml_arm64_a32_gadgets[static_cast<std::size_t>(which)];",
            "}", ""]
    out.append("enum class Family : std::uint8_t {")
    for family in FAMILIES:
        out.append(f"\t{family},")
    out += ["};", "",
            "/* What the builder lowers a form as: the family, an operation",
            " * within it and a family-specific aspect (generate_a32.py",
            " * plan_of). */",
            "struct FormPlan final {", "\tFamily family;",
            "\tstd::uint8_t sub;", "\tstd::uint8_t aux;", "};", ""]
    out.append("inline constexpr FormPlan kFormPlans["
               "aarch32::kFormCount] = {")
    unplanned = []
    for row in forms:
        family, sub, aux = plan_of(row)
        if family == "none" and row.availability == "implemented" and \
                row.name not in ("Undefined",):
            unplanned.append(row.name)
        out.append(f"\t{{Family::{family}, {sub}, {aux}}},  // {row.name}")
    out += ["};", ""]
    if unplanned:
        raise GenerationError(
            "implemented forms without a lowering: " + ", ".join(unplanned))
    # Operation tables.
    out.append("/* Data processing: [operation][S][shape]; the operations "
               "and shapes are")
    out.append(" * DP_ORDER and DP_SHAPES. */")
    out.append("inline constexpr G kDp[17][2][12] = {")
    for op in DP_ORDER:
        out.append("\t{")
        for s in (False, True):
            entries = []
            for shape in DP_SHAPES:
                name = f"dp_{op}{'s' if s and op not in COMPARE else ''}_{shape}"
                if op in COMPARE and not s:
                    entries.append(absent)
                else:
                    entries.append(g(name))
            out.append("\t\t{" + ", ".join(entries) + "},")
        out.append("\t},")
    out += ["};", ""]
    out.append("/* In-place ADD and SUB of an immediate with the register "
               "baked: [subtract][S][register]. */")
    out.append(f"inline constexpr G kDpInplace[2][2][{INPLACE_REGISTERS}] = {{")
    for subtract in (False, True):
        out.append("\t{")
        for s in (False, True):
            out.append("\t\t{" + ", ".join(
                g(inplace_name(subtract, s, r))
                for r in range(INPLACE_REGISTERS)) + "},")
        out.append("\t},")
    out += ["};", ""]
    out.append("/* Conditional data processing as a select: [operation][shape];"
               " the operations")
    out.append(" * are DP_ORDER's, the shapes imm and reg (generate_a32.py "
               "SELECT_OPS). */")
    out.append("inline constexpr G kDpSelect[17][2] = {")
    for op in DP_ORDER:
        out.append("\t{" + ", ".join(
            g(select_name(op, shape)) if op in SELECT_OPS else absent
            for shape in SELECT_SHAPES) + "},")
    out += ["};", ""]
    out.append("/* Single transfers: [size][mode]; modes are i ipre ipost ra "
               "rs rapre rspre rapost rspost abs. */")
    out.append("inline constexpr G kLoad[5][10] = {")
    for size in LOAD_ORDER:
        out.append("\t{" + ", ".join(
            g(f"ld_{size}_{m}") for m in SINGLE_MODES) + "},")
    out += ["};", "inline constexpr G kStore[3][10] = {"]
    for size in STORE_ORDER:
        out.append("\t{" + ", ".join(
            g(f"st_{size}_{m}") for m in SINGLE_MODES) + "},")
    out += ["};", "inline constexpr G kLdrd[10] = {" + ", ".join(
        g(f"ldrd_{m}") for m in DOUBLE_MODES) + "};",
        "inline constexpr G kStrd[10] = {" + ", ".join(
            g(f"strd_{m}") for m in DOUBLE_MODES) + "};",
        "inline constexpr G kLdm[17] = {G::count, " + ", ".join(
            f"G::ldm_{c}" for c in range(1, 17)) + "};",
        "inline constexpr G kStm[17] = {G::count, " + ", ".join(
            f"G::stm_{c}" for c in range(1, 17)) + "};", ""]
    out.append("/* The skips whose distance is in the gadget: [condition]"
               "[words - kSkipWordsMin]. */")
    for bound, words in (("Min", SKIP_WORDS[0]), ("Max", SKIP_WORDS[-1])):
        out.append(f"inline constexpr std::size_t kSkipWords{bound} = {words};")
    out.append(f"inline constexpr G kSkipBy[14][{len(SKIP_WORDS)}] = {{")
    for cc in CONDITIONS:
        row = ", ".join(g(f"skip_{cc}_{n}") for n in SKIP_WORDS)
        out.append("\t{" + row + "},")
    out += ["};", ""]
    out += a32_simd.header_tables(index)
    out.append("")
    out += a32_armv8.header_tables(index)
    out.append("")
    out += a32_parallel.header_tables(index)
    out += ["", "} // namespace uml::cpu::arm64::gadgets::a32", ""]
    path.write_text("\n".join(out))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--table", type=Path, required=True)
    parser.add_argument("--assembly", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        _, forms = load_forms(arguments.table)
        asm = Assembly()
        build_assembly(asm)
        hand = hand_written(Path(__file__).resolve().parent)
        for name in hand:
            if name in asm.names:
                raise GenerationError("generated gadget shadows a32.inc's "
                                      + name)
        arguments.assembly.parent.mkdir(parents=True, exist_ok=True)
        write_assembly(arguments.assembly, asm, hand)
        write_header(arguments.header, asm, hand, forms)
    except GenerationError as error:
        print("generate_a32.py:", error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
