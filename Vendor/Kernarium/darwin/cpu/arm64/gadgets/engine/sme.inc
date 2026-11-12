/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The SME part of the gadget region: everything the engine runs only where
 * the virtual CPU implements SME. Nothing here is pushed into a base part,
 * so no base entry moves or changes for it.
 *
 * Streaming discipline (../README.md). The host is in streaming mode
 * only while a program translated for SVCR.SM = 1 runs, entered through
 * uml_arm64_gadget_enter_streaming, with host ZA on exactly when the guest's
 * is; guest ZA and ZT0 are then resident in host ZA and ZT0, and in the
 * vCPU's register file otherwise. Code that runs in streaming mode executes
 * no AdvSIMD instruction and calls no helper: a gadget that needs a helper
 * first leaves streaming mode through uml_arm64_gadget_sme_suspend, which
 * spills ZA and ZT0, and enters it again through uml_arm64_gadget_sme_resume.
 * Both keep the host FPSR across the SMSTART or SMSTOP that would reset it.
 * The guest's FPSR is never in the host register in streaming mode, since
 * XNU resets FPSR on every kernel entry while a thread is in streaming mode;
 * its FPCR is (uml_arm64_gadget_enter_streaming).
 * The arm64-gadget-streaming-safety test walks everything a streaming
 * program can reach on every linked image.
 *
 * Names: sme_* gadgets run in every mode, sm_* only with SVCR.SM = 1, and
 * nsm_* only with SVCR.SM = 0. The sme_* and sm_* gadgets lie between
 * uml_arm64_gadget_sme_streaming_begin and _end, and the streaming twins of
 * base gadgets, also sm_*, between uml_arm64_gadget_sm_twins_begin and _end;
 * the table of streaming entries below names both ranges together with the
 * base entries that are safe in streaming mode. */
#include "../sme_layout.h"

.arch_extension sme
.arch_extension sme2
.arch_extension sme-f64f64
.arch_extension sme-i16i64

/* The entries a program translated for SVCR.SM = 1 may name: rows of
 * [begin, end) the builder checks every selected entry against. A base row
 * holds only gadgets whose code, with everything it can reach, uses no
 * AdvSIMD instruction, calls no helper and touches no V register. */
.pushsection __DATA_CONST,__gadget_sme
.p2align 3
.globl CSYM(uml_arm64_gadget_streaming_rows_begin)
CSYM(uml_arm64_gadget_streaming_rows_begin):
	.macro streaming_row begin, end
	.quad CSYM(uml_arm64_gadget_\begin), CSYM(uml_arm64_gadget_\end)
	.endm
	/* One entry: gadgets are 16-byte aligned, so four bytes name one. */
	.macro streaming_entry name
	.quad CSYM(uml_arm64_gadget_\name), CSYM(uml_arm64_gadget_\name) + 4
	.endm
	streaming_row move_wide_constant, bit_reverse_count_w_cnt
	streaming_row bit_reverse_count_w_abs, bit_reverse_count_x_cnt
	streaming_row bit_reverse_count_x_abs, floating_move_immediate
	streaming_row sysreg_cntvctss_read, simd_structure_contig_d_1_store_0
	streaming_row nop, literal_load32_fast_r
	streaming_row svc, fp_link_entry
	streaming_row branch, simd_destination_resident_load
	streaming_row add_sub_immediate_inplace_w_add_n_0, simd_fused_inplace_fmla_4h_0
	streaming_row move_wide_packed_zero, constant_store_end
	streaming_row test_bit_branch_z, test_bit_branch_end
	streaming_row bitfield_shift_w_lsl, bitfield_shift_end
	streaming_entry sysreg_fpcr_read_afp
	streaming_entry return_x30_cached
#if UML_GADGET_EXECUTION_HISTOGRAM
	streaming_entry execution_count
#endif
	streaming_row sme_streaming_begin, sme_streaming_end
	streaming_row sm_twins_begin, sm_twins_end
	streaming_row sm_generated_begin, sm_generated_end
	streaming_row sm_memory_begin, sm_memory_end
	.purgem streaming_row
	.purgem streaming_entry
.globl CSYM(uml_arm64_gadget_streaming_rows_end)
CSYM(uml_arm64_gadget_streaming_rows_end):
.popsection

.pushsection GADGET_SECTION_SME

/* Put the host in the guest's SME mode: SMSTART SM, or SMSTART with ZA on,
 * whose ZA and ZT0 then come from the register file. x1 = CpuState; uses
 * x8-x17 and the vector, predicate and ZA state only, so a gadget keeps
 * x0-x7 across it. */
