# A32 instruction coverage

The virtual CPU runs AArch32 EL0 programs of Armv8-A: ARMv7-A and Armv8
A32 and T32 (Thumb-2, with IT blocks) with VFPv4-D32, Advanced SIMD and
the Armv8 additions, for the hard-float EABI of armhf and the softfp EABI of
Android's armeabi-v7a, which both execute VFP and NEON on hardware. There is
no soft-float-only, VFPv3-D16 or ARMv5/v6 profile.
The pinned Arm AArch32 release (2026-06) defines the catalog; its 302 Base
and 265 FP&SIMD instruction pages hold 2,363 encodings. 143 of them are
aliases, preferred disassemblies of words that another encoding decodes,
so the decoder's table and the inventory carry the other 2,220.

`tests/unit/arm64/a32_instruction_inventory.tsv` lists every one with its
instruction set, level, optional feature, profile and status.
`validate_a32_instruction_inventory.py` (the `arm64-a32-instruction-inventory`
tooling test) checks it against the generated decoder table, and checks that
the next two sentences state its counts.

`tests/unit/arm64/a32_instruction_inventory.tsv` lists 2,220 encodings (912 Base and 1,308 FP&SIMD): 38 `pending`, 2,127 `verified` and 55 `absent`.
By level there are 898 `base`, 948 `simd` and 374 `complete` encodings.

## Levels and features

The inventory and the decoder's table classify every encoding by the part of
the profile it belongs to, and a guest sees all three: `base` is the integer
core with Thumb-2 and the Armv8 load-acquire and store-release group (LDA,
STL, LDAEX, STLEX in every size, and SEVL), which every Armv8 AArch32 EL0
has and which arm64 Linux's compat kuser helpers execute; `simd` is
VFPv4-D32 and Advanced SIMD, and `complete` is the other Armv8 additions
(VSEL, VMAXNM, VMINNM, VRINT*, VCVT[ANPM], the double/half conversions) and
the optional extensions.

The optional extensions are FEAT_FP16, FEAT_AES, FEAT_PMULL, FEAT_SHA1,
FEAT_SHA256, FEAT_CRC32, FEAT_DotProd, FEAT_FHM, FEAT_AA32BF16,
FEAT_AA32I8MM, FEAT_SB, FEAT_RDM, FEAT_FCMA and FEAT_JSCVT. An engine
implements each only where the host has the A64 feature it lowers onto, and
its advertisement says the same; an encoding of an extension the profile
lacks is UNDEFINED. The profile never has FEAT_RAS, FEAT_TRF, FEAT_CLRBHB or
FEAT_PAN, so ESB, TSB CSYNC and CLRBHB execute as the NOPs they replace.

## What EL0 gets

Every UNPREDICTABLE encoding, including a should-be-zero or should-be-one
bit that differs, is UNDEFINED: the decoder never guesses. The profile
leaves out, by `absent` entries whose corpus cases expect an illegal
instruction:

- SWP and SWPB, which Armv8 AArch32 does not have (they are absent from the
  release and decode as unallocated, so an engine raises the event whatever the
  condition and the guest kernel emulates them behind `abi.swp`), and SETEND
  and SETPAN;
- the privileged and debug forms: ERET, RFE, SRS, SMC, HVC, HLT, DCPS, the
  banked-register and user-register forms of MRS, MSR, LDM and STM;
- LDC and STC, and MCRR, because no coprocessor register is accessible by
  those forms at EL0; and
- the Thumb exception return SUBS PC, LR, which is UNPREDICTABLE at EL0.

The `restricted` entries decode, and their fields select what an EL0
program may touch. MRC, MCR and MRRC reach only TPIDRURO and TPIDRURW,
CNTFRQ, CNTVCT (the virtual count) and the CP15 barrier operations, the last
only while the guest's CP15BEN is set, which is its default. MRS and MSR
reach the APSR only, and VMRS and VMSR reach FPSCR only; an MSR whose x field
would write PSTATE.E is undefined. CPS is a NOP and
BXJ behaves as BX. FPSCR's exception trap enables, Len and Stride read as
zero and ignore writes.

## Status

An entry is `absent` when no word of it executes, `pending` until an engine
passes the corpus for it, and `verified` after. The gadget engine passes the
corpus for every executed entry (`arm64-gadget-a32-corpus --forms` lists the
encodings whose cases ran), except the 38 BF16 and I8MM entries: the engine decodes an
extension only where the host has it, so on a host without them (the M1 Ultra)
those cases are skipped, and the entries stay `pending`. The decoder's
interface is
[`../darwin/cpu/arm64/aarch32/decode.hpp`](../darwin/cpu/arm64/aarch32/decode.hpp),
and
[`../darwin/cpu/arm64/aarch32/README.md`](../darwin/cpu/arm64/aarch32/README.md)
holds its profile choices;
`generate_a32_encoding_table.py` derives the inventory, the decoder's table
and its test data from the release, and `a32_profile_policy.py` is the one
place that holds the profile's choices. The status is the one column the
release cannot give: a regeneration keeps each encoding's `pending` or
`verified` from the checked-in inventory, so a lane marks an encoding
`verified` by editing that file and the freshness test still passes.
