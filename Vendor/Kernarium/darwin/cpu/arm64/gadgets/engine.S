/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Keep one translation unit so shared macros and local labels remain cheap. */

/* The gadget region: the bank, the sections its gadgets push forwarding
 * variants, cold paths and FEAT_AFP twins into, the island of branches to the
 * helpers outside it, and the SME part, in that order. Every image that
 * carries the engine links UML::GadgetRegionLayout, which merges these
 * sections into __TEXT,__text and puts them first, so the region sits at one
 * image offset whatever else the image holds. The bank starts on a 16 KiB
 * host page and every later part on a 64-byte line plus its phase, so the
 * region's bytes and offsets depend on this file alone; docs/architecture.md
 * has the contract.
 *
 * The twins follow every base part: on a host with FEAT_AFP they replace base
 * entries on the hot path and need the same fixed placement, and elsewhere
 * they never run, so where they sit moves no base entry. The island follows
 * them because its words alone encode distances to code outside the region.
 *
 * The SME part runs only where the virtual CPU implements SME, so where it
 * sits moves no base entry either. It follows the island rather than
 * preceding it: every helper call in the base parts is a branch to its veneer
 * in another section, so a part between them and the island would re-encode
 * each one. Its own helper calls take veneers appended to the island's end.
 * The AArch32 part, A32 and T32 translated by the shared decoder, follows
 * the SME part for the same reason and has a bank, a cold section and an
 * island of its own: it may not add a veneer to the first island, which
 * would move the SME part, so a helper it calls first is reached through a
 * veneer at the end of its own, and its code reaches the base part's veneers
 * for the helpers the base parts already call. It holds no SVE or SME
 * instruction, and so runs outside streaming mode only.
 * The names are the arm64 engine's own, so the x86-64 engine's sections never
 * interleave with them in an image that carries both. */
#define GADGET_SECTION_BANK __TEXT,__a64_bank,regular,pure_instructions
#define GADGET_SECTION_FORWARD __TEXT,__a64_forward,regular,pure_instructions
#define GADGET_SECTION_VECTOR_FORWARD __TEXT,__a64_vforward,regular,pure_instructions
#define GADGET_SECTION_COLD __TEXT,__a64_cold,regular,pure_instructions
#define GADGET_SECTION_AFP __TEXT,__a64_afp,regular,pure_instructions
#define GADGET_SECTION_VENEER __TEXT,__a64_veneer,regular,pure_instructions
#define GADGET_SECTION_SME __TEXT,__a64_sme,regular,pure_instructions
#define GADGET_SECTION_A32_BANK __TEXT,__a32_bank,regular,pure_instructions
#define GADGET_SECTION_A32_COLD __TEXT,__a32_cold,regular,pure_instructions
#define GADGET_SECTION_A32_VENEER __TEXT,__a32_veneer,regular,pure_instructions

/* Unreachable bytes ahead of each part's first entry, which set the part's
 * 64-byte phase. The bank's phase is the one the iPad measured fastest with
 * the region pinned: at 16, scalar_mix ran in its slow mode in most launches.
 * The other parts keep the phases their sections had in the shipped
 * KRuntime_6_18_16K at 49a43648, where each began wherever the code before
 * it ended. No host with FEAT_AFP has measured the twins' phase, and no host
 * with SME the SME part's. */
#define GADGET_PHASE_BANK 48
#define GADGET_PHASE_FORWARD 16
#define GADGET_PHASE_VECTOR_FORWARD 48
#define GADGET_PHASE_COLD 4
#define GADGET_PHASE_AFP 0
#define GADGET_PHASE_SME 0
#define GADGET_PHASE_A32_BANK 0
#define GADGET_PHASE_A32_COLD 0
#define GADGET_PHASE_A32_VENEER 0

/* Zero-size bounds of each part, for the guest sampler's PC test. CSYM() is
 * not defined until core_integer.inc. */
.macro gadget_bank_bound name
	.globl _\name
_\name:
.endm

/* The start of one part: its boundary, a private label at offset 0 that the
 * order file names, and the phase pad. */
.macro gadget_region_start part, alignment, phase
	.p2align \alignment
	.private_extern _uml_arm64_gadget_region_\part
_uml_arm64_gadget_region_\part:
	.space \phase
.endm