.p2align 4
uml_arm64_gadget_sme_resume:
	ldrb w9, [_vcpu, #UML_ARM64_CPU_SVCR]
	ldrb w10, [_vcpu, #UML_ARM64_CPU_SME_HOST_MODE]
	mrs x16, fpsr
	tbz w9, #1, 2f
	smstart
	ldr x11, [_vcpu, #UML_ARM64_CPU_SME_REGISTERS]
	ldr w13, [_vcpu, #UML_ARM64_CPU_SME_SVL]
	mov x14, #UML_ARM64_SME_ZA_OFFSET_VL
	madd x11, x13, x14, x11
	mov w12, wzr
1:
	ldr za[w12, 0], [x11]
	ldr za[w12, 1], [x11, #1, mul vl]
	ldr za[w12, 2], [x11, #2, mul vl]
	ldr za[w12, 3], [x11, #3, mul vl]
	ldr za[w12, 4], [x11, #4, mul vl]
	ldr za[w12, 5], [x11, #5, mul vl]
	ldr za[w12, 6], [x11, #6, mul vl]
	ldr za[w12, 7], [x11, #7, mul vl]
	ldr za[w12, 8], [x11, #8, mul vl]
	ldr za[w12, 9], [x11, #9, mul vl]
	ldr za[w12, 10], [x11, #10, mul vl]
	ldr za[w12, 11], [x11, #11, mul vl]
	ldr za[w12, 12], [x11, #12, mul vl]
	ldr za[w12, 13], [x11, #13, mul vl]
	ldr za[w12, 14], [x11, #14, mul vl]
	ldr za[w12, 15], [x11, #15, mul vl]
	add w12, w12, #16
	add x11, x11, x13, lsl #4
	cmp w12, w13
	b.lo 1b
	/* x11 now addresses ZT0, which follows the last row. */
	tbz w10, #4, 3f
	ldr zt0, [x11]
3:
	orr w10, w10, #(UML_ARM64_SME_HOST_STREAMING | UML_ARM64_SME_HOST_ZA)
	b 4f
2:
	smstart sm
	orr w10, w10, #UML_ARM64_SME_HOST_STREAMING
4:
	strb w10, [_vcpu, #UML_ARM64_CPU_SME_HOST_MODE]
	msr fpsr, x16
	ret

/* Leave streaming mode, if the host is in it, spilling ZA and ZT0 to the
 * register file when host ZA is on. The same register contract as
 * uml_arm64_gadget_sme_resume. */
.p2align 4
uml_arm64_gadget_sme_suspend:
	ldrb w10, [_vcpu, #UML_ARM64_CPU_SME_HOST_MODE]
	tbz w10, #0, 9f
	mrs x16, fpsr
	tbz w10, #1, 2f
	ldr x11, [_vcpu, #UML_ARM64_CPU_SME_REGISTERS]
	ldr w13, [_vcpu, #UML_ARM64_CPU_SME_SVL]
	mov x14, #UML_ARM64_SME_ZA_OFFSET_VL
	madd x11, x13, x14, x11
	mov w12, wzr
1:
	str za[w12, 0], [x11]
	str za[w12, 1], [x11, #1, mul vl]
	str za[w12, 2], [x11, #2, mul vl]
	str za[w12, 3], [x11, #3, mul vl]
	str za[w12, 4], [x11, #4, mul vl]
	str za[w12, 5], [x11, #5, mul vl]
	str za[w12, 6], [x11, #6, mul vl]
	str za[w12, 7], [x11, #7, mul vl]
	str za[w12, 8], [x11, #8, mul vl]
	str za[w12, 9], [x11, #9, mul vl]
	str za[w12, 10], [x11, #10, mul vl]
	str za[w12, 11], [x11, #11, mul vl]
	str za[w12, 12], [x11, #12, mul vl]
	str za[w12, 13], [x11, #13, mul vl]
	str za[w12, 14], [x11, #14, mul vl]
	str za[w12, 15], [x11, #15, mul vl]
	add w12, w12, #16
	add x11, x11, x13, lsl #4
	cmp w12, w13
	b.lo 1b
	tbz w10, #4, 2f
	str zt0, [x11]
2:
	smstop
	and w10, w10, #~(UML_ARM64_SME_HOST_STREAMING | UML_ARM64_SME_HOST_ZA)
	strb w10, [_vcpu, #UML_ARM64_CPU_SME_HOST_MODE]
	msr fpsr, x16
9:
	ret

/* uml_arm64_gadget_enter for a streaming program: the same arguments and
 * result. AAPCS64 keeps d8-d15 across a call, and SMSTART and SMSTOP zero
 * them, so they are saved here. The guest's FPCR, with its trap enables
 * clear, is the host's while the program runs; FPCR survives every kernel
 * entry. The guest's FPSR stays in CpuState, where the executor left it
 * with the native FP environment inactive, and the host's own FPCR and FPSR
 * are back when this returns. */
.globl CSYM(uml_arm64_gadget_enter_streaming)
.p2align 4
CSYM(uml_arm64_gadget_enter_streaming):
	stp x29, x30, [sp, #-0x70]!
	mov x29, sp
	stp x19, x20, [sp, #0x10]
	stp d8, d9, [sp, #0x20]
	stp d10, d11, [sp, #0x30]
	stp d12, d13, [sp, #0x40]
	stp d14, d15, [sp, #0x50]
	mrs x9, fpcr
	mrs x10, fpsr
	stp x9, x10, [sp, #0x60]
	ldr w11, [x1, #UML_GADGET_CPU_FPCR]
	mov w12, #UML_ARM64_FPCR_TRAP_ENABLES
	bic w11, w11, w12
	msr fpcr, x11
	mov x19, x1
	bl uml_arm64_gadget_sme_resume
	bl CSYM(uml_arm64_gadget_enter)
	mov x20, x1
	mov x1, x19
	bl uml_arm64_gadget_sme_suspend
	mov x1, x20
	ldp x9, x10, [sp, #0x60]
	msr fpcr, x9
	msr fpsr, x10
	ldp d14, d15, [sp, #0x50]
	ldp d12, d13, [sp, #0x40]
	ldp d10, d11, [sp, #0x30]
	ldp d8, d9, [sp, #0x20]
	ldp x19, x20, [sp, #0x10]
	ldp x29, x30, [sp], #0x70
	ret

/* Translate the \bytes (a register; at most one guest page) at guest address
 * \address through the TLB for \access, as transfer_translate does, leaving
 * the host address in \address. Every crossing or miss the fast path cannot
 * take branches to \slow, whose helper does the whole
 * access in C++. Scratch: x8-x12. */
.macro sme_translate access, address, bytes, slow
	sub x8, \bytes, #1
	add x8, \address, x8
	eor x8, x8, \address
	lsr x8, x8, #UML_GADGET_GUEST_PAGE_SHIFT
	cbnz x8, \slow
	lsr x8, \address, #UML_GADGET_GUEST_PAGE_SHIFT
	ubfx x9, \address, #UML_GADGET_GUEST_PAGE_SHIFT, #UML_GADGET_TLB_INDEX_BITS
	add x10, _swtlb, x9, lsl #UML_GADGET_TLB_SET_SHIFT
	.if \access
	ldp x11, x12, [x10, #UML_GADGET_TLB_ENTRY_WRITE_PAGE]
	.else
	ldp x11, x12, [x10, #UML_GADGET_TLB_ENTRY_READ_PAGE]
	.endif
	cmp x11, x8
	b.ne \slow
	add \address, x12, \address
.endm

/* The base register of a ZA or ZT0 access, in x7; an SP base must be 16-byte
 * aligned (CheckSPAlignment). The descriptor is in x14. */
.macro sme_base slow
	and x9, x14, #0xffff
	ldr x7, [_vcpu, x9]
	tbz x14, #UML_GADGET_SME_BASE_SP_BIT, 81f
	tst x7, #0xf
	b.ne \slow
81:
.endm

/* Drop the tag of the address in x7 where top-byte-ignore is on, unless the
 * base was SP, as the base transfers do. */
.macro sme_untag
	tbnz x14, #UML_GADGET_SME_BASE_SP_BIT, 82f
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, 82f
	ubfx x7, x7, #0, #56
82:
.endm

/* The C++ half of every ZA and ZT0 access and zeroing:
 * uml_arm64_gadget_sme_memory(CpuState, Tlb, descriptor), on the register
 * file, with the descriptor the stream word before _stream. Returns to the
 * caller's next word, or leaves through the memory-fault exit, whose PC word
 * _stream addresses. Streaming callers suspend before and resume after. */
.macro sme_memory_helper fault
	mov x19, x1
	mov x20, x2
	mov x0, x1
	mov x1, x2
	ldur x2, [_stream, #-8]
	helper_call uml_arm64_gadget_sme_memory
	mov x1, x19
	mov x2, x20
	cbz w0, \fault
.endm

	.p2align 4
	.globl CSYM(uml_arm64_gadget_sme_streaming_begin)
CSYM(uml_arm64_gadget_sme_streaming_begin):

/* MRS Xt, SVCR. Stream: [Xt offset]. */
gadget sme_svcr_read
	ldr x8, [_stream], #8
	ldrb w9, [_vcpu, #UML_ARM64_CPU_SVCR]
	str x9, [_vcpu, x8]
	next_gadget

/* MRS Xt, TPIDR2_EL0: the guest's register, never the host's. */
gadget sme_tpidr2_read
	ldr x8, [_stream], #8
	ldr x9, [_vcpu, #UML_ARM64_CPU_TPIDR2]
	str x9, [_vcpu, x8]
	next_gadget

/* MSR TPIDR2_EL0, Xt. Stream: [Xt offset]. */
gadget sme_tpidr2_write_r
	ldr x8, [_stream], #8
	ldr x9, [_vcpu, x8]
	str x9, [_vcpu, #UML_ARM64_CPU_TPIDR2]
	next_gadget

gadget sme_tpidr2_write_z
	str xzr, [_vcpu, #UML_ARM64_CPU_TPIDR2]
	next_gadget

/* RDSVL Xd, #imm: imm x SVL in bytes, read from the vCPU at run time.
 * Stream: [Xd offset][imm, sign-extended]. */
gadget sme_rdsvl
	ldp x8, x10, [_stream], #16
	ldr w9, [_vcpu, #UML_ARM64_CPU_SME_SVL]
	mul x9, x9, x10
	str x9, [_vcpu, x8]
	next_gadget

/* ADDSVL and ADDSPL: Xd|SP = Xn|SP + imm x SVL (or SVL / 8). Stream:
 * [Xd offset | Xn offset << 16][imm, sign-extended]. */
.macro sme_add_vector_length name, shift
gadget sme_\name
	ldp x8, x10, [_stream], #16
	ubfx x11, x8, #16, #16
	and x8, x8, #0xffff
	ldr w9, [_vcpu, #UML_ARM64_CPU_SME_SVL]
	.if \shift
	lsr w9, w9, #\shift
	.endif
	ldr x12, [_vcpu, x11]
	madd x9, x9, x10, x12
	str x9, [_vcpu, x8]
	next_gadget
.endm
sme_add_vector_length addsvl, 0
sme_add_vector_length addspl, 3
.purgem sme_add_vector_length

/* A write of SVCR: SMSTART, SMSTOP or MSR SVCR. It ends the block, so the
 * next one runs translated for the new mode. Stream: [mask | value << 8] or
 * [Xt offset], then [instruction PC]. The helper leaves the native FP
 * environment, applies the write's architectural effects and empties the
 * return ring, whose records name the old mode's code; with no task storage
 * it changes nothing and the exit asks the kernel for storage. */
.macro sme_svcr_write name, from_register
gadget sme_svcr_write_\name
	bl uml_arm64_gadget_sme_suspend
	ldr x8, [_stream], #8
	.if \from_register
	ldr x8, [_vcpu, x8]
	and x8, x8, #(UML_ARM64_SVCR_SM | UML_ARM64_SVCR_ZA)
	lsl x8, x8, #UML_GADGET_SME_SVCR_VALUE_SHIFT
	orr x8, x8, #(UML_ARM64_SVCR_SM | UML_ARM64_SVCR_ZA)
	.endif
	mov x19, x1
	mov x0, x1
	mov x1, x8
	helper_call uml_arm64_gadget_sme_write_svcr
	mov x1, x19
	ldr x8, [_stream]
	cmp w0, #UML_GADGET_SME_SVCR_WRITTEN
	b.ne 1f
	add x8, x8, #4
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
1:
	str _stream, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	cmp w0, #UML_GADGET_SME_SVCR_NEEDS_STORAGE
	mov w0, #UML_GADGET_EVENT_SME_ACCESS
	mov w9, #-1
	csel w0, w0, w9, eq
	b uml_arm64_gadget_leave
.endm
sme_svcr_write immediate, 0
sme_svcr_write register, 1
.purgem sme_svcr_write

/* LDR and STR ZA[Wv, #imm], [Xn|SP, #imm, MUL VL] with host ZA resident: the
 * array vector (Wv + imm) mod SVL, SVL bytes at Xn + imm x SVL. The native
 * instruction takes the slice from W15 and the host address of a same-page
 * access; anything else goes through the helper. Stream: [descriptor]
 * [instruction PC]. */
.macro sm_za_vector name, access
gadget sm_za_vector_\name
	ldr x14, [_stream], #8
	sme_base 2f
	ubfx x10, x14, #32, #4
	ldr w13, [_vcpu, #UML_ARM64_CPU_SME_SVL]
	madd x7, x10, x13, x7
	sme_untag
	ubfx x9, x14, #16, #16
	ldr w15, [_vcpu, x9]
	add w15, w15, w10
	sme_translate \access, x7, x13, 2f
	.if \access
	str za[w15, 0], [x7]
	.else
	ldr za[w15, 0], [x7]
	.endif
	add _stream, _stream, #8
	next_gadget
2:
	bl uml_arm64_gadget_sme_suspend
	sme_memory_helper 3f
	bl uml_arm64_gadget_sme_resume
	add _stream, _stream, #8
	next_gadget
3:
	b uml_arm64_gadget_memory_fault
.endm
sm_za_vector load, 0
sm_za_vector store, 1
.purgem sm_za_vector

/* LDR and STR ZT0, [Xn|SP]: 64 bytes. Stream as above. */
.macro sm_zt0 name, access
gadget sm_zt0_\name
	ldr x14, [_stream], #8
	sme_base 2f
	sme_untag
	mov x13, #UML_ARM64_SME_ZT0_BYTES
	sme_translate \access, x7, x13, 2f
	.if \access
	str zt0, [x7]
	.else
	ldr zt0, [x7]
	.endif
	add _stream, _stream, #8
	next_gadget
2:
	bl uml_arm64_gadget_sme_suspend
	sme_memory_helper 3f
	bl uml_arm64_gadget_sme_resume
	add _stream, _stream, #8
	next_gadget
3:
	b uml_arm64_gadget_memory_fault
.endm
sm_zt0 load, 0
sm_zt0 store, 1
.purgem sm_zt0

gadget sm_zt0_zero
	zero {zt0}
	next_gadget

/* ZERO { mask } with host ZA resident: one entry per tile mask, 16 bytes
 * apart, so the builder selects table + mask x 16. */
	.p2align 4
	.globl CSYM(uml_arm64_gadget_sm_za_zero_table)
CSYM(uml_arm64_gadget_sm_za_zero_table):
	.set Lsme_zero_mask, 0
	.rept 256
	.inst 0xc0080000 | Lsme_zero_mask
	next_gadget
	.p2align 4
	.set Lsme_zero_mask, Lsme_zero_mask + 1
	.endr
	.globl CSYM(uml_arm64_gadget_sm_za_zero_table_end)
CSYM(uml_arm64_gadget_sm_za_zero_table_end):

	.globl CSYM(uml_arm64_gadget_sme_streaming_end)
CSYM(uml_arm64_gadget_sme_streaming_end):

/* Every one of those accesses and zeroings with SVCR.SM = 0 and ZA = 1, when
 * guest ZA and ZT0 stay in the register file: the helper does them all.
 * Stream: [descriptor][instruction PC]. */
gadget nsm_za_memory
	add _stream, _stream, #8
	sme_memory_helper 3f
	add _stream, _stream, #8
	next_gadget
3:
	b uml_arm64_gadget_memory_fault

/* Streaming twins of the base GPR gadgets that streaming mode cannot run:
 * CSSC CNT's AdvSIMD count, and the memory gadgets. A base load, store, pair,
 * exclusive, atomic, MOPS or cache gadget runs GPR-only code on its hot path
 * but calls helpers on its TLB-miss, page-crossing and alignment paths, and
 * a few of those paths use AdvSIMD, so a streaming program cannot select it.
 * The files that hold those families are assembled here a second time with
 * Lsme_twin and UML_GADGET_SME_TWIN set, which keeps every hot path and
 * changes only:
 * - the names: each twin is uml_arm64_gadget_sm_ plus its base entry's name,
 *   with a row mapping one to the other, and the shared cold stubs become
 *   local copies;
 * - each helper call, which goes through a trampoline below that leaves
 *   streaming mode around it, and the slow paths' AdvSIMD words, which
 *   become FMOVs;
 * - nothing else: no forwarding variants, scalar contracts or SIMD&FP
 *   families are assembled again.
 * The twins sit in their own streaming row, so the safety test walks them. */

/* A helper call from a twin: CpuState is at SP + \cpu. */
.macro sme_helper_call symbol, cpu
	bl Lsme_trampoline_\cpu\()_\symbol
.endm

/* The trampoline of a helper, called where the twin's base gadget calls the
 * helper's veneer, with the same arguments and results in x0-x7: it leaves
 * streaming mode, spilling ZA and ZT0 when host ZA is on, calls the helper
 * and enters the guest's mode again. A twin's helper call whose trampoline
 * is missing here is an undefined label. */
.macro sme_trampoline symbol, cpu=0
Lsme_trampoline_\cpu\()_\symbol:
	stp x1, x30, [sp, #-16]!
	ldr x1, [sp, #(16 + \cpu)]
	bl uml_arm64_gadget_sme_suspend
	ldr x1, [sp]
	bl Lveneer_\symbol
	str x1, [sp]
	ldr x1, [sp, #(16 + \cpu)]
	bl uml_arm64_gadget_sme_resume
	ldp x1, x30, [sp], #16
	ret
.endm

	.p2align 4
	sme_trampoline uml_arm64_tlb_miss
	sme_trampoline uml_arm64_memory_transfer, 32
	sme_trampoline uml_arm64_alignment_fault
	sme_trampoline uml_arm64_guest_code_synchronize
	sme_trampoline uml_arm64_gadget_mops_prologue_transfer_copy_forward
	sme_trampoline uml_arm64_gadget_mops_prologue_transfer_copy_general
	sme_trampoline uml_arm64_gadget_mops_prologue_transfer_set
	sme_trampoline uml_arm64_gadget_mops_epilogue_transfer_copy_forward
	sme_trampoline uml_arm64_gadget_mops_epilogue_transfer_copy_general
	sme_trampoline uml_arm64_gadget_mops_epilogue_transfer_set_r
	sme_trampoline uml_arm64_gadget_mops_epilogue_transfer_set_z
.purgem sme_trampoline

/* Twin rows: entries [base, base + 4) run as [twin, twin + 4), the layout
 * of the FEAT_AFP twin table. */
.macro sme_twin_row base, twin
	.pushsection __DATA_CONST,__gadget_sme
	.quad \base, \base + 4, \twin, \twin + 4
	.popsection
.endm

.pushsection __DATA_CONST,__gadget_sme
.p2align 3
.globl CSYM(uml_arm64_sme_twin_begin)
CSYM(uml_arm64_sme_twin_begin):
.popsection

/* 1 while a twin has a base entry of the same name. */
.set Lsme_twin_of_base, 1

.purgem gadget
.macro gadget name
	.globl CSYM(uml_arm64_gadget_sm_\name)
	.p2align 4
CSYM(uml_arm64_gadget_sm_\name):
	.if Lsme_twin_of_base
	sme_twin_row CSYM(uml_arm64_gadget_\name), CSYM(uml_arm64_gadget_sm_\name)
	.endif
.endm

.purgem scalar_entry
.macro scalar_entry name, forward, family, destination, first, second, third, words
	gadget \name
.endm

.purgem scalar_variants
.macro scalar_variants forward, first, second, third, generator, arguments:vararg
.endm

	.p2align 4
	.globl CSYM(uml_arm64_gadget_sm_twins_begin)
CSYM(uml_arm64_gadget_sm_twins_begin):

/* CSSC CNT on a host without FEAT_CSSC: a GPR-only population count, where
 * the base entry counts with AdvSIMD. Stream: as the base entry's. */
.macro sme_population_count suffix, bits64
gadget bit_reverse_count_\suffix
	ldr x9, [_stream], #8
	lsr x11, x9, #16
	ubfx x10, x9, #0, #16
	ldr x12, [_vcpu, x10]
	.if !\bits64
	mov w12, w12
	.endif
	lsr x13, x12, #1
	and x13, x13, #0x5555555555555555
	sub x12, x12, x13
	and x13, x12, #0x3333333333333333
	lsr x12, x12, #2
	and x12, x12, #0x3333333333333333
	add x12, x12, x13
	add x12, x12, x12, lsr #4
	and x12, x12, #0x0f0f0f0f0f0f0f0f
	mov x13, #0x0101010101010101
	mul x12, x12, x13
	lsr x0, x12, #56
	str x0, [_vcpu, x11]
	next_gadget
.endm
sme_population_count w_cnt, 0
sme_population_count x_cnt, 1
.purgem sme_population_count

/* MSR FPCR. While a streaming program runs, the guest's FPCR is the host's
 * too (uml_arm64_gadget_enter_streaming installs it) but its FPSR never is:
 * XNU resets FPSR on every kernel entry while a thread is in streaming mode,
 * so the guest's stays in CpuState. A write updates both copies of FPCR, and
 * takes no exit for a trap enable, since floating-point exceptions never
 * trap in streaming mode. Stream: as the base entries'. */
.macro sme_fpcr_write name, mask
gadget \name
	ldp x8, x13, [_stream], #16
	ldr w9, [_vcpu, x8]
	mov w10, #((\mask) & 0xffff)
	movk w10, #((\mask) >> 16), lsl #16
	and w9, w9, w10
	str w9, [_vcpu, #UML_GADGET_CPU_FPCR]
	mov w12, #UML_ARM64_FPCR_TRAP_ENABLES
	bic w10, w9, w12
	msr fpcr, x10
	next_gadget
.endm
sme_fpcr_write sysreg_fpcr_write_r, UML_ARM64_FPCR_WRITABLE
sme_fpcr_write sysreg_fpcr_write_r_afp, (UML_ARM64_FPCR_WRITABLE | UML_ARM64_FPCR_AFP_CONTROLS)
.purgem sme_fpcr_write

gadget sysreg_fpcr_write_z
	str wzr, [_vcpu, #UML_GADGET_CPU_FPCR]
	msr fpcr, xzr
	next_gadget

#define UML_GADGET_SME_TWIN 1
#undef GADGET_SECTION_COLD
#define GADGET_SECTION_COLD GADGET_SECTION_SME
#undef GADGET_SECTION_FORWARD
#define GADGET_SECTION_FORWARD GADGET_SECTION_SME
#define uml_arm64_gadget_memory_fault Lsm_memory_fault
#define uml_arm64_gadget_memory_fault_indexed Lsm_memory_fault_indexed
#define Lscalar_alignment_fault_read_indexed Lsm_scalar_alignment_fault_read_indexed
#define Lscalar_alignment_fault_write_indexed Lsm_scalar_alignment_fault_write_indexed
#define uml_arm64_gadget_pair_alignment_fault_read_indexed Lsm_pair_alignment_fault_read_indexed
#define uml_arm64_gadget_pair_alignment_fault_write_indexed Lsm_pair_alignment_fault_write_indexed
#define uml_arm64_gadget_atomic_alignment_fault_ Lsm_atomic_alignment_fault_
#define uml_arm64_gadget_atomic_alignment_fault_read Lsm_atomic_alignment_fault_read
#define uml_arm64_gadget_atomic_alignment_fault_write Lsm_atomic_alignment_fault_write
#define uml_arm64_gadget_lse2_alignment_ Lsm_lse2_alignment_
#define uml_arm64_gadget_lse2_alignment_read_ Lsm_lse2_alignment_read_
#define uml_arm64_gadget_lse2_alignment_write_ Lsm_lse2_alignment_write_
#define Llse2_stub_ Lsm_lse2_stub_
#define Llse2_resume_ Lsm_lse2_resume_
.set Lsme_twin, 1
#undef EXCL
#define EXCL(name) Lsm_excl_##name

#include "system_atomic.inc"
#include "load_store.inc"

/* A word or doubleword transfer of a register runs in the base bank as the
 * in-place entry that bakes the register, but its stream word names the
 * register too, so all 31 of those entries share one unbaked twin. */
.set Lsme_twin_of_base, 0
scalar_unsigned_fast 0, 0, 0, 0, store32_unsigned_fast_r
scalar_unsigned_fast 0, 0, 0, 1, store32_unsigned_fast_r_sp
scalar_unsigned_fast 0, 1, 0, 0, store64_unsigned_fast_r
scalar_unsigned_fast 0, 1, 0, 1, store64_unsigned_fast_r_sp
scalar_unsigned_fast 1, 0, 0, 0, load32_unsigned_fast_r
scalar_unsigned_fast 1, 0, 0, 1, load32_unsigned_fast_r_sp
scalar_unsigned_fast 1, 1, 0, 0, load64_unsigned_fast_r
scalar_unsigned_fast 1, 1, 0, 1, load64_unsigned_fast_r_sp
.set Lsme_twin_of_base, 1
.purgem scalar_unsigned_fast
.purgem sp_alignment_check

.macro sme_inplace_twins prefix, twin
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	sme_twin_row CSYM(uml_arm64_gadget_\prefix\()_r_\reg), CSYM(uml_arm64_gadget_sm_\twin\()_r)
	sme_twin_row CSYM(uml_arm64_gadget_\prefix\()_r_sp_\reg), CSYM(uml_arm64_gadget_sm_\twin\()_r_sp)
	.endr
.endm
sme_inplace_twins store32_unsigned_inplace, store32_unsigned_fast
sme_inplace_twins store64_unsigned_inplace, store64_unsigned_fast
sme_inplace_twins load32_unsigned_inplace, load32_unsigned_fast
sme_inplace_twins load64_unsigned_inplace, load64_unsigned_fast
.purgem sme_inplace_twins

#include "lse2_alignment.inc"

.set Lsme_twin, 0
#undef EXCL
#define EXCL(name) Lexcl_##name
#undef Llse2_resume_
#undef Llse2_stub_
#undef uml_arm64_gadget_lse2_alignment_write_
#undef uml_arm64_gadget_lse2_alignment_read_
#undef uml_arm64_gadget_lse2_alignment_
#undef uml_arm64_gadget_atomic_alignment_fault_write
#undef uml_arm64_gadget_atomic_alignment_fault_read
#undef uml_arm64_gadget_atomic_alignment_fault_
#undef uml_arm64_gadget_pair_alignment_fault_write_indexed
#undef uml_arm64_gadget_pair_alignment_fault_read_indexed
#undef Lscalar_alignment_fault_write_indexed
#undef Lscalar_alignment_fault_read_indexed
#undef uml_arm64_gadget_memory_fault_indexed
#undef uml_arm64_gadget_memory_fault
#undef GADGET_SECTION_FORWARD
#define GADGET_SECTION_FORWARD __TEXT,__a64_forward,regular,pure_instructions
#undef GADGET_SECTION_COLD
#define GADGET_SECTION_COLD __TEXT,__a64_cold,regular,pure_instructions
#undef UML_GADGET_SME_TWIN

	.globl CSYM(uml_arm64_gadget_sm_twins_end)
CSYM(uml_arm64_gadget_sm_twins_end):

.pushsection __DATA_CONST,__gadget_sme
.globl CSYM(uml_arm64_sme_twin_end)
CSYM(uml_arm64_sme_twin_end):
.popsection

/* The register-only SVE, SVE2 and SME forms and the FP&SIMD classes legal in
 * streaming mode, which engine/generate_streaming.py writes at build time
 * from the encoding table: gadgets between uml_arm64_gadget_sm_generated_begin
 * and _end, in their own streaming row, and the table of their entries the
 * builder selects from. */
#include "streaming_generated.inc"

/* The vector memory gadgets: the SVE and SME loads and stores, and the
 * SIMD&FP ones streaming mode keeps, also generated, one per instruction
 * the host runs for them (streaming_memory.inc). Each takes the stream
 * sme_layout.h describes. The fast path forms the address, checks that the
 * bytes the access spans lie in one guest page the TLB holds for it, and
 * runs the native instruction on host registers loaded from the register
 * file. A crossing, a miss or a misaligned SP base takes the slow path,
 * whose helper does the whole access on the register file: it reads the
 * governing predicate, touches only the active elements, translates every
 * page they lie in before it moves a byte, and reports a fault as the
 * hardware does. */

/* The guest address of the access in x7, translated through the TLB for
 * \access, or a branch to \slow. x14 and x15 hold stream words 0 and 1.
 * Scratch: x8-x13. */
.macro sm_vector_address access, slow
	and x9, x14, #0xffff
	ldr x7, [_vcpu, x9]
	tbz x14, #UML_GADGET_SME_VM_BASE_SP_BIT, 1f
	tst x7, #0xf
	b.ne \slow
1:
	tbnz x14, #UML_GADGET_SME_VM_POST_BIT, 2f
	add x7, x7, x15
2:
	tbz x14, #UML_GADGET_SME_VM_INDEX_BIT, 4f
	ubfx x9, x14, #16, #16
	ldr x10, [_vcpu, x9]
	tbz x14, #UML_GADGET_SME_VM_WORD_BIT, 3f
	tbnz x14, #UML_GADGET_SME_VM_SIGNED_BIT, 5f
	mov w10, w10
	b 3f
5:
	sxtw x10, w10
3:
	ubfx x11, x14, #UML_GADGET_SME_VM_SHIFT_SHIFT, #3
	lsl x10, x10, x11
	add x7, x7, x10
4:
	tbnz x14, #UML_GADGET_SME_VM_BASE_SP_BIT, 6f
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, 6f
	ubfx x7, x7, #0, #56
6:
	ubfx x13, x14, #UML_GADGET_SME_VM_FOOTPRINT_SHIFT, #16
	sme_translate \access, x7, x13, \slow
.endm

/* The base written back with base + immediate, after the access. */
.macro sm_vector_writeback
	tbz x14, #UML_GADGET_SME_VM_WRITEBACK_BIT, 7f
	and x9, x14, #0xffff
	ldr x10, [_vcpu, x9]
	add x10, x10, x15
	str x10, [_vcpu, x9]
7:
.endm

/* The slow path, with _stream at stream word 0: the helper does the access
 * with the host out of streaming mode, then the gadget continues at the
 * next instruction, or leaves through the memory-fault exit, whose PC word
 * _stream then addresses. */
.macro sm_vector_slow
	add _stream, _stream, #32
	bl uml_arm64_gadget_sme_suspend
	mov x19, x1
	mov x20, x2
	mov x0, x1
	mov x1, x2
	sub x2, _stream, #32
	helper_call uml_arm64_gadget_sme_vector_memory
	mov x1, x19
	mov x2, x20
	cbz w0, 9f
	bl uml_arm64_gadget_sme_resume
	add _stream, _stream, #8
	next_gadget
9:
	b uml_arm64_gadget_memory_fault
.endm

.set Lsme_twin_of_base, 0
#include "streaming_memory.inc"
.set Lsme_twin_of_base, 1
.purgem sm_vector_slow
.purgem sm_vector_writeback
.purgem sm_vector_address

.popsection

.arch_extension nosme-i16i64
.arch_extension nosme-f64f64
.arch_extension nosme2
.arch_extension nosme
