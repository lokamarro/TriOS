/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The arm64 guest gadgets. Runtime blocks contain only pointers to these
 * precompiled routines and inline integer operands; NOTICE.md beside the
 * engine credits the design that inspired this arrangement.
 */
#include "../../layout/assembly_offsets.h"
#include "../../../gadget_host_frame.inc"

/* Apple Silicon exposes the Armv8 CRC and cryptographic extensions. Keep the
 * assembler aware of them so extension-backed gadgets can use native ops. */
.arch armv8.6-a+crc+crypto+sha3+rcpc+i8mm+fp16

#define CSYM(symbol) _##symbol

_vcpu .req x1
_swtlb .req x2
_stream .req x28

.macro gadget name
	.globl CSYM(uml_arm64_gadget_\name)
	.p2align 4
CSYM(uml_arm64_gadget_\name):
.endm

#include "scalar_contract.inc"
#include "vector_contract.inc"

.macro next_gadget
	ldr x8, [_stream], #8
	br x8
.endm

/* Native scalar FP gadgets share a host floating-point environment for the
 * whole threaded block. The first saves the caller's state and selects the
 * guest FPCR; later gadgets notice guest FPCR writes. The execution boundary
 * restores the host state without leaking guest exception flags. */
.macro ensure_fp_environment
	/* Production retains the native guest environment across internal block
	 * exits. FPCR writes update it eagerly, so the steady path is one load
	 * and one predicted branch. Keep the cold system-register setup out of
	 * every generated gadget to minimize code footprint. */
	ldrb w6, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbnz w6, 99f
	bl uml_arm64_gadget_activate_native_fp_environment
99:
.endm

/* Call uml_arm64_\helper(CpuState, x8) inside the active guest FP
 * environment, for a helper that models an instruction in software and
 * reports its exceptions by ORing them into CpuState.fpsr. The live FPSR is
 * the host register, so CpuState's copy can lack cumulative flags that
 * earlier native gadgets of the block raised: materialize it before the call
 * and reinstall it after. The host register holds only the writable FPSR
 * fields, so storing it commits what the execution boundary would. The guest
 * sees exactly the prior FPSR plus the modelled exceptions, and any status
 * the host raises while running the helper is discarded. x1/x2 survive in
 * x19/x20. A helper whose exceptions come from host instructions run under
 * the guest FPCR uses fpsr_merge_helper instead.
 */