.macro gadget_bank_bounds edge
	.section GADGET_SECTION_BANK
	.ifc \edge,begin
	gadget_region_start bank, 14, GADGET_PHASE_BANK
	.endif
	gadget_bank_bound uml_arm64_gadget_bank_\edge
	.pushsection GADGET_SECTION_FORWARD
	.ifc \edge,begin
	gadget_region_start forward, 6, GADGET_PHASE_FORWARD
	.endif
	gadget_bank_bound uml_arm64_gadget_forward_section_\edge
	.popsection
	.pushsection GADGET_SECTION_VECTOR_FORWARD
	.ifc \edge,begin
	gadget_region_start vector_forward, 6, GADGET_PHASE_VECTOR_FORWARD
	.endif
	gadget_bank_bound uml_arm64_gadget_vector_forward_section_\edge
	.popsection
	.pushsection GADGET_SECTION_COLD
	.ifc \edge,begin
	gadget_region_start cold, 6, GADGET_PHASE_COLD
	.endif
	gadget_bank_bound uml_arm64_gadget_cold_section_\edge
	.popsection
	.pushsection GADGET_SECTION_AFP
	.ifc \edge,begin
	gadget_region_start afp, 6, GADGET_PHASE_AFP
	.endif
	gadget_bank_bound uml_arm64_gadget_afp_section_\edge
	.popsection
	.pushsection GADGET_SECTION_VENEER
	.ifc \edge,begin
	gadget_region_start veneer, 4, 0
	.endif
	gadget_bank_bound uml_arm64_gadget_veneer_section_\edge
	.popsection
	.pushsection GADGET_SECTION_SME
	.ifc \edge,begin
	gadget_region_start sme, 6, GADGET_PHASE_SME
	.endif
	gadget_bank_bound uml_arm64_gadget_sme_section_\edge
	.popsection
	.pushsection GADGET_SECTION_A32_BANK
	.ifc \edge,begin
	gadget_region_start a32_bank, 6, GADGET_PHASE_A32_BANK
	.endif
	gadget_bank_bound uml_arm64_gadget_a32_bank_section_\edge
	.popsection
	.pushsection GADGET_SECTION_A32_COLD
	.ifc \edge,begin
	gadget_region_start a32_cold, 6, GADGET_PHASE_A32_COLD
	.endif
	gadget_bank_bound uml_arm64_gadget_a32_cold_section_\edge
	.popsection
	.pushsection GADGET_SECTION_A32_VENEER
	.ifc \edge,begin
	gadget_region_start a32_veneer, 4, GADGET_PHASE_A32_VENEER
	.endif
	gadget_bank_bound uml_arm64_gadget_a32_veneer_section_\edge
	.popsection
.endm

/* Nonzero while engine/sme.inc assembles the GPR memory families a second
 * time, as their streaming twins; every base part is assembled with it 0. */
.set Lsme_twin, 0

/* A call from the region to a helper outside it branches to the helper's
 * veneer, one branch in the island defined at its first use, so no other word
 * of the region encodes a distance to code that moves with the rest of the
 * image. A streaming twin calls the helper through sme_helper_call instead,
 * which leaves streaming mode around the call; `cpu` is where the caller
 * saved CpuState, as an offset from SP. */
.macro helper_call symbol, cpu=0
	.ifndef Lveneer_\symbol
	.pushsection GADGET_SECTION_VENEER
Lveneer_\symbol:
	b _\symbol
	.popsection
	.endif
	.if Lsme_twin
	sme_helper_call \symbol, \cpu
	.else
	bl Lveneer_\symbol
	.endif
.endm

	gadget_bank_bounds begin
#include "engine/core_integer.inc"
#include "engine/afp_twin.inc"
#include "engine/floating_simd.inc"
#include "engine/system_atomic.inc"
#include "engine/load_store.inc"
#include "engine/control.inc"
/* Families accepted after the bank's layout was measured are appended, so
 * adding one did not move or rephase any established gadget. */
#include "engine/experimental.inc"
#include "engine/inplace_immediate.inc"
#include "engine/inplace_extended.inc"
#include "engine/inplace_memory.inc"
#include "engine/constant_store.inc"
#include "engine/test_bit_branch.inc"
#include "engine/bitfield_shift.inc"
#include "engine/scalar_forwarding.inc"
#include "engine/vector_forwarding.inc"
#include "engine/execution_count.inc"
/* The cold FEAT_LSE2 alignment stubs of the atomic families go last, in reach
 * of their conditional branches, so no established gadget moves for them. */
#include "engine/lse2_alignment.inc"
/* RET through X30 appends its entry after them: neither the bank nor the cold
 * part moves for it. */
#include "engine/return_x30.inc"
/* Only into the SME part, after every base part's code. */
#include "engine/sme.inc"
/* Only into the AArch32 part's sections, after every part before it. */
#include "engine/a32.inc"
	afp_twin_table_end
	gadget_bank_bounds end
