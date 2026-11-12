# A64 instruction coverage

The A64 catalog (Base, FP/SIMD, SVE and SME) is owned by the checked TSV
files under `tests/unit/arm64/`, not by this document; they hold every exact
list and count. Every catalog entry has representative cases in the
[instruction corpus](../tests/unit/arm64/corpus/README.md), which runs them
through the gadget and JIT engines, and the corpus check fails if an entry has
none. Base and FP/SIMD cases passed the interpreter and gadget lanes (and the
JIT lane where the export host implements the feature) before export; SVE and
SME cases are generated from the encoding table below, with native results
recorded on an SME host.

## Status

An entry is `verified` when the virtual CPU implements it and the corpus pins
it, and `absent` when it needs a feature the virtual CPU omits: MTE, GCS, CPA,
PAuth, the later memory, SIMD&FP and branch or wait extensions, FEAT_SVE
itself (the SME profile lacks it), and SVE or SME extensions outside the
profile. Their
non-hint encodings are UNDEFINED and their hint-space forms (the PAC and AUT
hints, GCSB) are NOPs; the corpus cases expect exactly that. An entry stays
`verified` when only one form needs an omitted feature (LDAPR and STLR
writeback, DSB nXS, MSR (immediate) SSBS).

`verified` is not exhaustive variant proof: no entry has an independently
reviewed partition of all forms, widths, aliases, architectural state,
optional features, reserved encodings, fault ordering and operand
combinations, so none is variant-complete.

## What the guest sees

A guest learns the CPU from the identification registers an EL0 MRS reads and
from AT_HWCAP and AT_HWCAP2. Each register field is set on the host predicate
the engines' native path uses, and both hwcap words are derived from those
fields by `guest_hwcaps()` in
[`darwin/cpu/arm64/id_registers.hpp`](../darwin/cpu/arm64/id_registers.hpp), so
the answers always agree. `arm64-advertised-features` pins the words, the
registers and the `/proc/cpuinfo` Features line.

- FEAT_AFP (with FEAT_RPRES), FEAT_ECV and FEAT_JSCVT are advertised exactly
  where the host implements them (`implements_afp()` in
  [`darwin/cpu/arm64/floating_controls.hpp`](../darwin/cpu/arm64/floating_controls.hpp)).
  Elsewhere FPCR.FIZ, AH and NEP are RES0: MSR FPCR and every context restore
  clear them, and floating-point instructions behave as if all three were
  zero. Len, Stride and EBF always read as zero.
- CNTVCTSS_EL0 is read with the host instruction where FEAT_ECV exists and is
  UNDEFINED otherwise; CNTPCT_EL0 and CNTPCTSS_EL0 are UNDEFINED everywhere,
  because Linux gives EL0 no physical count.
- FEAT_CSSC and FEAT_MOPS are implemented on every host (the host instruction,
  or short base sequences), so their hwcaps are always set. MOPS is option B:
  the prologue formats the registers and the epilogue moves every byte;
  the JIT runs page-contained transfers inline.

The corpus pins what it can (`arm64-base-system-registers` also pins the FPCR
write mask for a CPU without FEAT_AFP on every host); the guest HWCAP sanity
payload checks counter values and the host-dependent controls on each host.

## SVE and SME encodings

One generated table decides the SVE and SME encodings for both engines:
[`darwin/cpu/arm64/detail/sme_encodings.hpp`](../darwin/cpu/arm64/detail/sme_encodings.hpp),
which `generate_sme_encoding_table.py` derives from the pinned Arm release and
LLVM MC. Each row records its requirements in and outside streaming mode, the
smallest streaming vector length at which it is allocated, the PSTATE.SM and
PSTATE.ZA state its access checks demand, its operands and its memory shape. A
companion table classifies each FP&SIMD encoding as AdvSIMD (traps in
streaming mode) or FP, and `sme_decode.hpp` decodes against both.
`arm64-sme-encoding-table` checks the decoder, each row's LLVM MC witness and
each row's requirements against the table.

The table keeps the architecture's requirements and the virtual CPU implements
no SVE feature, so a row whose outside-streaming requirement names one is
UNDEFINED outside streaming mode. The exception is the SM = 0 family (CNT*,
INC, DEC, RDVL, ADDVL, ADDPL, and LDR and STR of Z and P registers), which a
virtual CPU with SME and without SVE executes for a vector length of 16 bytes,
a deliberate departure from the architecture that
[`architecture.md`](architecture.md) describes.

An SVE or SME entry is `verified` when one of its encodings needs only the SME
profile, and so are SMSTART and SMSTOP. Where the virtual CPU lacks SME (a host
without the profile, or the guest-SME policy forced off) the corpus pins
SMSTART, SMSTOP, the SVCR accesses and one word of every profile encoding
UNDEFINED. The `arm64-sme-native` records hold the results an SME host gives
at its own streaming vector length and run where an engine implements SME at
that length. MOVT between ZT0 and a general register executes only in Debug
state, so it has no table row and is UNDEFINED in every mode.

## Sources of truth

- `a64_instruction_inventory.tsv` owns the stable catalog identity and status;
  the `arm64-instruction-inventory` tooling test validates it.
- `a64_instruction_allocations.tsv` and `a64_instruction_decode_partitions.tsv`
  map the pinned Arm A-profile release and LLVM MC allocation space; the
  corpus builder attributes every case to inventory entries through them.
- `a64_instruction_evidence_bindings.tsv` and
  `a64_instruction_variant_contracts.tsv` supply one witness encoding per
  allocation and independently reviewed encoding contracts; both feed the
  allocation generator, and the corpus check enforces their case names.
- `darwin/cpu/arm64/detail/sme_encodings.hpp` maps the same release to the SVE
  and SME decode; the opt-in `arm64-sme-encoding-table-freshness` tooling test
  regenerates it.

The generators live under `tests/tooling/arm64`; use their `--help` output for
the pinned regeneration inputs.