.macro fpsr_model_helper helper
	mrs x16, fpsr
	str w16, [_vcpu, #UML_GADGET_CPU_FPSR]
	mov x19, x1
	mov x20, x2
	mov x0, x1
	mov x1, x8
	helper_call uml_arm64_\helper
	mov x1, x19
	mov x2, x20
	ldr w17, [_vcpu, #UML_GADGET_CPU_FPSR]
	msr fpsr, x17
.endm

/* Call uml_arm64_\helper(CpuState, x8), keeping x1/x2 on the stack. */
.macro software_helper helper
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x8
	helper_call uml_arm64_\helper
	ldp x1, x2, [sp], #16
.endm

/* software_helper for a helper whose exceptions come from host instructions
 * run under the guest FPCR: the live FPSR, which holds the block's cumulative
 * status and what those instructions raised, is ORed into CpuState.fpsr, and
 * the result becomes the live FPSR. */
.macro fpsr_merge_helper helper
	software_helper \helper
	mrs x17, fpsr
	ldr w16, [_vcpu, #UML_GADGET_CPU_FPSR]
	orr w16, w16, w17
	str w16, [_vcpu, #UML_GADGET_CPU_FPSR]
	msr fpsr, x16
.endm

/* Translate guest address x7 through the persistent software TLB of 16,384
 * direct-mapped entries. On success x7 is the host address. The miss helper
 * updates the entry.
 */
.macro translate access, page_offset, forward=0, fault=uml_arm64_gadget_memory_fault
	/* Entries are keyed by guest page number (address >> page shift). */
	lsr x8, x7, #UML_GADGET_GUEST_PAGE_SHIFT
	ubfx x9, x7, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	add x10, _swtlb, x9, lsl #UML_GADGET_TLB_SET_SHIFT
	/* Each access-specific tag is paired with its backing delta, so the
	 * successful way needs one cache-line-local load pair and no later load. */
	ldp x11, x12, [x10, #\page_offset]
	cmp x8, x11
	b.eq 91f
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x7
	mov w2, #\access
	helper_call uml_arm64_tlb_miss
	ldp x1, x2, [sp], #16
	.if \forward || Lsme_twin
	/* Cross-section conditional relocations are not available on Mach-O.
	 * Only the cold miss path needs this long-branch trampoline. */
	cbnz x0, 94f
	b \fault
94:
	.else
	cbz x0, \fault
	.endif
	mov x7, x0
	b 92f
91:
	add x7, x12, x7
92:
.endm

/* Strip a top-byte-ignore tag from guest address \reg where TBI is on, as the
 * scalar transfers do, so the atomic, ordered, RCpc and exclusive forms key
 * the TLB, check alignment and report a fault by the same untagged address.
 * Scratch: w9. */
.macro tbi_strip reg
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, Ltbi_strip_done\@
	ubfx \reg, \reg, #0, #56
Ltbi_strip_done\@:
.endm

/* A translated transfer is at most 64 bytes, so adding its final byte can
 * cross at most one guest-page boundary.  The xor therefore needs only the
 * page-index low bit, avoiding a separate mask/compare instruction. */
.macro branch_if_cross_page address, bytes, scratch, target
	add \scratch, \address, #(\bytes - 1)
	eor \scratch, \scratch, \address
	tbnz \scratch, #UML_GADGET_GUEST_PAGE_SHIFT, \target
.endm

/* Hot-path translation of a same-page transfer of \bytes bytes at \address,
 * which becomes the host address on the fall-through path. The pair
 * transfer_translate / transfer_translate_cold replaces branch_if_cross_page
 * plus translate; the cold half is placed after the gadget's dispatch and
 * resumes at \resume. One-byte transfers cannot cross and are never checked.
 *
 * The lookup is keyed by the page of the transfer's final byte while indexed
 * by its first: pages n and n + 1 never share a direct-mapped set, so a
 * crossing transfer can only miss and the cold path is where it is
 * recognized. The key is shifted before a plain compare: a shifted-operand
 * compare resolved the branch late enough to cost two percent on
 * transfer-bound loops. Scratch: x8-x12.
 */
.macro transfer_translate access, page_offset, bytes, address, cold, resume, cross, forward=0, fault=uml_arm64_gadget_memory_fault
	.if \bytes > 1
	add x8, \address, #(\bytes - 1)
	.endif
	ubfx x9, \address, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	add x10, _swtlb, x9, lsl #UML_GADGET_TLB_SET_SHIFT
	ldp x11, x12, [x10, #\page_offset]
	.if \bytes > 1
	lsr x8, x8, #UML_GADGET_GUEST_PAGE_SHIFT
	.else
	lsr x8, \address, #UML_GADGET_GUEST_PAGE_SHIFT
	.endif
	cmp x11, x8
	b.ne \cold
	add \address, x12, \address
\resume:
.endm

/* Crossing, miss and fault handling for transfer_translate. */
/* outofline=1 when the caller places this tail in a section of its own: a
 * conditional branch cannot leave its section, so the fault exit takes the
 * same trampoline the forwarding arm already uses. So does a streaming twin,
 * whose default fault exit is in the bank. */
.macro transfer_translate_cold access, page_offset, bytes, address, cold, resume, cross, forward=0, outofline=0, fault=uml_arm64_gadget_memory_fault
\cold:
	.if \bytes > 1
	branch_if_cross_page \address, \bytes, x9, \cross
	.endif
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, \address
	mov w2, #\access
	helper_call uml_arm64_tlb_miss
	ldp x1, x2, [sp], #16
	.if \forward || \outofline || Lsme_twin
	cbnz x0, 94f
	b \fault
94:
	.else
	cbz x0, \fault
	.endif
	mov \address, x0
	b \resume
.endm

/* Rare cross-page accesses stay on the gadget path. The helper validates all
 * pages before stores, so a fault cannot leave a partially modified range.
 * q0/q1 carry up to 32 bytes to or from the helper. A streaming twin clears
 * them with FMOV, which streaming mode keeps, where the base gadget uses
 * MOVI, which it does not.
 */
.macro transfer_slow access, address, size, fault=uml_arm64_gadget_memory_fault
	sub sp, sp, #48
	stp x1, x2, [sp, #32]
	.if \access
	stp q0, q1, [sp]
	.else
	.if Lsme_twin
	fmov d0, xzr
	fmov d1, xzr
	.else
	movi v0.16b, #0
	movi v1.16b, #0
	.endif
	stp q0, q1, [sp]
	.endif
	mov x0, x2
	mov x1, \address
	mov x2, sp
	mov x3, #\size
	mov w4, #\access
	helper_call uml_arm64_memory_transfer, 32
	ldp x1, x2, [sp, #32]
	cbz w0, 97f
	.if !\access
	ldp q0, q1, [sp]
	.endif
	add sp, sp, #48
	b 98f
97:
	add sp, sp, #48
	b \fault
98:
.endm

.section GADGET_SECTION_BANK
.p2align 4
uml_arm64_gadget_activate_native_fp_environment:
	mrs x6, fpcr
	mrs x5, fpsr
	str x6, [_vcpu, #UML_GADGET_CPU_HOST_FPCR]
	str x5, [_vcpu, #UML_GADGET_CPU_HOST_FPSR]
	mov w6, #1
	strb w6, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]
	ldr w6, [_vcpu, #UML_GADGET_CPU_FPCR]
	mov w5, #UML_ARM64_FPCR_TRAP_ENABLES
	bic w6, w6, w5
	msr fpcr, x6
	ldr w6, [_vcpu, #UML_GADGET_CPU_FPSR]
	msr fpsr, x6
	ret

/* The entry frame's own slots, after the registers gadget_host_frame.inc
 * saves. A build that counts executed instructions keeps the caller's counter
 * pointer in one, which execution_count reads. */
#if UML_GADGET_EXECUTION_HISTOGRAM
#define UML_GADGET_FRAME_HISTOGRAM GADGET_HOST_FRAME_SAVED
#define UML_GADGET_FRAME_SLOTS 16
#else
#define UML_GADGET_FRAME_SLOTS 0
#endif

/* Runs a data-only stream until a gadget leaves the engine.
 *   in:  x0 stream, x1 CpuState, x2 TLB, x3 instructions already retired
 *        (x4 the counter table, in a histogram build)
 *   out: w0 the event, x1 the instructions retired
 * Inside, x28 walks the stream, x29 holds the retirement count and x27 the
 * guest's flags; the frame keeps the caller's copies of all three. The result
 * pair is returned in registers, so the hot boundary stores nothing. */
.globl CSYM(uml_arm64_gadget_enter)
.p2align 4
CSYM(uml_arm64_gadget_enter):
	gadget_host_frame_push UML_GADGET_FRAME_SLOTS
#if UML_GADGET_EXECUTION_HISTOGRAM
	str x4, [sp, #UML_GADGET_FRAME_HISTOGRAM]
#endif
	mov _stream, x0
	/* Bias the retirement accumulator by the linked-chain limit. This lets
	 * follow_block_link test its sign bit in one instruction while preserving
	 * the exact architectural count at the execution boundary. */
	sub x29, x3, #1, lsl #12
	/* Keep the architectural flags in a callee-saved host register. Gadget
	 * scratch comparisons may clobber native PSTATE, so consumers still issue
	 * MSR NZCV, but no per-instruction CpuState load/store is required. */
	ldr w27, [_vcpu, #UML_GADGET_CPU_NZCV]
	next_gadget

.p2align 4
uml_arm64_gadget_leave:
	str w27, [_vcpu, #UML_GADGET_CPU_NZCV]
	add x1, x29, #1, lsl #12
	gadget_host_frame_pop UML_GADGET_FRAME_SLOTS
	ret

/* MOVZ/MOVN are folded to a full constant by the translator.  Rd==XZR is
 * folded to NOP, so the live entry is a three-load straight-line store. */
scalar_entry move_wide_constant, 0, 0, Lscalar_offset0, Lscalar_none, Lscalar_none, Lscalar_none, 2
	ldr x8, [_stream], #8
	ldr _scalar_x11, [_stream], #8
	add x13, _vcpu, #UML_GADGET_CPU_X0
	str _scalar_x11, [x13, x8]
	next_gadget

/* The ADD, SUB, ADDS or SUBS of \rn and \rm into \rd that \subtract and \flags
 * select. `add` and `sub` name the mnemonics, so passing adc and sbc selects
 * ADC, SBC, ADCS or SBCS. inplace_extended.inc, the last user, purges it. */
.macro add_sub_op subtract, flags, rd, rn, rm, add=add, sub=sub
	.if \subtract
		.if \flags
	\sub\()s \rd, \rn, \rm
		.else
	\sub \rd, \rn, \rm
		.endif
	.else
		.if \flags
	\add\()s \rd, \rn, \rm
		.else
	\add \rd, \rn, \rm
		.endif
	.endif
.endm

/* Production ADD/SUB-immediate entries.  Width, operation, flag update and
 * both architectural register-31 meanings are fixed by the translator.  The
 * compact operand is Rd[4:0], Rn[12:8], immediate[39:16].  These variants
 * deliberately contain no opcode tests or conditional branches. */
.macro add_sub_immediate_fast bits64, subtract, flags, source_sp, destination_special, suffix, forward=0
	.set Lsd\@, Lscalar_register0
	.if \destination_special && !\flags
	.set Lsd\@, Lscalar_sp
	.endif
	.if \destination_special && \flags
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_register8
	.if \source_sp
	.set Lsn\@, Lscalar_sp
	.endif
scalar_entry add_sub_immediate_\suffix, \forward, 1, Lsd\@, Lsn\@, Lscalar_none, Lscalar_none, 1
	/* This family is non-faulting and leaves x14 free. Builder always emits a
	 * terminal after a translated fallthrough, so the successor word exists
	 * even when this is the final guest instruction in the block. */
	ldp x8, x14, [_stream], #16
	.if \source_sp
	scalar_load_sp \forward, 1, x15
	.else
	scalar_load \forward, 1, x15, 10, 8, 8, 5, 3, _vcpu
	.endif
	lsr x11, x8, #16
	.if \bits64
	add_sub_op \subtract, \flags, _scalar_x16, x15, x11
	.else
	add_sub_op \subtract, \flags, _scalar_w16, w15, w11
	.endif
	.if \flags
	mrs x27, nzcv
	.endif
	.if \destination_special
		.if !\flags
	str _scalar_x16, [_vcpu, #UML_GADGET_CPU_SP]
		.endif
	.else
	and w9, w8, #0x1f
	str _scalar_x16, [_vcpu, x9, lsl #3]
	.endif
	br x14
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, add_sub_immediate_fast, \bits64, \subtract, \flags, \source_sp, \destination_special, \suffix
.endm

.macro add_sub_immediate_modes bits64, subtract, flags, prefix
add_sub_immediate_fast \bits64, \subtract, \flags, 0, 0, \prefix\()_rr
add_sub_immediate_fast \bits64, \subtract, \flags, 0, 1, \prefix\()_rs
add_sub_immediate_fast \bits64, \subtract, \flags, 1, 0, \prefix\()_sr
add_sub_immediate_fast \bits64, \subtract, \flags, 1, 1, \prefix\()_ss
.endm

add_sub_immediate_modes 0, 0, 0, w_add_n
add_sub_immediate_modes 0, 0, 1, w_add_s
add_sub_immediate_modes 0, 1, 0, w_sub_n
add_sub_immediate_modes 0, 1, 1, w_sub_s
add_sub_immediate_modes 1, 0, 0, x_add_n
add_sub_immediate_modes 1, 0, 1, x_add_s
add_sub_immediate_modes 1, 1, 0, x_sub_n
add_sub_immediate_modes 1, 1, 1, x_sub_s

.purgem add_sub_immediate_modes
.purgem add_sub_immediate_fast

/* Production ADD/SUB shifted/extended register entries. CpuState byte offsets
 * for ordinary registers/SP and the shift amount are prepacked by Builder.
 * Width, arithmetic, flags, transform, XZR inputs, and discard/SP destination
 * behavior are all entry-point constants. */
.macro add_sub_register_finish bits64, subtract, flags, destination_discard
	.if \bits64
	add_sub_op \subtract, \flags, _scalar_x7, x15, x16
	.else
	add_sub_op \subtract, \flags, _scalar_w7, w15, w16
	.endif
	.if \flags
	mrs x27, nzcv
	.endif
	.if !\destination_discard
	and x9, x8, #0xffff
	str _scalar_x7, [_vcpu, x9]
	.endif
	next_gadget
.endm

.macro add_sub_register_shifted_fast bits64, subtract, flags, shift_kind, left_zero, right_zero, destination_discard, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.if \destination_discard
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_offset16
	.if \left_zero
	.set Lsn\@, Lscalar_none
	.endif
	.set Lsm\@, Lscalar_offset32
	.if \right_zero
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry add_sub_register_\suffix, \forward, 2, Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _vcpu
	.endif
	.if \right_zero
	mov x16, xzr
	.else
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _vcpu
	.endif
	.if \shift_kind != 0
	ubfx x6, x8, #48, #6
		.if \bits64
			.if \shift_kind == 1
	lsl x16, x16, x6
			.elseif \shift_kind == 2
	lsr x16, x16, x6
			.else
	asr x16, x16, x6
			.endif
		.else
			.if \shift_kind == 1
	lsl w16, w16, w6
			.elseif \shift_kind == 2
	lsr w16, w16, w6
			.else
	asr w16, w16, w6
			.endif
		.endif
	.endif
	add_sub_register_finish \bits64, \subtract, \flags, \destination_discard
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, add_sub_register_shifted_fast, \bits64, \subtract, \flags, \shift_kind, \left_zero, \right_zero, \destination_discard, \suffix
.endm

.macro add_sub_register_extended_fast bits64, subtract, flags, extension, shift_nonzero, right_zero, destination_discard, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.if \destination_discard
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_offset16
	.set Lsm\@, Lscalar_offset32
	.if \right_zero
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry add_sub_register_\suffix, \forward, 3, Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _vcpu
	.if \right_zero
	mov x16, xzr
	.else
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _vcpu
	.endif
	.if \extension == 0
	and x16, x16, #0xff
	.elseif \extension == 1
	and x16, x16, #0xffff
	.elseif \extension == 2
	mov w16, w16
	.elseif \extension == 4
	sxtb x16, w16
	.elseif \extension == 5
	sxth x16, w16
	.elseif \extension == 6
	sxtw x16, w16
	.endif
	.if \shift_nonzero
	ubfx x6, x8, #48, #3
	lsl x16, x16, x6
	.endif
	add_sub_register_finish \bits64, \subtract, \flags, \destination_discard
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, add_sub_register_extended_fast, \bits64, \subtract, \flags, \extension, \shift_nonzero, \right_zero, \destination_discard, \suffix
.endm

.macro add_sub_shifted_destinations bits64, subtract, flags, shift_kind, left_zero, right_zero, prefix
add_sub_register_shifted_fast \bits64, \subtract, \flags, \shift_kind, \left_zero, \right_zero, 0, \prefix\()_r
add_sub_register_shifted_fast \bits64, \subtract, \flags, \shift_kind, \left_zero, \right_zero, 1, \prefix\()_d
.endm
.macro add_sub_shifted_sources bits64, subtract, flags, shift_kind, prefix
add_sub_shifted_destinations \bits64, \subtract, \flags, \shift_kind, 0, 0, \prefix\()_rr
add_sub_shifted_destinations \bits64, \subtract, \flags, \shift_kind, 0, 1, \prefix\()_rz
add_sub_shifted_destinations \bits64, \subtract, \flags, \shift_kind, 1, 0, \prefix\()_zr
add_sub_shifted_destinations \bits64, \subtract, \flags, \shift_kind, 1, 1, \prefix\()_zz
.endm
.macro add_sub_shifted_transforms bits64, subtract, flags, prefix
add_sub_shifted_sources \bits64, \subtract, \flags, 0, \prefix\()_n
add_sub_shifted_sources \bits64, \subtract, \flags, 1, \prefix\()_l
add_sub_shifted_sources \bits64, \subtract, \flags, 2, \prefix\()_r
add_sub_shifted_sources \bits64, \subtract, \flags, 3, \prefix\()_a
.endm

.macro add_sub_extended_destinations bits64, subtract, flags, extension, shift_nonzero, right_zero, prefix
add_sub_register_extended_fast \bits64, \subtract, \flags, \extension, \shift_nonzero, \right_zero, 0, \prefix\()_r
add_sub_register_extended_fast \bits64, \subtract, \flags, \extension, \shift_nonzero, \right_zero, 1, \prefix\()_d
.endm
.macro add_sub_extended_sources bits64, subtract, flags, extension, shift_nonzero, prefix
add_sub_extended_destinations \bits64, \subtract, \flags, \extension, \shift_nonzero, 0, \prefix\()_r
add_sub_extended_destinations \bits64, \subtract, \flags, \extension, \shift_nonzero, 1, \prefix\()_z
.endm
.macro add_sub_extended_transforms bits64, subtract, flags, prefix
add_sub_extended_sources \bits64, \subtract, \flags, 0, 0, \prefix\()_e0n
add_sub_extended_sources \bits64, \subtract, \flags, 0, 1, \prefix\()_e0s
add_sub_extended_sources \bits64, \subtract, \flags, 1, 0, \prefix\()_e1n
add_sub_extended_sources \bits64, \subtract, \flags, 1, 1, \prefix\()_e1s
add_sub_extended_sources \bits64, \subtract, \flags, 2, 0, \prefix\()_e2n
add_sub_extended_sources \bits64, \subtract, \flags, 2, 1, \prefix\()_e2s
add_sub_extended_sources \bits64, \subtract, \flags, 3, 0, \prefix\()_e3n
add_sub_extended_sources \bits64, \subtract, \flags, 3, 1, \prefix\()_e3s
add_sub_extended_sources \bits64, \subtract, \flags, 4, 0, \prefix\()_e4n
add_sub_extended_sources \bits64, \subtract, \flags, 4, 1, \prefix\()_e4s
add_sub_extended_sources \bits64, \subtract, \flags, 5, 0, \prefix\()_e5n
add_sub_extended_sources \bits64, \subtract, \flags, 5, 1, \prefix\()_e5s
add_sub_extended_sources \bits64, \subtract, \flags, 6, 0, \prefix\()_e6n
add_sub_extended_sources \bits64, \subtract, \flags, 6, 1, \prefix\()_e6s
add_sub_extended_sources \bits64, \subtract, \flags, 7, 0, \prefix\()_e7n
add_sub_extended_sources \bits64, \subtract, \flags, 7, 1, \prefix\()_e7s
.endm

.macro add_sub_register_all bits64, subtract, flags, prefix
add_sub_shifted_transforms \bits64, \subtract, \flags, \prefix\()_sh
add_sub_extended_transforms \bits64, \subtract, \flags, \prefix\()_ex
.endm

add_sub_register_all 0, 0, 0, w_add_n
add_sub_register_all 0, 0, 1, w_add_s
add_sub_register_all 0, 1, 0, w_sub_n
add_sub_register_all 0, 1, 1, w_sub_s
add_sub_register_all 1, 0, 0, x_add_n
add_sub_register_all 1, 0, 1, x_add_s
add_sub_register_all 1, 1, 0, x_sub_n
add_sub_register_all 1, 1, 1, x_sub_s

.purgem add_sub_register_all
.purgem add_sub_extended_transforms
.purgem add_sub_extended_sources
.purgem add_sub_extended_destinations
.purgem add_sub_shifted_transforms
.purgem add_sub_shifted_sources
.purgem add_sub_shifted_destinations
.purgem add_sub_register_extended_fast
.purgem add_sub_register_shifted_fast
.purgem add_sub_register_finish

/* Production ADC/SBC entries. All instruction-invariant choices, including
 * XZR, equal sources, and a discarded destination, are translation-selected.
 * Stream operand: destination byte offset[15:0], left byte offset[31:16],
 * right byte offset[47:32]. */
.macro add_sub_carry_fast bits64, subtract, flags, left_zero, right_zero, same_source, destination_zero, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.if \destination_zero
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_offset16
	.if \left_zero
	.set Lsn\@, Lscalar_none
	.endif
	.set Lsm\@, Lscalar_offset32
	.if \right_zero || \same_source
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry add_sub_carry_\suffix, \forward, 4, Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _vcpu
	.endif
	.if \right_zero
	mov x16, xzr
	.elseif \same_source
	mov x16, x15
	.else
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _vcpu
	.endif
	msr nzcv, x27
	.if \bits64
	add_sub_op \subtract, \flags, _scalar_x7, x15, x16, adc, sbc
	.else
	add_sub_op \subtract, \flags, _scalar_w7, w15, w16, adc, sbc
	.endif
	.if \flags
	mrs x27, nzcv
	.endif
	.if !\destination_zero
	uxth w9, w8
	str _scalar_x7, [_vcpu, x9]
	.endif
	next_gadget
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, add_sub_carry_fast, \bits64, \subtract, \flags, \left_zero, \right_zero, \same_source, \destination_zero, \suffix
.endm

.macro add_sub_carry_destinations bits64, subtract, flags, left_zero, right_zero, same_source, prefix
add_sub_carry_fast \bits64, \subtract, \flags, \left_zero, \right_zero, \same_source, 0, \prefix\()_r
	.if \flags
add_sub_carry_fast \bits64, \subtract, \flags, \left_zero, \right_zero, \same_source, 1, \prefix\()_z
	.endif
.endm

.macro add_sub_carry_source_modes bits64, subtract, flags, prefix
add_sub_carry_destinations \bits64, \subtract, \flags, 0, 0, 0, \prefix\()_rr
add_sub_carry_destinations \bits64, \subtract, \flags, 0, 0, 1, \prefix\()_eq
add_sub_carry_destinations \bits64, \subtract, \flags, 0, 1, 0, \prefix\()_rz
add_sub_carry_destinations \bits64, \subtract, \flags, 1, 0, 0, \prefix\()_zr
add_sub_carry_destinations \bits64, \subtract, \flags, 1, 1, 0, \prefix\()_zz
.endm

add_sub_carry_source_modes 0, 0, 0, w_adc_n
add_sub_carry_source_modes 0, 0, 1, w_adc_s
add_sub_carry_source_modes 0, 1, 0, w_sbc_n
add_sub_carry_source_modes 0, 1, 1, w_sbc_s
add_sub_carry_source_modes 1, 0, 0, x_adc_n
add_sub_carry_source_modes 1, 0, 1, x_adc_s
add_sub_carry_source_modes 1, 1, 0, x_sbc_n
add_sub_carry_source_modes 1, 1, 1, x_sbc_s

.purgem add_sub_carry_source_modes
.purgem add_sub_carry_destinations
.purgem add_sub_carry_fast

/* Production multiply family. The validated operation and every XZR choice
 * are translation-selected; discarded results are removed by Builder. Stream:
 * destination, left, right, addend CpuState byte offsets in 16-bit fields. */
.macro multiply_add_fast operation, left_zero, right_zero, addend_zero, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.set Lsn\@, Lscalar_offset16
	.if \left_zero || \right_zero
	.set Lsn\@, Lscalar_none
	.endif
	.set Lsm\@, Lscalar_offset32
	.if \left_zero || \right_zero
	.set Lsm\@, Lscalar_none
	.endif
	.set Lsa\@, Lscalar_offset48
	.if \operation >= 8 || \addend_zero
	.set Lsa\@, Lscalar_none
	.endif
scalar_entry multiply_add_\suffix, \forward, 5, Lsd\@, Lsn\@, Lsm\@, Lsa\@, 1
	ldr x8, [_stream], #8
	.if \left_zero || \right_zero
		/* A zero multiplicand makes the product zero. */
		.if \operation >= 8 || \addend_zero
	mov _scalar_x7, xzr
		.else
	scalar_load \forward, 4, _scalar_x7, 12, 8, 48, 16, 0, _vcpu
			/* W-form writes zero-extend the copied addend. */
			.if \operation == 0 || \operation == 1
	mov _scalar_w7, _scalar_w7
			.endif
		.endif
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _vcpu
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _vcpu
		.if \operation < 8 && !\addend_zero
	scalar_load \forward, 4, x17, 12, 8, 48, 16, 0, _vcpu
		.endif
		.if \operation == 0
			.if \addend_zero
	mul _scalar_w7, w15, w16
			.else
	madd _scalar_w7, w15, w16, w17
			.endif
		.elseif \operation == 1
			.if \addend_zero
	mneg _scalar_w7, w15, w16
			.else
	msub _scalar_w7, w15, w16, w17
			.endif
		.elseif \operation == 2
			.if \addend_zero
	mul _scalar_x7, x15, x16
			.else
	madd _scalar_x7, x15, x16, x17
			.endif
		.elseif \operation == 3
			.if \addend_zero
	mneg _scalar_x7, x15, x16
			.else
	msub _scalar_x7, x15, x16, x17
			.endif
		.elseif \operation == 4
			.if \addend_zero
	smull _scalar_x7, w15, w16
			.else
	smaddl _scalar_x7, w15, w16, x17
			.endif
		.elseif \operation == 5
			.if \addend_zero
	smnegl _scalar_x7, w15, w16
			.else
	smsubl _scalar_x7, w15, w16, x17
			.endif
		.elseif \operation == 6
			.if \addend_zero
	umull _scalar_x7, w15, w16
			.else
	umaddl _scalar_x7, w15, w16, x17
			.endif
		.elseif \operation == 7
			.if \addend_zero
	umnegl _scalar_x7, w15, w16
			.else
	umsubl _scalar_x7, w15, w16, x17
			.endif
		.elseif \operation == 8
	smulh _scalar_x7, x15, x16
		.else
	umulh _scalar_x7, x15, x16
		.endif
	.endif
	and x9, x8, #0xffff
	str _scalar_x7, [_vcpu, x9]
	next_gadget
	scalar_variants \forward, Lsn\@, Lsm\@, Lsa\@, multiply_add_fast, \operation, \left_zero, \right_zero, \addend_zero, \suffix
.endm
.macro multiply_result operation, left_zero, right_zero, addend_zero, prefix
multiply_add_fast \operation, \left_zero, \right_zero, \addend_zero, \prefix\()_r
.endm
.macro multiply_addends operation, left_zero, right_zero, prefix
multiply_result \operation, \left_zero, \right_zero, 0, \prefix\()_r
multiply_result \operation, \left_zero, \right_zero, 1, \prefix\()_z
.endm
.macro multiply_sources operation, prefix
multiply_addends \operation, 0, 0, \prefix\()_p
multiply_addends \operation, 1, 0, \prefix\()_z
.endm

multiply_sources 0, w_madd
multiply_sources 1, w_msub
multiply_sources 2, x_madd
multiply_sources 3, x_msub
multiply_sources 4, smaddl
multiply_sources 5, smsubl
multiply_sources 6, umaddl
multiply_sources 7, umsubl
.macro multiply_high_sources operation, prefix
multiply_result \operation, 0, 0, 1, \prefix\()_p_z
multiply_result \operation, 1, 0, 1, \prefix\()_z_z
.endm
multiply_high_sources 8, smulh
multiply_high_sources 9, umulh

.purgem multiply_high_sources
.purgem multiply_sources
.purgem multiply_addends
.purgem multiply_result
.purgem multiply_add_fast

/* Translation-selected LSLV/LSRV/ASRV/RORV. Discarded destinations are
 * folded to NOP, so every remaining entry stores a result. */
.macro variable_shift_fast operation, bits64, left_zero, right_zero, same_source, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.set Lsn\@, Lscalar_offset16
	.if \left_zero
	.set Lsn\@, Lscalar_none
	.endif
	.set Lsm\@, Lscalar_offset32
	.if \right_zero || \same_source
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry variable_shift_\suffix, \forward, 6, Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _vcpu
	.endif
	.if \right_zero
	mov x16, xzr
	.elseif \same_source
	mov x16, x15
	.else
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _vcpu
	.endif
	.if \bits64
		.if \operation == 0
	lsl _scalar_x7, x15, x16
		.elseif \operation == 1
	lsr _scalar_x7, x15, x16
		.elseif \operation == 2
	asr _scalar_x7, x15, x16
		.else
	ror _scalar_x7, x15, x16
		.endif
	.else
		.if \operation == 0
	lsl _scalar_w7, w15, w16
		.elseif \operation == 1
	lsr _scalar_w7, w15, w16
		.elseif \operation == 2
	asr _scalar_w7, w15, w16
		.else
	ror _scalar_w7, w15, w16
		.endif
	.endif
	uxth w9, w8
	str _scalar_x7, [_vcpu, x9]
	next_gadget
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, variable_shift_fast, \operation, \bits64, \left_zero, \right_zero, \same_source, \suffix
.endm

.macro variable_shift_sources operation, bits64, prefix
variable_shift_fast \operation, \bits64, 0, 0, 0, \prefix\()_rr_r
variable_shift_fast \operation, \bits64, 0, 0, 1, \prefix\()_eq_r
variable_shift_fast \operation, \bits64, 0, 1, 0, \prefix\()_rz_r
variable_shift_fast \operation, \bits64, 1, 0, 0, \prefix\()_zr_r
variable_shift_fast \operation, \bits64, 1, 1, 0, \prefix\()_zz_r
.endm

variable_shift_sources 0, 0, w_lsl
variable_shift_sources 1, 0, w_lsr
variable_shift_sources 2, 0, w_asr
variable_shift_sources 3, 0, w_ror
variable_shift_sources 0, 1, x_lsl
variable_shift_sources 1, 1, x_lsr
variable_shift_sources 2, 1, x_asr
variable_shift_sources 3, 1, x_ror

.purgem variable_shift_sources
.purgem variable_shift_fast

/* Translation-selected UDIV/SDIV. Host A64 already implements the guest's
 * zero-divisor and signed-overflow results without a conditional branch.
 * Discarded destinations are folded to NOP. */
.macro divide_fast signed, bits64, left_zero, right_zero, same_source, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.set Lsn\@, Lscalar_offset16
	.if \left_zero
	.set Lsn\@, Lscalar_none
	.endif
	.set Lsm\@, Lscalar_offset32
	.if \right_zero || \same_source
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry divide_\suffix, \forward, 7, Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	.if \left_zero
	mov x15, xzr
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _vcpu
	.endif
	.if \right_zero
	mov x16, xzr
	.elseif \same_source
	mov x16, x15
	.else
	scalar_load \forward, 2, x16, 11, 8, 32, 16, 0, _vcpu
	.endif
	.if \bits64
		.if \signed
	sdiv _scalar_x7, x15, x16
		.else
	udiv _scalar_x7, x15, x16
		.endif
	.else
		.if \signed
	sdiv _scalar_w7, w15, w16
		.else
	udiv _scalar_w7, w15, w16
		.endif
	.endif
	uxth w9, w8
	str _scalar_x7, [_vcpu, x9]
	next_gadget
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, divide_fast, \signed, \bits64, \left_zero, \right_zero, \same_source, \suffix
.endm
.macro divide_sources signed, bits64, prefix
divide_fast \signed, \bits64, 0, 0, 0, \prefix\()_rr_r
divide_fast \signed, \bits64, 0, 0, 1, \prefix\()_eq_r
divide_fast \signed, \bits64, 0, 1, 0, \prefix\()_rz_r
divide_fast \signed, \bits64, 1, 0, 0, \prefix\()_zr_r
divide_fast \signed, \bits64, 1, 1, 0, \prefix\()_zz_r
.endm

divide_sources 0, 0, w_u
divide_sources 1, 0, w_s
divide_sources 0, 1, x_u
divide_sources 1, 1, x_s

.purgem divide_sources
.purgem divide_fast

/* Translation-selected checksum operations. Operation, source mode, and
 * direct CpuState byte offsets are all fixed before execution. Native
 * extension bodies remain the shortest supported-host path; software entries
 * share one fully unrolled body per operation for extension-less hosts. */
.macro crc32_load_sources source_mode
	ldr x11, [_stream], #8
	and x10, x11, #0xffff
	.ifc \source_mode, rr
	ubfx x8, x11, #16, #16
	lsr x9, x11, #32
	ldr x14, [_vcpu, x8]
	ldr x15, [_vcpu, x9]
	.else
	.ifc \source_mode, same
	ubfx x8, x11, #16, #16
	ldr x14, [_vcpu, x8]
	mov x15, x14
	.else
	.ifc \source_mode, rz
	ubfx x8, x11, #16, #16
	ldr x14, [_vcpu, x8]
	mov x15, xzr
	.else
	.ifc \source_mode, zr
	lsr x9, x11, #32
	mov w14, wzr
	ldr x15, [_vcpu, x9]
	.else
	mov w14, wzr
	mov x15, xzr
	.endif
	.endif
	.endif
	.endif
.endm

.macro crc32_native operation, source_mode, suffix
gadget crc32_\suffix
	crc32_load_sources \source_mode
	.if \operation == 0
	crc32b w16, w14, w15
	.elseif \operation == 1
	crc32h w16, w14, w15
	.elseif \operation == 2
	crc32w w16, w14, w15
	.elseif \operation == 3
	crc32x w16, w14, x15
	.elseif \operation == 4
	crc32cb w16, w14, w15
	.elseif \operation == 5
	crc32ch w16, w14, w15
	.elseif \operation == 6
	crc32cw w16, w14, w15
	.else
	crc32cx w16, w14, x15
	.endif
	str x16, [_vcpu, x10]
	next_gadget
.endm

.macro crc32_software_step
	sbfx w17, w14, #0, #1
	and w17, w17, w13
	eor w14, w17, w14, lsr #1
.endm

.macro crc32_software_byte
	uxtb w17, w15
	eor w14, w14, w17
	.rept 8
	crc32_software_step
	.endr
.endm

.macro crc32_software_entry source_mode, suffix
gadget crc32_software_\suffix
	crc32_load_sources \source_mode
	b 91f
.endm

.macro crc32_sources operation, prefix, bytes, polynomial_low, polynomial_high
crc32_native \operation, rr, \prefix\()_rr
crc32_native \operation, same, \prefix\()_same
crc32_native \operation, rz, \prefix\()_rz
crc32_native \operation, zr, \prefix\()_zr
crc32_native \operation, zz, \prefix\()_zz
crc32_software_entry rr, \prefix\()_rr
crc32_software_entry same, \prefix\()_same
crc32_software_entry rz, \prefix\()_rz
crc32_software_entry zr, \prefix\()_zr
crc32_software_entry zz, \prefix\()_zz
91:
	movz w13, #\polynomial_low
	movk w13, #\polynomial_high, lsl #16
	crc32_software_byte
	.if \bytes > 1
	lsr x15, x15, #8
	crc32_software_byte
	.endif
	.if \bytes > 2
	lsr x15, x15, #8
	crc32_software_byte
	lsr x15, x15, #8
	crc32_software_byte
	.endif
	.if \bytes > 4
	lsr x15, x15, #8
	crc32_software_byte
	lsr x15, x15, #8
	crc32_software_byte
	lsr x15, x15, #8
	crc32_software_byte
	lsr x15, x15, #8
	crc32_software_byte
	.endif
	str x14, [_vcpu, x10]
	next_gadget
.endm

crc32_sources 0, b, 1, 0x8320, 0xedb8
crc32_sources 1, h, 2, 0x8320, 0xedb8
crc32_sources 2, w, 4, 0x8320, 0xedb8
crc32_sources 3, x, 8, 0x8320, 0xedb8
crc32_sources 4, cb, 1, 0x3b78, 0x82f6
crc32_sources 5, ch, 2, 0x3b78, 0x82f6
crc32_sources 6, cw, 4, 0x3b78, 0x82f6
crc32_sources 7, cx, 8, 0x3b78, 0x82f6

.purgem crc32_sources
.purgem crc32_software_entry
.purgem crc32_software_byte
.purgem crc32_software_step
.purgem crc32_native
.purgem crc32_load_sources

/* Translation-selected EXTR/ROR. Width, zero shift, and all XZR choices are
 * fixed. Stream operand: Rd, Rn, Rm, shift in successive bytes. */
.macro extract_register_fast bits64, shift_nonzero, left_zero, right_zero, destination_zero, suffix, forward=0
	.set Lsd\@, Lscalar_register0
	.if \destination_zero
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_register8
	.if \left_zero
	.set Lsn\@, Lscalar_none
	.endif
	.set Lsm\@, Lscalar_register16
	.if \right_zero
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry extract_register_\suffix, \forward, 8, Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	add x13, _vcpu, #UML_GADGET_CPU_X0
	.if \left_zero
	mov x14, xzr
	.else
	scalar_load \forward, 1, x14, 10, 8, 8, 5, 3, x13
	.endif
	.if \right_zero
	mov x15, xzr
	.else
	scalar_load \forward, 2, x15, 11, 8, 16, 5, 3, x13
	.endif
	.if \shift_nonzero
	ubfx w12, w8, #24, #6
	/* LSL takes its amount modulo the register width and this variant
	 * guarantees a shift of 1..width-1, so the complement is just the
	 * negation: (2^32 - shift) mod width == width - shift for both widths,
	 * since 2^32 is a multiple of 64. One instruction instead of two, and
	 * the chain no longer waits on a constant materialisation. */
		.if \bits64
	lsr _scalar_x16, x15, x12
	neg w17, w12
	lsl x17, x14, x17
	orr _scalar_x16, _scalar_x16, x17
		.else
	lsr _scalar_w16, w15, w12
	neg w17, w12
	lsl w17, w14, w17
	orr _scalar_w16, _scalar_w16, w17
		.endif
	.else
		.if \bits64
	mov _scalar_x16, x15
		.else
	mov _scalar_w16, w15
		.endif
	.endif
	.if !\destination_zero
	and w9, w8, #0x1f
	str _scalar_x16, [x13, x9, lsl #3]
	.endif
	next_gadget
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, extract_register_fast, \bits64, \shift_nonzero, \left_zero, \right_zero, \destination_zero, \suffix
.endm
.macro extract_destinations bits64, shift_nonzero, left_zero, right_zero, prefix
extract_register_fast \bits64, \shift_nonzero, \left_zero, \right_zero, 0, \prefix\()_r
extract_register_fast \bits64, \shift_nonzero, \left_zero, \right_zero, 1, \prefix\()_z
.endm
.macro extract_sources bits64, shift_nonzero, prefix
extract_destinations \bits64, \shift_nonzero, 0, 0, \prefix\()_rr
extract_destinations \bits64, \shift_nonzero, 0, 1, \prefix\()_rz
extract_destinations \bits64, \shift_nonzero, 1, 0, \prefix\()_zr
extract_destinations \bits64, \shift_nonzero, 1, 1, \prefix\()_zz
.endm

extract_sources 0, 0, w_n
extract_sources 0, 1, w_s
extract_sources 1, 0, x_n
extract_sources 1, 1, x_s

.purgem extract_sources
.purgem extract_destinations
.purgem extract_register_fast

/* Production CSEL/CSINC/CSINV/CSNEG use the native conditional-select family
 * directly. Builder maps XZR sources to CpuState::gadget_zero and discards an
 * XZR destination as NOP, so no opcode/register-mode branches remain. */
.macro conditional_select_fast name, condition, operation, bits64, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.set Lsn\@, Lscalar_offset16
	.set Lsm\@, Lscalar_offset32
scalar_entry conditional_select_fast_\name\()_\suffix, \forward, 9, Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	scalar_load \forward, 1, x15, 10, 8, 16
	scalar_load \forward, 2, x16, 11, 8, 32
	msr nzcv, x27
	.if \bits64
		.if \operation == 0
	csel _scalar_x7, x15, x16, \condition
		.elseif \operation == 1
	csinc _scalar_x7, x15, x16, \condition
		.elseif \operation == 2
	csinv _scalar_x7, x15, x16, \condition
		.else
	csneg _scalar_x7, x15, x16, \condition
		.endif
	.else
		.if \operation == 0
	csel _scalar_w7, w15, w16, \condition
		.elseif \operation == 1
	csinc _scalar_w7, w15, w16, \condition
		.elseif \operation == 2
	csinv _scalar_w7, w15, w16, \condition
		.else
	csneg _scalar_w7, w15, w16, \condition
		.endif
	.endif
	str _scalar_x7, [_vcpu, x9]
	next_gadget
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, conditional_select_fast, \name, \condition, \operation, \bits64, \suffix
.endm
.macro conditional_select_fast_modes name, condition
conditional_select_fast \name, \condition, 0, 0, w_sel
conditional_select_fast \name, \condition, 0, 1, x_sel
conditional_select_fast \name, \condition, 1, 0, w_inc
conditional_select_fast \name, \condition, 1, 1, x_inc
conditional_select_fast \name, \condition, 2, 0, w_inv
conditional_select_fast \name, \condition, 2, 1, x_inv
conditional_select_fast \name, \condition, 3, 0, w_neg
conditional_select_fast \name, \condition, 3, 1, x_neg
.endm

conditional_select_fast_modes eq, eq
conditional_select_fast_modes ne, ne
conditional_select_fast_modes hs, hs
conditional_select_fast_modes lo, lo
conditional_select_fast_modes mi, mi
conditional_select_fast_modes pl, pl
conditional_select_fast_modes vs, vs
conditional_select_fast_modes vc, vc
conditional_select_fast_modes hi, hi
conditional_select_fast_modes ls, ls
conditional_select_fast_modes ge, ge
conditional_select_fast_modes lt, lt
conditional_select_fast_modes gt, gt
conditional_select_fast_modes le, le

/* AL and NV both select the first operand unconditionally.  Translation
 * shares these two compact copy gadgets across all four opcodes, avoiding an
 * NZCV restore, a second source load, and a conditional-select instruction. */
.macro conditional_select_always bits64, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.set Lsn\@, Lscalar_offset16
scalar_entry conditional_select_fast_always_\suffix, \forward, 9, Lsd\@, Lsn\@, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	scalar_load \forward, 1, _scalar_x15, 10, 8, 16, 16, 0, _vcpu
	.if \bits64
	str _scalar_x15, [_vcpu, x9]
	.else
	mov _scalar_w7, _scalar_w15
	str _scalar_x7, [_vcpu, x9]
	.endif
	next_gadget
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, conditional_select_always, \bits64, \suffix
.endm
conditional_select_always 0, w
conditional_select_always 1, x

.purgem conditional_select_always
.purgem conditional_select_fast_modes
.purgem conditional_select_fast

/* Production CCMN/CCMP. The sole conditional branch is the architectural
 * condition itself; width, add/subtract, immediate/register and XZR selection
 * are fixed at translation. Stream: left offset, right offset/immediate, and
 * the already-positioned alternative NZCV value. */
.macro conditional_compare_fast name, condition, subtract, bits64, immediate, always_true, suffix, forward=0
	.set Lsn\@, Lscalar_offset0
	.set Lsm\@, Lscalar_offset16
	.if \immediate
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry conditional_compare_fast_\name\()_\suffix, \forward, 10, Lscalar_none, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	.if \always_true == 0
	msr nzcv, x27
	b.\condition Lconditional_compare_true\@
	lsr x27, x8, #32
	next_gadget
Lconditional_compare_true\@:
	.endif
	scalar_load \forward, 1, x12, 10, 8, 0, 16, 0, _vcpu
	.if \immediate
	ubfx x13, x8, #16, #5
	.else
	scalar_load \forward, 2, x13, 11, 8, 16, 16, 0, _vcpu
	.endif
	.if \bits64
		.if \subtract
	cmp x12, x13
		.else
	cmn x12, x13
		.endif
	.else
		.if \subtract
	cmp w12, w13
		.else
	cmn w12, w13
		.endif
	.endif
	mrs x27, nzcv
	next_gadget
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, conditional_compare_fast, \name, \condition, \subtract, \bits64, \immediate, \always_true, \suffix
.endm
.macro conditional_compare_fast_modes name, condition, always_true
conditional_compare_fast \name, \condition, 0, 0, 0, \always_true, w_cmn_r
conditional_compare_fast \name, \condition, 0, 0, 1, \always_true, w_cmn_i
conditional_compare_fast \name, \condition, 0, 1, 0, \always_true, x_cmn_r
conditional_compare_fast \name, \condition, 0, 1, 1, \always_true, x_cmn_i
conditional_compare_fast \name, \condition, 1, 0, 0, \always_true, w_cmp_r
conditional_compare_fast \name, \condition, 1, 0, 1, \always_true, w_cmp_i
conditional_compare_fast \name, \condition, 1, 1, 0, \always_true, x_cmp_r
conditional_compare_fast \name, \condition, 1, 1, 1, \always_true, x_cmp_i
.endm

conditional_compare_fast_modes eq, eq, 0
conditional_compare_fast_modes ne, ne, 0
conditional_compare_fast_modes hs, hs, 0
conditional_compare_fast_modes lo, lo, 0
conditional_compare_fast_modes mi, mi, 0
conditional_compare_fast_modes pl, pl, 0
conditional_compare_fast_modes vs, vs, 0
conditional_compare_fast_modes vc, vc, 0
conditional_compare_fast_modes hi, hi, 0
conditional_compare_fast_modes ls, ls, 0
conditional_compare_fast_modes ge, ge, 0
conditional_compare_fast_modes lt, lt, 0
conditional_compare_fast_modes gt, gt, 0
conditional_compare_fast_modes le, le, 0
conditional_compare_fast_modes always, al, 1

.purgem conditional_compare_fast_modes
.purgem conditional_compare_fast

/* Production logical shifted-register entries.  Builder specializes width,
 * operation, inversion, shift kind (including the common no-shift form), and
 * every XZR choice. Stream: Rd, Rn, Rm, shift in successive bytes. */
.macro logical_shifted_fast operation, bits64, invert, shift_kind, left_zero, right_zero, destination_zero, suffix, forward=0
	.set Lsd\@, Lscalar_register0
	.if \destination_zero
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_register8
	.if \left_zero
	.set Lsn\@, Lscalar_none
	.endif
	.set Lsm\@, Lscalar_register16
	.if \right_zero
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry logical_shifted_\suffix, \forward, 11, Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	.if \left_zero
		.if \operation != 1 || \invert || \shift_kind != 0 || \destination_zero
	mov x15, xzr
		.endif
	.else
	scalar_load \forward, 1, x15, 10, 8, 8, 5, 3, _vcpu
	.endif
	.if \right_zero
	.if \operation == 1 && \bits64 && \left_zero && !\invert && \shift_kind == 0 && !\destination_zero
	mov _scalar_x16, xzr
	.else
	mov x16, xzr
	.endif
	.else
	.if \operation == 1 && \bits64 && \left_zero && !\invert && \shift_kind == 0 && !\destination_zero
	scalar_load \forward, 2, _scalar_x16, 11, 8, 16, 5, 3, _vcpu
	.else
	scalar_load \forward, 2, x16, 11, 8, 16, 5, 3, _vcpu
	.endif
	.endif
	.if \shift_kind != 0
	ubfx w6, w8, #24, #6
		.if \bits64
			.if \shift_kind == 1
	lsl x16, x16, x6
			.elseif \shift_kind == 2
	lsr x16, x16, x6
			.elseif \shift_kind == 3
	asr x16, x16, x6
			.else
	ror x16, x16, x6
			.endif
		.else
			.if \shift_kind == 1
	lsl w16, w16, w6
			.elseif \shift_kind == 2
	lsr w16, w16, w6
			.elseif \shift_kind == 3
	asr w16, w16, w6
			.else
	ror w16, w16, w6
			.endif
		.endif
	.endif
	.if \invert
	mvn x16, x16
	.endif
	.if \operation == 1 && \left_zero && !\invert && \shift_kind == 0 && !\destination_zero
		.if \bits64
	and w9, w8, #0x1f
	str _scalar_x16, [_vcpu, x9, lsl #3]
		.else
	mov _scalar_w7, w16
	and w9, w8, #0x1f
	str _scalar_x7, [_vcpu, x9, lsl #3]
		.endif
	.else
		.if \bits64
		.if \operation == 0
	and _scalar_x7, x15, x16
		.elseif \operation == 1
	orr _scalar_x7, x15, x16
		.elseif \operation == 2
	eor _scalar_x7, x15, x16
		.else
	ands _scalar_x7, x15, x16
		.endif
	.else
		.if \operation == 0
	and _scalar_w7, w15, w16
		.elseif \operation == 1
	orr _scalar_w7, w15, w16
		.elseif \operation == 2
	eor _scalar_w7, w15, w16
		.else
	ands _scalar_w7, w15, w16
		.endif
		.endif
		.if \operation == 3
	mrs x27, nzcv
		.endif
		.if !\destination_zero
	and w9, w8, #0x1f
	str _scalar_x7, [_vcpu, x9, lsl #3]
		.endif
	.endif
	next_gadget
	/* Preserve the established bank layout after removing the base add. */
	nop
	/* Keep later logical entries at their established cache-line phase. The
	 * native 64-bit MOV alias removed two additional runtime instructions. */
	.if \operation == 1 && \bits64 && \left_zero && !\invert && \shift_kind == 0 && !\destination_zero
	nop
	nop
	nop
	.endif
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, logical_shifted_fast, \operation, \bits64, \invert, \shift_kind, \left_zero, \right_zero, \destination_zero, \suffix
.endm
.macro logical_shifted_destinations operation, bits64, invert, shift_kind, left_zero, right_zero, prefix
logical_shifted_fast \operation, \bits64, \invert, \shift_kind, \left_zero, \right_zero, 0, \prefix\()_r
	.if \operation == 3
logical_shifted_fast \operation, \bits64, \invert, \shift_kind, \left_zero, \right_zero, 1, \prefix\()_z
	.endif
.endm
.macro logical_shifted_sources operation, bits64, invert, shift_kind, prefix
logical_shifted_destinations \operation, \bits64, \invert, \shift_kind, 0, 0, \prefix\()_rr
logical_shifted_destinations \operation, \bits64, \invert, \shift_kind, 0, 1, \prefix\()_rz
logical_shifted_destinations \operation, \bits64, \invert, \shift_kind, 1, 0, \prefix\()_zr
logical_shifted_destinations \operation, \bits64, \invert, \shift_kind, 1, 1, \prefix\()_zz
.endm
.macro logical_shifted_shifts operation, bits64, invert, prefix
logical_shifted_sources \operation, \bits64, \invert, 0, \prefix\()_n
logical_shifted_sources \operation, \bits64, \invert, 1, \prefix\()_l
logical_shifted_sources \operation, \bits64, \invert, 2, \prefix\()_r
logical_shifted_sources \operation, \bits64, \invert, 3, \prefix\()_a
logical_shifted_sources \operation, \bits64, \invert, 4, \prefix\()_o
.endm

logical_shifted_shifts 0, 0, 0, w_and_p
logical_shifted_shifts 0, 0, 1, w_and_n
logical_shifted_shifts 1, 0, 0, w_orr_p
logical_shifted_shifts 1, 0, 1, w_orr_n
logical_shifted_shifts 2, 0, 0, w_eor_p
logical_shifted_shifts 2, 0, 1, w_eor_n
logical_shifted_shifts 3, 0, 0, w_ands_p
logical_shifted_shifts 3, 0, 1, w_ands_n
logical_shifted_shifts 0, 1, 0, x_and_p
logical_shifted_shifts 0, 1, 1, x_and_n
logical_shifted_shifts 1, 1, 0, x_orr_p
logical_shifted_shifts 1, 1, 1, x_orr_n
logical_shifted_shifts 2, 1, 0, x_eor_p
logical_shifted_shifts 2, 1, 1, x_eor_n
logical_shifted_shifts 3, 1, 0, x_ands_p
logical_shifted_shifts 3, 1, 1, x_ands_n

.purgem logical_shifted_shifts
.purgem logical_shifted_sources
.purgem logical_shifted_destinations
.purgem logical_shifted_fast

/* Production logical-immediate entries.  The immediate is decoded once by
 * Builder; width, operation, XZR/SP meanings and flag update are entry-point
 * constants. Stream: [Rd[4:0], Rn[12:8], W immediate[63:32]][X immediate]: a
 * 32-bit immediate shares the register word, a 64-bit one has a word of its
 * own. */
.macro logical_immediate_fast operation, bits64, source_zero, destination_special, suffix, forward=0
	.set Lsd\@, Lscalar_register0
	.if \destination_special && \operation != 3
	.set Lsd\@, Lscalar_sp
	.endif
	.if \destination_special && \operation == 3
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_register8
	.if \source_zero
	.set Lsn\@, Lscalar_none
	.endif
scalar_entry logical_immediate_\suffix, \forward, 12, Lsd\@, Lsn\@, Lscalar_none, Lscalar_none, 1 + \bits64
	ldr x8, [_stream], #8
	.if \bits64
	ldr x14, [_stream], #8
	.else
	lsr x14, x8, #32
	.endif
	.if \source_zero
		.if \operation == 0
	mov _scalar_x16, xzr
		.elseif \operation == 1 || \operation == 2
	mov _scalar_x16, x14
		.else
	mov _scalar_x16, xzr
	mov x27, #0x40000000
		.endif
	.else
	scalar_load \forward, 1, x15, 10, 8, 8, 5, 3, _vcpu
		.if \bits64
			.if \operation == 0
	and _scalar_x16, x15, x14
			.elseif \operation == 1
	orr _scalar_x16, x15, x14
			.elseif \operation == 2
	eor _scalar_x16, x15, x14
			.else
	ands _scalar_x16, x15, x14
			.endif
		.else
			.if \operation == 0
	and _scalar_w16, w15, w14
			.elseif \operation == 1
	orr _scalar_w16, w15, w14
			.elseif \operation == 2
	eor _scalar_w16, w15, w14
			.else
	ands _scalar_w16, w15, w14
			.endif
		.endif
		.if \operation == 3
	mrs x27, nzcv
		.endif
	.endif
	.if \destination_special
		.if \operation != 3
	str _scalar_x16, [_vcpu, #UML_GADGET_CPU_SP]
		.endif
	.else
	and w9, w8, #0x1f
	str _scalar_x16, [_vcpu, x9, lsl #3]
	.endif
	next_gadget
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, logical_immediate_fast, \operation, \bits64, \source_zero, \destination_special, \suffix
.endm
.macro logical_immediate_modes operation, bits64, prefix
logical_immediate_fast \operation, \bits64, 0, 0, \prefix\()_rr
logical_immediate_fast \operation, \bits64, 0, 1, \prefix\()_rs
logical_immediate_fast \operation, \bits64, 1, 0, \prefix\()_zr
logical_immediate_fast \operation, \bits64, 1, 1, \prefix\()_zs
.endm

logical_immediate_modes 0, 0, w_and
logical_immediate_modes 1, 0, w_orr
logical_immediate_modes 2, 0, w_eor
logical_immediate_modes 3, 0, w_ands
logical_immediate_modes 0, 1, x_and
logical_immediate_modes 1, 1, x_orr
logical_immediate_modes 2, 1, x_eor
logical_immediate_modes 3, 1, x_ands

.purgem logical_immediate_modes
.purgem logical_immediate_fast

/* Production SBFM/BFM/UBFM. Builder has already decoded both masks, folds a
 * discarded Rd to NOP, and selects width/operation/XZR/zero-rotate. Stream:
 * [destination/source CpuState offsets, rotate, imms][write mask][test mask]. */
.macro bitfield_fast operation, bits64, rotate_nonzero, source_zero, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.set Lsn\@, Lscalar_offset16
	.if \source_zero
	.set Lsn\@, Lscalar_none
	.endif
	.set Lsm\@, Lscalar_offset0
	.if \operation != 1
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry bitfield_\suffix, \forward, 13, Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 3
	ldr x8, [_stream], #8
	ldr x14, [_stream], #8
	ldr x17, [_stream], #8
	.if \source_zero
	mov x15, xzr
	.else
	scalar_load \forward, 1, x15, 10, 8, 16, 16, 0, _vcpu
	.endif
	.if \operation == 1
	/* Capture the old destination before the result overwrites x0. */
	scalar_load \forward, 2, x6, 9, 8, 0
	.endif
	.if !\bits64
	mov w15, w15
	.endif
	.if \rotate_nonzero
	ubfx x12, x8, #32, #6
		.if \bits64
	ror _scalar_x16, x15, x12
		.else
	ror _scalar_w16, w15, w12
		.endif
	.else
	mov _scalar_x16, x15
	.endif
	and _scalar_x16, _scalar_x16, x14
	.if \operation == 0
	ubfx x11, x8, #40, #6
	lsr x6, x15, x11
	tst x6, #1
	csetm x6, ne
	.elseif \operation == 1
	bic x7, x6, x14
	orr _scalar_x16, _scalar_x16, x7
	.else
	mov x6, xzr
	.endif
	bic x6, x6, x17
	and _scalar_x16, _scalar_x16, x17
	orr _scalar_x16, _scalar_x16, x6
	.if !\bits64
	mov _scalar_w16, _scalar_w16
	.endif
	and x9, x8, #0xffff
	str _scalar_x16, [_vcpu, x9]
	next_gadget
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, bitfield_fast, \operation, \bits64, \rotate_nonzero, \source_zero, \suffix
.endm
.macro bitfield_sources operation, bits64, prefix
bitfield_fast \operation, \bits64, 0, 0, \prefix\()_n_r
bitfield_fast \operation, \bits64, 0, 1, \prefix\()_n_z
bitfield_fast \operation, \bits64, 1, 0, \prefix\()_r_r
bitfield_fast \operation, \bits64, 1, 1, \prefix\()_r_z
.endm

bitfield_sources 0, 0, w_s
bitfield_sources 1, 0, w_b
bitfield_sources 2, 0, w_u
bitfield_sources 0, 1, x_s
bitfield_sources 1, 1, x_b
bitfield_sources 2, 1, x_u

.purgem bitfield_sources
.purgem bitfield_fast

/* Translation-selected data-processing (1 source) operations. Source and
 * destination are direct CpuState byte offsets. The optional native forms are
 * selected only when the running host advertises the matching extension. */
.macro bit_reverse_count_fast operation, bits64, native, suffix, forward=0
	.set Lsd\@, Lscalar_offset16
	.set Lsn\@, Lscalar_offset0
scalar_entry bit_reverse_count_\suffix, \forward, 14, Lsd\@, Lsn\@, Lscalar_none, Lscalar_none, 1
	ldr x9, [_stream], #8
	lsr x11, x9, #16
	scalar_load \forward, 1, x12, 10, 9, 0
	.if \native
		.if \bits64
			.if \operation == 6
	.inst 0xdac01980 /* ctz x0, x12 */
			.elseif \operation == 7
	.inst 0xdac01d80 /* cnt x0, x12 */
			.else
	.inst 0xdac02180 /* abs x0, x12 */
			.endif
		.else
			.if \operation == 6
	.inst 0x5ac01980 /* ctz w0, w12 */
			.elseif \operation == 7
	.inst 0x5ac01d80 /* cnt w0, w12 */
			.else
	.inst 0x5ac02180 /* abs w0, w12 */
			.endif
		.endif
	.elseif \bits64
		.if \operation == 0
	rbit _scalar_x13, x12
		.elseif \operation == 1
	rev16 _scalar_x13, x12
		.elseif \operation == 2
	rev32 _scalar_x13, x12
		.elseif \operation == 3
	rev _scalar_x13, x12
		.elseif \operation == 4
	clz _scalar_x13, x12
		.elseif \operation == 5
	cls _scalar_x13, x12
		.elseif \operation == 6
	rbit _scalar_x13, x12
	clz _scalar_x13, _scalar_x13
		.elseif \operation == 7
	fmov d0, x12
	cnt v0.8b, v0.8b
	addv b0, v0.8b
	umov _scalar_w13, v0.b[0]
		.else
	cmp x12, #0
	cneg _scalar_x13, x12, mi
		.endif
	.else
		.if \operation == 0
	rbit _scalar_w13, w12
		.elseif \operation == 1
	rev16 _scalar_w13, w12
		.elseif \operation == 2
	rev _scalar_w13, w12
		.elseif \operation == 4
	clz _scalar_w13, w12
		.elseif \operation == 5
	cls _scalar_w13, w12
		.elseif \operation == 6
	rbit _scalar_w13, w12
	clz _scalar_w13, _scalar_w13
		.elseif \operation == 7
	mov w12, w12
	fmov d0, x12
	cnt v0.8b, v0.8b
	addv b0, v0.8b
	umov _scalar_w13, v0.b[0]
		.else
	cmp w12, #0
	cneg _scalar_w13, w12, mi
		.endif
	.endif
	str _scalar_x13, [_vcpu, x11]
	next_gadget
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, bit_reverse_count_fast, \operation, \bits64, \native, \suffix
.endm

bit_reverse_count_fast 0, 0, 0, w_rbit
bit_reverse_count_fast 1, 0, 0, w_rev16
bit_reverse_count_fast 2, 0, 0, w_rev
bit_reverse_count_fast 4, 0, 0, w_clz
bit_reverse_count_fast 5, 0, 0, w_cls
bit_reverse_count_fast 6, 0, 0, w_ctz
bit_reverse_count_fast 7, 0, 0, w_cnt
bit_reverse_count_fast 8, 0, 0, w_abs
bit_reverse_count_fast 0, 1, 0, x_rbit
bit_reverse_count_fast 1, 1, 0, x_rev16
bit_reverse_count_fast 2, 1, 0, x_rev32
bit_reverse_count_fast 3, 1, 0, x_rev
bit_reverse_count_fast 4, 1, 0, x_clz
bit_reverse_count_fast 5, 1, 0, x_cls
bit_reverse_count_fast 6, 1, 0, x_ctz
bit_reverse_count_fast 7, 1, 0, x_cnt
bit_reverse_count_fast 8, 1, 0, x_abs
bit_reverse_count_fast 6, 0, 1, w_ctz_native
bit_reverse_count_fast 7, 0, 1, w_cnt_native
bit_reverse_count_fast 8, 0, 1, w_abs_native
bit_reverse_count_fast 6, 1, 1, x_ctz_native
bit_reverse_count_fast 7, 1, 1, x_cnt_native
bit_reverse_count_fast 8, 1, 1, x_abs_native

.purgem bit_reverse_count_fast

/* Production FEAT_CSSC min/max. Width, operation and operand form are fixed;
 * source fields are translation-resolved CpuState offsets. For immediates the
 * upper stream word already contains the correctly extended comparison value. */
.macro integer_min_max_fast name, condition, is_signed, bits64, immediate, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.set Lsn\@, Lscalar_offset16
	.set Lsm\@, Lscalar_offset32
	.if \immediate
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry integer_\name\()_fast_\suffix, \forward, 15, Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	scalar_load \forward, 1, x12, 10, 8, 16, 16, 0, _vcpu
	.if \immediate
		.if \is_signed
	asr x13, x8, #32
		.else
	lsr x13, x8, #32
		.endif
	.else
	scalar_load \forward, 2, x13, 11, 8, 32, 16, 0, _vcpu
	.endif
	.if \bits64
	cmp x12, x13
	csel _scalar_x14, x12, x13, \condition
	.else
	cmp w12, w13
	csel _scalar_w14, w12, w13, \condition
	.endif
	str _scalar_x14, [_vcpu, x9]
	next_gadget
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, integer_min_max_fast, \name, \condition, \is_signed, \bits64, \immediate, \suffix
.endm
.macro integer_min_max_fast_modes name, condition, is_signed
integer_min_max_fast \name, \condition, \is_signed, 0, 0, w_r
integer_min_max_fast \name, \condition, \is_signed, 0, 1, w_i
integer_min_max_fast \name, \condition, \is_signed, 1, 0, x_r
integer_min_max_fast \name, \condition, \is_signed, 1, 1, x_i
.endm

integer_min_max_fast_modes smax, ge, 1
integer_min_max_fast_modes umax, hs, 0
integer_min_max_fast_modes smin, le, 1
integer_min_max_fast_modes umin, ls, 0

.purgem integer_min_max_fast_modes
.purgem integer_min_max_fast

/* Equal inputs and unsigned zero identities are recognized by the builder.
 * These shared copy entries remove the second load, compare and select. */
.macro integer_min_max_copy bits64, suffix, forward=0
	.set Lsd\@, Lscalar_offset0
	.set Lsn\@, Lscalar_offset16
scalar_entry integer_min_max_copy_\suffix, \forward, 15, Lsd\@, Lsn\@, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	ubfx x9, x8, #0, #16
	scalar_load \forward, 1, _scalar_x12, 10, 8, 16, 16, 0, _vcpu
	.if \bits64 == 0
	mov _scalar_w12, _scalar_w12
	.endif
	str _scalar_x12, [_vcpu, x9]
	next_gadget
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, integer_min_max_copy, \bits64, \suffix
.endm

integer_min_max_copy 0, w
integer_min_max_copy 1, x

.purgem integer_min_max_copy

gadget cfinv
	eor w27, w27, #0x20000000
	next_gadget

gadget axflag
	mov w8, w27
	ubfx w9, w8, #30, #1
	ubfx w10, w8, #29, #1
	ubfx w11, w8, #28, #1
	orr w9, w9, w11
	bic w10, w10, w11
	lsl w9, w9, #30
	orr w9, w9, w10, lsl #29
	mov w27, w9
	next_gadget

/* The builder selects these one-instruction host forms only after detecting
 * the exact extension.  The software bodies above remain the portable gadget
 * implementation and keep gadget conformance independent of the JIT
 * eligibility decision. */
gadget axflag_native
	msr nzcv, x27
	.inst 0xd500405f
	mrs x27, nzcv
	next_gadget

gadget xaflag
	mov w8, w27
	ubfx w9, w8, #30, #1
	ubfx w10, w8, #29, #1
	orr w11, w9, w10
	eor w12, w11, #1
	and w13, w9, w10
	bic w14, w9, w10
	lsl w12, w12, #31
	orr w12, w12, w13, lsl #30
	orr w12, w12, w11, lsl #29
	orr w12, w12, w14, lsl #28
	mov w27, w12
	next_gadget

gadget xaflag_native
	msr nzcv, x27
	.inst 0xd500403f
	mrs x27, nzcv
	next_gadget

gadget rmif_fast
	ldr x8, [_stream], #8
	and x12, x8, #0xffff
	ubfx x11, x8, #16, #6
	lsr x14, x8, #32
	ldr x12, [_vcpu, x12]
	ror x13, x12, x11
	lsl w13, w13, #28
	bic w27, w27, w14
	and w13, w13, w14
	orr w27, w27, w13
	next_gadget

gadget rmif_zero
	ldr w8, [_stream], #8
	bic w27, w27, w8
	next_gadget

.macro setf_fast width, suffix
gadget setf_fast_\suffix
	ldr x8, [_stream], #8
	ldr w12, [_vcpu, x8]
	tst w12, #((1 << \width) - 1)
	cset w17, eq
	ubfx w15, w12, #(\width - 1), #1
	eor w16, w12, w12, lsr #1
	ubfx w16, w16, #(\width - 1), #1
	and w13, w27, #0x20000000
	orr w13, w13, w15, lsl #31
	orr w13, w13, w17, lsl #30
	orr w13, w13, w16, lsl #28
	mov w27, w13
	next_gadget
.endm

setf_fast 8, 8
setf_fast 16, 16

.purgem setf_fast

.macro setf_native encoding, suffix
gadget setf_native_\suffix
	ldr x8, [_stream], #8
	ldr w8, [_vcpu, x8]
	msr nzcv, x27
	.inst \encoding
	mrs x27, nzcv
	next_gadget
.endm

/* Both encodings name w8; the guest source field is resolved to its CpuState
 * offset in the immutable stream. */
setf_native 0x3a00090d, 8
setf_native 0x3a00490d, 16

.purgem setf_native
