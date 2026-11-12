/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DC ZVA is fixed at 64 bytes by the virtual DCZID_EL0. Translation selects
 * the real-register and XZR forms, and the 64-byte alignment guarantees that
 * one successful guest-page TLB lookup covers every store. Stream:
 * [register byte offset, when present][guest PC]. */

.macro cache_zero name, zero
gadget cache_zero_\name
	.if \zero
	mov x7, xzr
	.else
	ldr x8, [_stream], #8
	ldr x7, [_vcpu, x8]
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, Lcache_zero_untagged\@
	ubfx x7, x7, #0, #56
Lcache_zero_untagged\@:
	and x7, x7, #0xffffffffffffffc0
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE
	stp xzr, xzr, [x7]
	stp xzr, xzr, [x7, #16]
	stp xzr, xzr, [x7, #32]
	stp xzr, xzr, [x7, #48]
	add _stream, _stream, #8
	next_gadget
.endm

cache_zero r, 0
cache_zero z, 1

.purgem cache_zero

/*
 * The guest instruction-cache synchronization point is deliberately a cold
 * execution boundary.  The helper publishes a translated-code sequence
 * change, then the executor resumes at the following guest instruction so
 * no stale suffix from the current data-only program can execute.
 * Stream: [register byte offset, when present][fallthrough guest PC].
 */
.macro code_synchronize name, zero
gadget code_synchronize_\name
	.if \zero
	mov x0, xzr
	.else
	ldr x8, [_stream], #8
	ldr x0, [_vcpu, x8]
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, Lcode_synchronize_untagged\@
	ubfx x0, x0, #0, #56
Lcode_synchronize_untagged\@:
	.endif
	stp x1, x2, [sp, #-16]!
	helper_call uml_arm64_guest_code_synchronize
	ldp x1, x2, [sp], #16
	ldr x8, [_stream]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
.endm

code_synchronize r, 0
code_synchronize z, 1

.purgem code_synchronize

/* FEAT_MOPS uses architectural option B. Translation specializes the family
 * and stage. Prologue formatting has no fault path; main is the ordinary nop
 * gadget; epilogues use the largest permitted 64-byte, page-contained host
 * bulk operations and retain only fault and malformed-stage-state branches. */
.macro mops_prologue name
gadget mops_prologue_\name
	ldr x8, [_stream], #8
	software_helper gadget_mops_prologue_transfer_\name
	ldr w27, [_vcpu, #UML_GADGET_CPU_NZCV]
	next_gadget
.endm

.macro mops_epilogue name
gadget mops_epilogue_\name
	ldr x8, [_stream], #8
	stp x1, x2, [sp, #-16]!
	str w27, [_vcpu, #UML_GADGET_CPU_NZCV]
	mov x0, x1
	mov x1, x2
	mov x2, x8
	helper_call uml_arm64_gadget_mops_epilogue_transfer_\name
	ldp x1, x2, [sp], #16
	ldr w27, [_vcpu, #UML_GADGET_CPU_NZCV]
	cbz w0, uml_arm64_gadget_memory_fault
	tbz w0, #1, 1f
	b CSYM(uml_arm64_gadget_illegal)
1:
	add _stream, _stream, #8
	next_gadget
.endm

mops_prologue copy_forward
mops_prologue copy_general
mops_prologue set
mops_epilogue copy_forward
mops_epilogue copy_general
mops_epilogue set_r
mops_epilogue set_z

/* engine/sme.inc assembles this file a second time with
 * UML_GADGET_SME_TWIN set, as the streaming twins of its memory families.
 * The system-register, PSTATE and barrier gadgets are shared by both modes
 * and the SIMD&FP structure families have no such twin, so this block has
 * none. */
#if !UML_GADGET_SME_TWIN
/* FEAT_ECV's self-synchronized virtual count, selected only on a host that
 * implements it. It occupies the head of the padding below, so no later
 * entry moves. */
gadget sysreg_cntvctss_read
	ldr x8, [_stream], #8
	mrs x9, s3_3_c14_c0_6	/* CNTVCTSS_EL0 */
	str x9, [_vcpu, x8]
	next_gadget

gadget pc_relative
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	str x9, [_vcpu, x8]
	next_gadget

/* Translation-time-specialized EL0 system-register accesses.  Operands are
 * direct byte offsets into CpuState::registers, and XZR cases use separate
 * gadgets (or NOP for discarded reads), leaving no register-number or opcode
 * branches in the production hot path. */
.macro sysreg_read64 name, field
gadget sysreg_\name\()_read
	ldr x8, [_stream], #8
	ldr x9, [_vcpu, #\field]
	str x9, [_vcpu, x8]
	next_gadget
.endm

.macro sysreg_write64 name, field
gadget sysreg_\name\()_write_r
	ldr x8, [_stream], #8
	ldr x9, [_vcpu, x8]
	str x9, [_vcpu, #\field]
	next_gadget
gadget sysreg_\name\()_write_z
	str xzr, [_vcpu, #\field]
	next_gadget
.endm

sysreg_read64 tls, UML_GADGET_CPU_TLS
sysreg_write64 tls, UML_GADGET_CPU_TLS

gadget sysreg_nzcv_read
	ldr x8, [_stream], #8
	str x27, [_vcpu, x8]
	next_gadget

.purgem sysreg_write64
.purgem sysreg_read64

/* The FEAT_AFP twins of the FPCR accesses also keep FIZ, AH and NEP. */
.macro sysreg_fpcr_read mask, afp=0
.if \afp
afp_gadget sysreg_fpcr_read
.else
gadget sysreg_fpcr_read
.endif
	ldr x8, [_stream], #8
	ldr w9, [_vcpu, #UML_GADGET_CPU_FPCR]
	mov w10, #((\mask) & 0xffff)
	movk w10, #((\mask) >> 16), lsl #16
	and w9, w9, w10
	str x9, [_vcpu, x8]
	next_gadget
.endm

sysreg_fpcr_read UML_ARM64_FPCR_WRITABLE
afp_twins_begin
sysreg_fpcr_read (UML_ARM64_FPCR_WRITABLE | UML_ARM64_FPCR_AFP_CONTROLS), 1
afp_twins_end
.purgem sysreg_fpcr_read

gadget sysreg_dczid_read
	ldr x8, [_stream], #8
	mov x9, #4
	str x9, [_vcpu, x8]
	next_gadget

gadget sysreg_zero_read
	ldr x8, [_stream], #8
	str xzr, [_vcpu, x8]
	next_gadget

gadget sysreg_ctr_read
	ldr x8, [_stream], #8
	mov w9, #0x8004
	movk w9, #0x8444, lsl #16
	str x9, [_vcpu, x8]
	next_gadget

gadget sysreg_cntfrq_read
	ldr x8, [_stream], #8
	mrs x9, cntfrq_el0
	str x9, [_vcpu, x8]
	next_gadget

gadget sysreg_cntvct_read
	ldr x8, [_stream], #8
	mrs x9, cntvct_el0
	str x9, [_vcpu, x8]
	next_gadget

gadget sysreg_fpsr_read
	ldr x8, [_stream], #8
	ldrb w11, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz x11, 8f
	mrs x9, fpsr
	b 9f
8:	ldr w9, [_vcpu, #UML_GADGET_CPU_FPSR]
9:
	mov w10, #(UML_ARM64_FPSR_WRITABLE & 0xffff)
	movk w10, #(UML_ARM64_FPSR_WRITABLE >> 16), lsl #16
	and w9, w9, w10
	str w9, [_vcpu, #UML_GADGET_CPU_FPSR]
	str x9, [_vcpu, x8]
	next_gadget

.macro sysreg_fp_write name, field, system, mask, afp=0
.if \afp
afp_gadget sysreg_\name\()_write_r
.else
gadget sysreg_\name\()_write_r
.endif
	ldr x8, [_stream], #8
	.ifc \name,fpcr
	ldr x13, [_stream], #8
	mov w12, #UML_ARM64_FPCR_TRAP_ENABLES
	.endif
	ldr w9, [_vcpu, x8]
	mov w10, #((\mask) & 0xffff)
	movk w10, #((\mask) >> 16), lsl #16
	and w9, w9, w10
	str w9, [_vcpu, #\field]
	ldrb w11, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz x11, 8f
	.ifc \name,fpcr
	bic w10, w9, w12
	msr \system, x10
	.else
	msr \system, x9
	.endif
8:
	.ifc \name,fpcr
	tst w9, w12
	b.eq 9f
	str _stream, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x13, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave
9:
	.endif
	next_gadget
	.if !\afp
gadget sysreg_\name\()_write_z
	str wzr, [_vcpu, #\field]
	ldrb w11, [_vcpu, #UML_GADGET_CPU_FP_ACTIVE]
	cbz x11, 9f
	msr \system, xzr
9:
	next_gadget
	.endif
.endm

sysreg_fp_write fpcr, UML_GADGET_CPU_FPCR, fpcr, UML_ARM64_FPCR_WRITABLE
sysreg_fp_write fpsr, UML_GADGET_CPU_FPSR, fpsr, UML_ARM64_FPSR_WRITABLE
afp_twins_begin
sysreg_fp_write fpcr, UML_GADGET_CPU_FPCR, fpcr, (UML_ARM64_FPCR_WRITABLE | UML_ARM64_FPCR_AFP_CONTROLS), 1
afp_twins_end

.purgem sysreg_fp_write

gadget sysreg_nzcv_write_r
	ldr x8, [_stream], #8
	ldr w9, [_vcpu, x8]
	and w9, w9, #0xf0000000
	mov w27, w9
	next_gadget

gadget sysreg_nzcv_write_z
	mov w27, wzr
	next_gadget

/* DIT is PSTATE.DIT presented as a system register: the architecture defines
 * the read as Zeros(39):PSTATE.DIT:Zeros(24) and the write as
 * PSTATE.DIT = X[t]<24>, so bit 24 is the only live position in either
 * direction.  Go's runtime reads it through MRS whenever HWCAP_DIT is
 * advertised, so the register form has to exist wherever the MSR immediate
 * form does. */
gadget sysreg_dit_read
	ldr x8, [_stream], #8
	ldrb w9, [_vcpu, #UML_GADGET_CPU_PSTATE_DIT]
	lsl x9, x9, #24
	str x9, [_vcpu, x8]
	next_gadget

gadget sysreg_dit_write_r
	ldr x8, [_stream], #8
	ldr x9, [_vcpu, x8]
	ubfx x9, x9, #24, #1
	strb w9, [_vcpu, #UML_GADGET_CPU_PSTATE_DIT]
	next_gadget

gadget sysreg_dit_write_z
	strb wzr, [_vcpu, #UML_GADGET_CPU_PSTATE_DIT]
	next_gadget

.macro pstate_store name, offset, value
gadget pstate_\name\()_\value
.if \value
	mov w8, #1
	strb w8, [_vcpu, #\offset]
.else
	strb wzr, [_vcpu, #\offset]
.endif
	next_gadget
.endm

.macro pstate_store_field name, offset
	pstate_store \name, \offset, 0
	pstate_store \name, \offset, 1
.endm

pstate_store_field dit, UML_GADGET_CPU_PSTATE_DIT

.purgem pstate_store_field
.purgem pstate_store

.macro pstate_daif name, operation, immediate
gadget pstate_daif_\name\()_\immediate
	ldrb w9, [_vcpu, #UML_GADGET_CPU_PSTATE_DAIF_ACCESS]
	cbz w9, CSYM(uml_arm64_gadget_illegal)
.if \immediate
	ldrb w9, [_vcpu, #UML_GADGET_CPU_PSTATE_DAIF]
	mov w10, #\immediate
	\operation w9, w9, w10
	strb w9, [_vcpu, #UML_GADGET_CPU_PSTATE_DAIF]
.endif
	/* The following inline word is the fault PC used only by the gated path. */
	add _stream, _stream, #8
	next_gadget
.endm

.macro pstate_daif_immediate immediate
	pstate_daif set, orr, \immediate
	pstate_daif clear, bic, \immediate
.endm

pstate_daif_immediate 0
pstate_daif_immediate 1
pstate_daif_immediate 2
pstate_daif_immediate 3
pstate_daif_immediate 4
pstate_daif_immediate 5
pstate_daif_immediate 6
pstate_daif_immediate 7
pstate_daif_immediate 8
pstate_daif_immediate 9
pstate_daif_immediate 10
pstate_daif_immediate 11
pstate_daif_immediate 12
pstate_daif_immediate 13
pstate_daif_immediate 14
pstate_daif_immediate 15

.purgem pstate_daif_immediate
.purgem pstate_daif

/* Barrier option is selected during translation, so each hot entry executes
 * exactly one native barrier and the threaded dispatch. */
.macro memory_barrier kind, option
gadget \kind\()_\option
	\kind #\option
	next_gadget
.endm

.macro memory_barrier_option option
	memory_barrier dmb, \option
	memory_barrier dsb, \option
.endm

memory_barrier_option 0
memory_barrier_option 1
memory_barrier_option 2
memory_barrier_option 3
memory_barrier_option 4
memory_barrier_option 5
memory_barrier_option 6
memory_barrier_option 7
memory_barrier_option 8
memory_barrier_option 9
memory_barrier_option 10
memory_barrier_option 11
memory_barrier_option 12
memory_barrier_option 13
memory_barrier_option 14
memory_barrier_option 15

.purgem memory_barrier_option
.purgem memory_barrier

/* ISB, a context synchronization point. A translation and the links out of
 * it are current as of the code-change sequence its executor validated it at
 * (CpuState::code_sequence_seen); when the address space's sequence has moved
 * since -- another vCPU changed code and invalidated it -- the ISB leaves at
 * its fall-through, and the executor revalidates and relinks before the next
 * instruction runs, as a refetch would. Leaving from inside the block gives
 * back the instructions translated after the ISB, which entering it
 * precharged. Stream: [instructions after this one][fall-through guest PC]. */
gadget isb
	ldr x8, [_vcpu, #UML_ARM64_CPU_CODE_SEQUENCE]
	ldr x9, [_vcpu, #UML_ARM64_CPU_CODE_SEQUENCE_SEEN]
	isb
	ldr x8, [x8]
	cmp x8, x9
	b.ne 1f
	add _stream, _stream, #16
	next_gadget
1:
	ldp x10, x11, [_stream]
	sub x29, x29, x10
	str x11, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, wzr
	b uml_arm64_gadget_leave

gadget speculation_barrier
	/* Native SB; selected only after host-feature detection at translation. */
	.inst 0xd50330ff
	next_gadget

gadget speculation_barrier_fallback
	/* Stronger branch-free sequence for hosts without native SB. */
	dsb sy
	isb
	next_gadget

/* CLREX's CRm immediate is ignored, so all encodings share this branch-free
 * native implementation. Clear the contiguous software-monitor image with
 * three paired stores; the final pair also covers exclusive_size and padding.
 */
gadget clear_exclusive
	clrex
	add x8, _vcpu, #UML_GADGET_CPU_EXCLUSIVE_ADDRESS
	mov x9, #-1
	stp x9, xzr, [x8]
	stp xzr, xzr, [x8, #16]
	stp xzr, xzr, [x8, #32]
	next_gadget

gadget yield
	yield
	next_gadget

gadget csdb
	/* Consumption speculation barrier, encoded as HINT #20. */
	.inst 0xd503229f
	next_gadget

gadget dgh
	/* FEAT_DGH hosts execute this directly; earlier hosts treat it as a hint. */
	.inst 0xd50320df
	next_gadget

gadget clrbhb
	/* FEAT_CLRBHB hosts clear their branch history. Earlier hosts are required
	 * to execute this architectural HINT encoding as a NOP.
	 */
	.inst 0xd50322df
	next_gadget

/* Translation-specialized AdvSIMD multiple-structure transfers. The symbol
 * fixes direction, register count, contiguous versus interleaved layout,
 * arrangement, and writeback mode. The packed operand holds Rt in bits 4:0,
 * Rn's CpuState byte offset in 31:16, Rm's offset in 47:32, and the
 * instruction's position in its block, which a fault reports in place of a PC
 * word, in 63:56. Mode 0 has no writeback, mode 1 uses the architecturally
 * implied byte count, and mode 2 reads the post-index increment from Rm. */
.macro simd_structure_cpu_load count
	add x8, _vcpu, #UML_GADGET_CPU_V0
	and w9, w19, #31
	ldr q0, [x8, x9, lsl #4]
	.irp reg, 1, 2, 3
	.if \count > \reg
	add w9, w9, #1
	and w9, w9, #31
	ldr q\reg, [x8, x9, lsl #4]
	.endif
	.endr
.endm

.macro simd_structure_cpu_store count
	add x8, _vcpu, #UML_GADGET_CPU_V0
	and w9, w19, #31
	str q0, [x8, x9, lsl #4]
	.irp reg, 1, 2, 3
	.if \count > \reg
	add w9, w9, #1
	and w9, w9, #31
	str q\reg, [x8, x9, lsl #4]
	.endif
	.endr
.endm

.macro simd_structure_memory_load count, elements, q, size
	.if \elements == 1
		.if \q
	ldr q0, [x7]
			.irp reg, 1, 2, 3
			.if \count > \reg
	ldr q\reg, [x7, #(\reg * 16)]
			.endif
			.endr
		.else
	ldr d0, [x7]
			.irp reg, 1, 2, 3
			.if \count > \reg
	ldr d\reg, [x7, #(\reg * 8)]
			.endif
			.endr
		.endif
	.elseif \elements == 2
		.if \q
			.if \size == 0
	ld2 {v0.16b, v1.16b}, [x7]
			.elseif \size == 1
	ld2 {v0.8h, v1.8h}, [x7]
			.elseif \size == 2
	ld2 {v0.4s, v1.4s}, [x7]
			.else
	ld2 {v0.2d, v1.2d}, [x7]
			.endif
		.else
			.if \size == 0
	ld2 {v0.8b, v1.8b}, [x7]
			.elseif \size == 1
	ld2 {v0.4h, v1.4h}, [x7]
			.else
	ld2 {v0.2s, v1.2s}, [x7]
			.endif
		.endif
	.elseif \elements == 3
		.if \q
			.if \size == 0
	ld3 {v0.16b, v1.16b, v2.16b}, [x7]
			.elseif \size == 1
	ld3 {v0.8h, v1.8h, v2.8h}, [x7]
			.elseif \size == 2
	ld3 {v0.4s, v1.4s, v2.4s}, [x7]
			.else
	ld3 {v0.2d, v1.2d, v2.2d}, [x7]
			.endif
		.else
			.if \size == 0
	ld3 {v0.8b, v1.8b, v2.8b}, [x7]
			.elseif \size == 1
	ld3 {v0.4h, v1.4h, v2.4h}, [x7]
			.else
	ld3 {v0.2s, v1.2s, v2.2s}, [x7]
			.endif
		.endif
	.else
		.if \q
			.if \size == 0
	ld4 {v0.16b, v1.16b, v2.16b, v3.16b}, [x7]
			.elseif \size == 1
	ld4 {v0.8h, v1.8h, v2.8h, v3.8h}, [x7]
			.elseif \size == 2
	ld4 {v0.4s, v1.4s, v2.4s, v3.4s}, [x7]
			.else
	ld4 {v0.2d, v1.2d, v2.2d, v3.2d}, [x7]
			.endif
		.else
			.if \size == 0
	ld4 {v0.8b, v1.8b, v2.8b, v3.8b}, [x7]
			.elseif \size == 1
	ld4 {v0.4h, v1.4h, v2.4h, v3.4h}, [x7]
			.else
	ld4 {v0.2s, v1.2s, v2.2s, v3.2s}, [x7]
			.endif
		.endif
	.endif
.endm

.macro simd_structure_memory_store count, elements, q, size
	.if \elements == 1
		.if \q
	str q0, [x7]
			.irp reg, 1, 2, 3
			.if \count > \reg
	str q\reg, [x7, #(\reg * 16)]
			.endif
			.endr
		.else
	str d0, [x7]
			.irp reg, 1, 2, 3
			.if \count > \reg
	str d\reg, [x7, #(\reg * 8)]
			.endif
			.endr
		.endif
	.elseif \elements == 2
		.if \q
			.if \size == 0
	st2 {v0.16b, v1.16b}, [x7]
			.elseif \size == 1
	st2 {v0.8h, v1.8h}, [x7]
			.elseif \size == 2
	st2 {v0.4s, v1.4s}, [x7]
			.else
	st2 {v0.2d, v1.2d}, [x7]
			.endif
		.else
			.if \size == 0
	st2 {v0.8b, v1.8b}, [x7]
			.elseif \size == 1
	st2 {v0.4h, v1.4h}, [x7]
			.else
	st2 {v0.2s, v1.2s}, [x7]
			.endif
		.endif
	.elseif \elements == 3
		.if \q
			.if \size == 0
	st3 {v0.16b, v1.16b, v2.16b}, [x7]
			.elseif \size == 1
	st3 {v0.8h, v1.8h, v2.8h}, [x7]
			.elseif \size == 2
	st3 {v0.4s, v1.4s, v2.4s}, [x7]
			.else
	st3 {v0.2d, v1.2d, v2.2d}, [x7]
			.endif
		.else
			.if \size == 0
	st3 {v0.8b, v1.8b, v2.8b}, [x7]
			.elseif \size == 1
	st3 {v0.4h, v1.4h, v2.4h}, [x7]
			.else
	st3 {v0.2s, v1.2s, v2.2s}, [x7]
			.endif
		.endif
	.else
		.if \q
			.if \size == 0
	st4 {v0.16b, v1.16b, v2.16b, v3.16b}, [x7]
			.elseif \size == 1
	st4 {v0.8h, v1.8h, v2.8h, v3.8h}, [x7]
			.elseif \size == 2
	st4 {v0.4s, v1.4s, v2.4s, v3.4s}, [x7]
			.else
	st4 {v0.2d, v1.2d, v2.2d, v3.2d}, [x7]
			.endif
		.else
			.if \size == 0
	st4 {v0.8b, v1.8b, v2.8b, v3.8b}, [x7]
			.elseif \size == 1
	st4 {v0.4h, v1.4h, v2.4h, v3.4h}, [x7]
			.else
	st4 {v0.2s, v1.2s, v2.2s, v3.2s}, [x7]
			.endif
		.endif
	.endif
.endm

.macro simd_structure_fast name, load, count, elements, q, size, mode, base_sp
gadget simd_structure_\name
	ldr x19, [_stream], #8
	ubfx x24, x19, #16, #16
	ldr x20, [_vcpu, x24]
	.if \base_sp
	tst x20, #0xf
	b.ne Lsimd_structure_alignment\@
	.endif
	.if \base_sp == 0 || \mode != 0
	ldrb w8, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbnz w8, Lsimd_structure_tagged\@
	.endif
	branch_if_cross_page x20, (\count * (8 << \q)), x8, Lsimd_structure_slow\@
	mov x7, x20
Lsimd_structure_translate\@:
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	simd_structure_memory_load \count, \elements, \q, \size
	simd_structure_cpu_store \count
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	simd_structure_cpu_load \count
	simd_structure_memory_store \count, \elements, \q, \size
	.endif
	.if \mode == 1
	add x20, x20, #(\count * (8 << \q))
	str x20, [_vcpu, x24]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_vcpu, x8]
	add x20, x20, x8
	str x20, [_vcpu, x24]
	.endif
	next_gadget

	.if \base_sp == 0 || \mode != 0
Lsimd_structure_tagged\@:
	/* TBI strips the logical tag and rejoins the fast path. */
	branch_if_cross_page x20, (\count * (8 << \q)), x8, Lsimd_structure_slow\@
	ubfx x7, x20, #0, #56
	b Lsimd_structure_translate\@
	.endif

Lsimd_structure_slow\@:
	sub sp, sp, #80
	stp x1, x2, [sp, #64]
	.if \load
	mov x0, x1
	mov x1, x2
	mov x2, x20
	mov x3, sp
	mov x4, #(\count * (8 << \q))
	mov w5, #0
	helper_call uml_arm64_simd_structure_memory_transfer
	ldp x1, x2, [sp, #64]
	cbz w0, Lsimd_structure_fault\@
	mov x7, sp
	simd_structure_memory_load \count, \elements, \q, \size
	simd_structure_cpu_store \count
	.else
	mov x7, sp
	simd_structure_cpu_load \count
	simd_structure_memory_store \count, \elements, \q, \size
	mov x0, x1
	mov x1, x2
	mov x2, x20
	mov x3, sp
	mov x4, #(\count * (8 << \q))
	mov w5, #1
	helper_call uml_arm64_simd_structure_memory_transfer
	ldp x1, x2, [sp, #64]
	cbz w0, Lsimd_structure_fault\@
	.endif
	add sp, sp, #80
	.if \mode == 1
	add x20, x20, #(\count * (8 << \q))
	str x20, [_vcpu, x24]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_vcpu, x8]
	add x20, x20, x8
	str x20, [_vcpu, x24]
	.endif
	next_gadget

Lsimd_structure_fault\@:
	add sp, sp, #80
	b uml_arm64_gadget_memory_fault_indexed

	.if \base_sp
Lsimd_structure_alignment\@:
	mov x7, x20
		.if \load
	b uml_arm64_gadget_atomic_alignment_fault_read
		.else
	b uml_arm64_gadget_atomic_alignment_fault_write
		.endif
	.endif
.endm

.macro simd_structure_modes base, load, count, elements, q, size
	simd_structure_fast \base\()_0, \load, \count, \elements, \q, \size, 0, 0
	simd_structure_fast \base\()_1, \load, \count, \elements, \q, \size, 1, 0
	simd_structure_fast \base\()_2, \load, \count, \elements, \q, \size, 2, 0
	simd_structure_fast \base\()_s0, \load, \count, \elements, \q, \size, 0, 1
	simd_structure_fast \base\()_s1, \load, \count, \elements, \q, \size, 1, 1
	simd_structure_fast \base\()_s2, \load, \count, \elements, \q, \size, 2, 1
.endm

.macro simd_structure_directions base, count, elements, q, size
	simd_structure_modes \base\()_store, 0, \count, \elements, \q, \size
	simd_structure_modes \base\()_load, 1, \count, \elements, \q, \size
.endm

.macro simd_structure_contiguous count
	simd_structure_directions contig_d_\count, \count, 1, 0, 0
	simd_structure_directions contig_q_\count, \count, 1, 1, 0
.endm

simd_structure_contiguous 1
simd_structure_contiguous 2
simd_structure_contiguous 3
simd_structure_contiguous 4

.macro simd_structure_interleaved count
	simd_structure_directions interleaved_d_b_\count, \count, \count, 0, 0
	simd_structure_directions interleaved_d_h_\count, \count, \count, 0, 1
	simd_structure_directions interleaved_d_s_\count, \count, \count, 0, 2
	simd_structure_directions interleaved_q_b_\count, \count, \count, 1, 0
	simd_structure_directions interleaved_q_h_\count, \count, \count, 1, 1
	simd_structure_directions interleaved_q_s_\count, \count, \count, 1, 2
	simd_structure_directions interleaved_q_d_\count, \count, \count, 1, 3
.endm

simd_structure_interleaved 2
simd_structure_interleaved 3
simd_structure_interleaved 4

.purgem simd_structure_interleaved
.purgem simd_structure_contiguous
.purgem simd_structure_directions
.purgem simd_structure_modes
.purgem simd_structure_fast
.purgem simd_structure_memory_store
.purgem simd_structure_memory_load
.purgem simd_structure_cpu_store
.purgem simd_structure_cpu_load

/* Translation-specialized LD1R/LD2R/LD3R/LD4R. Element width, destination
 * count, vector width, and post-index mode are compile-time properties. */
.macro simd_replicate_one width, q
	.if \width == 1
	ldrb w9, [x7]
		.if \q
	dup v0.16b, w9
		.else
	dup v0.8b, w9
		.endif
	.elseif \width == 2
	ldrh w9, [x7]
		.if \q
	dup v0.8h, w9
		.else
	dup v0.4h, w9
		.endif
	.elseif \width == 4
	ldr w9, [x7]
		.if \q
	dup v0.4s, w9
		.else
	dup v0.2s, w9
		.endif
	.else
	ldr x9, [x7]
		.if \q
	dup v0.2d, x9
		.else
	fmov d0, x9
		.endif
	.endif
	and w9, w26, #31
	add x10, _vcpu, #UML_GADGET_CPU_V0
	add x10, x10, x9, lsl #4
	str q0, [x10]
.endm

/* Fill count consecutive registers from Rt, one element each from x7 on. */
.macro simd_replicate_transfer count, width, q
	and w26, w19, #31
	simd_replicate_one \width, \q
	.irp reg, 1, 2, 3
	.if \count > \reg
	add x7, x7, #\width
	add w26, w26, #1
	simd_replicate_one \width, \q
	.endif
	.endr
.endm

.macro simd_replicate_fast name, count, width, q, mode, base_sp
gadget simd_replicate_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #16, #16
	ldr x21, [_vcpu, x20]
	.if \base_sp
	tst x21, #0xf
	b.ne Lsimd_replicate_alignment\@
	.endif
	.if \base_sp == 0 || \mode != 0
	ldrb w8, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbnz w8, Lsimd_replicate_tagged\@
	.endif
	.if \count * \width > 1
	branch_if_cross_page x21, (\count * \width), x8, Lsimd_replicate_slow\@
	.endif
	mov x7, x21
Lsimd_replicate_translate\@:
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	simd_replicate_transfer \count, \width, \q
	.if \mode == 1
	add x21, x21, #(\count * \width)
	str x21, [_vcpu, x20]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_vcpu, x8]
	add x21, x21, x8
	str x21, [_vcpu, x20]
	.endif
	next_gadget

	.if \base_sp == 0 || \mode != 0
Lsimd_replicate_tagged\@:
	/* TBI strips the logical tag and rejoins the fast path. */
	.if \count * \width > 1
	branch_if_cross_page x21, (\count * \width), x8, Lsimd_replicate_slow\@
	.endif
	ubfx x7, x21, #0, #56
	b Lsimd_replicate_translate\@
	.endif

Lsimd_replicate_slow\@:
	sub sp, sp, #48
	stp x1, x2, [sp, #32]
	mov x0, x1
	mov x1, x2
	mov x2, x21
	mov x3, sp
	mov x4, #(\count * \width)
	mov w5, #0
	helper_call uml_arm64_simd_structure_memory_transfer
	ldp x1, x2, [sp, #32]
	cbz w0, Lsimd_replicate_fault\@
	mov x7, sp
	simd_replicate_transfer \count, \width, \q
	add sp, sp, #48
	.if \mode == 1
	add x21, x21, #(\count * \width)
	str x21, [_vcpu, x20]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_vcpu, x8]
	add x21, x21, x8
	str x21, [_vcpu, x20]
	.endif
	next_gadget

Lsimd_replicate_fault\@:
	add sp, sp, #48
	b uml_arm64_gadget_memory_fault_indexed

	.if \base_sp
Lsimd_replicate_alignment\@:
	mov x7, x21
	b uml_arm64_gadget_atomic_alignment_fault_read
	.endif
.endm

.macro simd_replicate_modes base, count, width, q
	simd_replicate_fast \base\()_0, \count, \width, \q, 0, 0
	simd_replicate_fast \base\()_1, \count, \width, \q, 1, 0
	simd_replicate_fast \base\()_2, \count, \width, \q, 2, 0
	simd_replicate_fast \base\()_s0, \count, \width, \q, 0, 1
	simd_replicate_fast \base\()_s1, \count, \width, \q, 1, 1
	simd_replicate_fast \base\()_s2, \count, \width, \q, 2, 1
.endm

.macro simd_replicate_counts type, width, q
	simd_replicate_modes \type\()_1, 1, \width, \q
	simd_replicate_modes \type\()_2, 2, \width, \q
	simd_replicate_modes \type\()_3, 3, \width, \q
	simd_replicate_modes \type\()_4, 4, \width, \q
.endm

simd_replicate_counts b_d, 1, 0
simd_replicate_counts b_q, 1, 1
simd_replicate_counts h_d, 2, 0
simd_replicate_counts h_q, 2, 1
simd_replicate_counts s_d, 4, 0
simd_replicate_counts s_q, 4, 1
simd_replicate_counts d_d, 8, 0
simd_replicate_counts d_q, 8, 1

.purgem simd_replicate_counts
.purgem simd_replicate_modes
.purgem simd_replicate_fast
.purgem simd_replicate_transfer
.purgem simd_replicate_one

/* Translation-specialized single-structure lane transfers. The lane byte
 * offset is precomputed into operand bits 55:48; width, register count,
 * direction, and writeback mode are fixed by the selected symbol. */
.macro simd_lane_one load, width
	and w9, w26, #31
	add x10, _vcpu, #UML_GADGET_CPU_V0
	add x10, x10, x9, lsl #4
	add x10, x10, x23
	.if \load
		.if \width == 1
	ldrb w9, [x7]
	strb w9, [x10]
		.elseif \width == 2
	ldrh w9, [x7]
	strh w9, [x10]
		.elseif \width == 4
	ldr w9, [x7]
	str w9, [x10]
		.else
	ldr x9, [x7]
	str x9, [x10]
		.endif
	.else
		.if \width == 1
	ldrb w9, [x10]
	strb w9, [x7]
		.elseif \width == 2
	ldrh w9, [x10]
	strh w9, [x7]
		.elseif \width == 4
	ldr w9, [x10]
	str w9, [x7]
		.else
	ldr x9, [x10]
	str x9, [x7]
		.endif
	.endif
.endm

.macro simd_lane_transfer load, count, width
	and w26, w19, #31
	simd_lane_one \load, \width
	.irp reg, 1, 2, 3
	.if \count > \reg
	add x7, x7, #\width
	add w26, w26, #1
	simd_lane_one \load, \width
	.endif
	.endr
.endm

.macro simd_lane_fast name, load, count, width, mode, base_sp
gadget simd_lane_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #16, #16
	ubfx x23, x19, #48, #8
	ldr x21, [_vcpu, x20]
	.if \base_sp
	tst x21, #0xf
	b.ne Lsimd_lane_alignment\@
	.endif
	.if \base_sp == 0 || \mode != 0
	ldrb w8, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbnz w8, Lsimd_lane_tagged\@
	.endif
	.if \count * \width > 1
	branch_if_cross_page x21, (\count * \width), x8, Lsimd_lane_slow\@
	.endif
	mov x7, x21
Lsimd_lane_translate\@:
	.if \load
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	simd_lane_transfer \load, \count, \width
	.if \mode == 1
	add x21, x21, #(\count * \width)
	str x21, [_vcpu, x20]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_vcpu, x8]
	add x21, x21, x8
	str x21, [_vcpu, x20]
	.endif
	next_gadget

	.if \base_sp == 0 || \mode != 0
Lsimd_lane_tagged\@:
	/* TBI strips the logical tag and rejoins the fast path. */
	.if \count * \width > 1
	branch_if_cross_page x21, (\count * \width), x8, Lsimd_lane_slow\@
	.endif
	ubfx x7, x21, #0, #56
	b Lsimd_lane_translate\@
	.endif

Lsimd_lane_slow\@:
	sub sp, sp, #48
	stp x1, x2, [sp, #32]
	.if \load
	mov x0, x1
	mov x1, x2
	mov x2, x21
	mov x3, sp
	mov x4, #(\count * \width)
	mov w5, #0
	helper_call uml_arm64_simd_structure_memory_transfer
	ldp x1, x2, [sp, #32]
	cbz w0, Lsimd_lane_fault\@
	mov x7, sp
	simd_lane_transfer \load, \count, \width
	.else
	mov x7, sp
	simd_lane_transfer \load, \count, \width
	mov x0, x1
	mov x1, x2
	mov x2, x21
	mov x3, sp
	mov x4, #(\count * \width)
	mov w5, #1
	helper_call uml_arm64_simd_structure_memory_transfer
	ldp x1, x2, [sp, #32]
	cbz w0, Lsimd_lane_fault\@
	.endif
	add sp, sp, #48
	.if \mode == 1
	add x21, x21, #(\count * \width)
	str x21, [_vcpu, x20]
	.elseif \mode == 2
	ubfx x8, x19, #32, #16
	ldr x8, [_vcpu, x8]
	add x21, x21, x8
	str x21, [_vcpu, x20]
	.endif
	next_gadget

Lsimd_lane_fault\@:
	add sp, sp, #48
	b uml_arm64_gadget_memory_fault_indexed

	.if \base_sp
Lsimd_lane_alignment\@:
	mov x7, x21
		.if \load
	b uml_arm64_gadget_atomic_alignment_fault_read
		.else
	b uml_arm64_gadget_atomic_alignment_fault_write
		.endif
	.endif
.endm

.macro simd_lane_modes base, load, count, width
	simd_lane_fast \base\()_0, \load, \count, \width, 0, 0
	simd_lane_fast \base\()_1, \load, \count, \width, 1, 0
	simd_lane_fast \base\()_2, \load, \count, \width, 2, 0
	simd_lane_fast \base\()_s0, \load, \count, \width, 0, 1
	simd_lane_fast \base\()_s1, \load, \count, \width, 1, 1
	simd_lane_fast \base\()_s2, \load, \count, \width, 2, 1
.endm

.macro simd_lane_directions type, count, width
	simd_lane_modes \type\()_\count\()_store, 0, \count, \width
	simd_lane_modes \type\()_\count\()_load, 1, \count, \width
.endm

.macro simd_lane_counts type, width
	simd_lane_directions \type, 1, \width
	simd_lane_directions \type, 2, \width
	simd_lane_directions \type, 3, \width
	simd_lane_directions \type, 4, \width
.endm

simd_lane_counts b, 1
simd_lane_counts h, 2
simd_lane_counts s, 4
simd_lane_counts d, 8

.purgem simd_lane_counts
.purgem simd_lane_directions
.purgem simd_lane_modes
.purgem simd_lane_fast
.purgem simd_lane_transfer
.purgem simd_lane_one
#endif

/* The alignment check of a single-copy-atomic access of \mask + 1 bytes at
 * x7: an LSE atomic, an exclusive, or a load-acquire/store-release form.
 * The hot path keeps its one test and branch. A misaligned address leaves for
 * the gadget's stub in lse2_alignment.inc, which resumes at the label below
 * when the host implements FEAT_LSE2 and the access stays inside one 16-byte
 * block, and otherwise delivers the alignment fault. A misaligned 16-byte
 * access always crosses a block, so it faults directly. */
.macro single_copy_alignment name, mask, write
	tst x7, #\mask
	.if \mask == 15
		.if \write
	b.ne uml_arm64_gadget_atomic_alignment_fault_write
		.else
	b.ne uml_arm64_gadget_atomic_alignment_fault_read
		.endif
	.else
	b.ne Llse2_stub_\name
Llse2_resume_\name:
	.endif
.endm

/*
 * CheckSPAlignment is selected solely from Rn at translation time. Ordinary
 * GPR-base atomic gadgets retain their existing hot paths; SP forms receive
 * this small prefix. The selected memory gadget and packed operand, which
 * holds the instruction's position in its block, follow it, so a fault
 * consumes those two words before entering the shared fault exit. SIMD
 * structure gadgets fold this check into SP-specialized symbols to avoid an
 * additional threaded dispatch.
 */
.macro atomic_sp_alignment name
gadget atomic_sp_alignment_\name
	ldr x7, [_vcpu, #UML_GADGET_CPU_SP]
	tst x7, #0xf
	b.eq 1f
	/* _stream currently addresses the selected atomic gadget pointer,
	 * followed by its packed operand. */
	add _stream, _stream, #16
	b uml_arm64_gadget_atomic_alignment_fault_\name
1:
	next_gadget
.endm

atomic_sp_alignment read
atomic_sp_alignment write

.purgem atomic_sp_alignment

.p2align 4
uml_arm64_gadget_atomic_alignment_fault_read:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x7
	mov w2, #0
	helper_call uml_arm64_alignment_fault
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault_indexed

.p2align 4
uml_arm64_gadget_atomic_alignment_fault_write:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x7
	mov w2, #1
	helper_call uml_arm64_alignment_fault
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault_indexed

/*
 * FEAT_LSE scalar atomic read-modify-write operations. Width, operation,
 * ordering, source-ZR handling, and result-discard handling are all selected
 * while the threaded program is built. The valid hot path therefore contains
 * no opcode/ordering dispatch and maps one guest operation to one native host
 * LSE instruction. Stream: one packed operand, which ends in the
 * instruction's position in its block.
 */
.macro atomic_rmw_gadget name, instruction, host_register, alignment, source_zero, discard_result
gadget atomic_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_vcpu, x20]
	tbi_strip x7
	.if \alignment
	single_copy_alignment atomic_\name, \alignment, 1
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	.if \source_zero
	mov x15, xzr
	.else
	ubfx x20, x19, #16, #16
	ldr x15, [_vcpu, x20]
	.endif
	\instruction \host_register\()15, \host_register\()16, [x7]
	.if !\discard_result
	and x21, x19, #0xffff
	str x16, [_vcpu, x21]
	.endif
	next_gadget
.endm

.macro atomic_rmw_register_modes name, instruction, host_register, alignment
	atomic_rmw_gadget \name\()_rr, \instruction, \host_register, \alignment, 0, 0
	atomic_rmw_gadget \name\()_rz, \instruction, \host_register, \alignment, 0, 1
	atomic_rmw_gadget \name\()_zr, \instruction, \host_register, \alignment, 1, 0
	atomic_rmw_gadget \name\()_zz, \instruction, \host_register, \alignment, 1, 1
.endm

.macro atomic_rmw_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	atomic_rmw_register_modes \name\()_b, \byte_instruction, w, 0
	atomic_rmw_register_modes \name\()_h, \half_instruction, w, 1
	atomic_rmw_register_modes \name\()_w, \word_instruction, w, 3
	atomic_rmw_register_modes \name\()_x, \double_instruction, x, 7
.endm

.macro atomic_rmw_family operation
	atomic_rmw_sizes \operation\()_relaxed, \operation\()b, \operation\()h, \operation, \operation
	atomic_rmw_sizes \operation\()_release, \operation\()lb, \operation\()lh, \operation\()l, \operation\()l
	atomic_rmw_sizes \operation\()_acquire, \operation\()ab, \operation\()ah, \operation\()a, \operation\()a
	atomic_rmw_sizes \operation\()_acquire_release, \operation\()alb, \operation\()alh, \operation\()al, \operation\()al
.endm

atomic_rmw_family ldadd
atomic_rmw_family ldclr
atomic_rmw_family ldeor
atomic_rmw_family ldset
atomic_rmw_family ldsmax
atomic_rmw_family ldsmin
atomic_rmw_family ldumax
atomic_rmw_family ldumin
atomic_rmw_family swp

.purgem atomic_rmw_family
.purgem atomic_rmw_sizes
.purgem atomic_rmw_register_modes
.purgem atomic_rmw_gadget

/* FEAT_LSE CAS. As with scalar RMW operations, every encoding-constant
 * choice is made by the builder. The mode suffix describes compare/result and
 * replacement registers: r is a GPR and z is ZR/discard. */
.macro compare_swap_gadget name, instruction, host_register, alignment, compare_zero, replacement_zero
gadget compare_swap_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_vcpu, x20]
	tbi_strip x7
	.if \alignment
	single_copy_alignment compare_swap_\name, \alignment, 1
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	.if \compare_zero
	mov x15, xzr
	.else
	and x20, x19, #0xffff
	ldr x15, [_vcpu, x20]
	.endif
	.if \replacement_zero
	mov x16, xzr
	.else
	ubfx x21, x19, #16, #16
	ldr x16, [_vcpu, x21]
	.endif
	\instruction \host_register\()15, \host_register\()16, [x7]
	.if !\compare_zero
	str x15, [_vcpu, x20]
	.endif
	next_gadget
.endm

.macro compare_swap_register_modes name, instruction, host_register, alignment
	compare_swap_gadget \name\()_rr, \instruction, \host_register, \alignment, 0, 0
	compare_swap_gadget \name\()_rz, \instruction, \host_register, \alignment, 0, 1
	compare_swap_gadget \name\()_zr, \instruction, \host_register, \alignment, 1, 0
	compare_swap_gadget \name\()_zz, \instruction, \host_register, \alignment, 1, 1
.endm

.macro compare_swap_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	compare_swap_register_modes \name\()_b, \byte_instruction, w, 0
	compare_swap_register_modes \name\()_h, \half_instruction, w, 1
	compare_swap_register_modes \name\()_w, \word_instruction, w, 3
	compare_swap_register_modes \name\()_x, \double_instruction, x, 7
.endm

compare_swap_sizes relaxed, casb, cash, cas, cas
compare_swap_sizes release, caslb, caslh, casl, casl
compare_swap_sizes acquire, casab, casah, casa, casa
compare_swap_sizes acquire_release, casalb, casalh, casal, casal

.purgem compare_swap_sizes
.purgem compare_swap_register_modes
.purgem compare_swap_gadget

/* FEAT_LSE CASP. Pair starts are even, so only register 30's second
 * element can be ZR. That fact is specialized independently for the compare
 * and replacement pairs. */
.macro compare_swap_pair_gadget name, instruction, host_register, alignment, compare_second_zero, replacement_second_zero
gadget compare_swap_pair_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_vcpu, x20]
	tbi_strip x7
	single_copy_alignment compare_swap_pair_\name, \alignment, 1
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	and x20, x19, #0xffff
	ubfx x21, x19, #16, #16
	ldr x14, [_vcpu, x20]
	.if \compare_second_zero
	mov x15, xzr
	.else
	add x22, x20, #8
	ldr x15, [_vcpu, x22]
	.endif
	ldr x16, [_vcpu, x21]
	.if \replacement_second_zero
	mov x17, xzr
	.else
	add x23, x21, #8
	ldr x17, [_vcpu, x23]
	.endif
	\instruction \host_register\()14, \host_register\()15, \
		\host_register\()16, \host_register\()17, [x7]
	str x14, [_vcpu, x20]
	.if !\compare_second_zero
	str x15, [_vcpu, x22]
	.endif
	next_gadget
.endm

.macro compare_swap_pair_register_modes name, instruction, host_register, alignment
	compare_swap_pair_gadget \name\()_rr, \instruction, \host_register, \alignment, 0, 0
	compare_swap_pair_gadget \name\()_rz, \instruction, \host_register, \alignment, 0, 1
	compare_swap_pair_gadget \name\()_zr, \instruction, \host_register, \alignment, 1, 0
	compare_swap_pair_gadget \name\()_zz, \instruction, \host_register, \alignment, 1, 1
.endm

.macro compare_swap_pair_sizes name, word_instruction, double_instruction
	compare_swap_pair_register_modes \name\()_w, \word_instruction, w, 7
	compare_swap_pair_register_modes \name\()_x, \double_instruction, x, 15
.endm

compare_swap_pair_sizes relaxed, casp, casp
compare_swap_pair_sizes release, caspl, caspl
compare_swap_pair_sizes acquire, caspa, caspa
compare_swap_pair_sizes acquire_release, caspal, caspal

.purgem compare_swap_pair_sizes
.purgem compare_swap_pair_register_modes
.purgem compare_swap_pair_gadget

/* Exclusive load/store operations: LDXR, LDAXR, STXR and STLXR at each width,
 * and the pair forms LDXP, LDAXP, STXP and STLXP. The builder chooses load
 * vs store, width, ordering, and ZR/status-discard variants. engine/
 * exclusive.inc explains how they keep a load/store exclusive loop making
 * progress; this file only instantiates it. A load probes the TLB by its
 * write tag and records the software monitor; a store whose probe misses,
 * or whose host store fails after the executor entered translated code
 * again, finishes in software in the shared routine for its width and
 * ordering. The gadgets' stubs follow each gadget and the shared routines
 * follow the last one. The group keeps the footprint it had before the
 * mechanism, so no gadget after it moved. */
#include "exclusive.inc"

.set Lexcl_footprint, 11936
.set Lexcl_footprint_twin, 12192

	.p2align 4
EXCL(group):

.macro exclusive_load_gadget name, instruction, host_register, alignment, discard_result
gadget exclusive_load_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #16, #16
	ldr x7, [_vcpu, x20]
	tbi_strip x7
	.if \alignment
	single_copy_alignment exclusive_load_\name, \alignment, 0
	.endif
	exclusive_translate EXCL(load_miss_)\name
EXCL(load_resume_)\name:
	\instruction \host_register\()15, [x7]
	exclusive_record_load
	.if !\discard_result
	and x21, x19, #0xffff
	str x15, [_vcpu, x21]
	.endif
	next_gadget
EXCL(load_miss_)\name:
	adr x13, EXCL(load_resume_)\name
	b EXCL(load_miss)
.endm

.macro exclusive_load_modes name, instruction, host_register, alignment
	exclusive_load_gadget \name\()_r, \instruction, \host_register, \alignment, 0
	exclusive_load_gadget \name\()_z, \instruction, \host_register, \alignment, 1
.endm

.macro exclusive_load_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	exclusive_load_modes \name\()_b, \byte_instruction, w, 0
	exclusive_load_modes \name\()_h, \half_instruction, w, 1
	exclusive_load_modes \name\()_w, \word_instruction, w, 3
	exclusive_load_modes \name\()_x, \double_instruction, x, 7
.endm

exclusive_load_sizes relaxed, ldxrb, ldxrh, ldxr, ldxr
exclusive_load_sizes acquire, ldaxrb, ldaxrh, ldaxr, ldaxr

.purgem exclusive_load_sizes
.purgem exclusive_load_modes
.purgem exclusive_load_gadget

.macro exclusive_store_gadget name, instruction, host_register, alignment, source_zero, discard_status, complete, lost
gadget exclusive_store_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_vcpu, x20]
	tbi_strip x7
	.if \source_zero
	mov x15, xzr
	.else
	and x21, x19, #0xffff
	ldr x15, [_vcpu, x21]
	.endif
	.if \alignment
	single_copy_alignment exclusive_store_\name, \alignment, 1
	.endif
	exclusive_translate EXCL(store_miss_)\name
	\instruction w14, \host_register\()15, [x7]
	cbnz w14, EXCL(store_lost_)\name
EXCL(store_done_)\name:
	exclusive_record_clear
	.if !\discard_status
	ubfx x20, x19, #16, #16
	str x14, [_vcpu, x20]
	.endif
	next_gadget
EXCL(store_miss_)\name:
	adr x13, EXCL(store_done_)\name
	b \complete
EXCL(store_lost_)\name:
	adr x13, EXCL(store_done_)\name
	b \lost
.endm

.macro exclusive_store_modes name, instruction, host_register, alignment
	exclusive_store_gadget \name\()_rr, \instruction, \host_register, \alignment, 0, 0, EXCL(store_complete_)\name, EXCL(store_lost_)\name
	exclusive_store_gadget \name\()_rz, \instruction, \host_register, \alignment, 0, 1, EXCL(store_complete_)\name, EXCL(store_lost_)\name
	exclusive_store_gadget \name\()_zr, \instruction, \host_register, \alignment, 1, 0, EXCL(store_complete_)\name, EXCL(store_lost_)\name
	exclusive_store_gadget \name\()_zz, \instruction, \host_register, \alignment, 1, 1, EXCL(store_complete_)\name, EXCL(store_lost_)\name
.endm

.macro exclusive_store_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	exclusive_store_modes \name\()_b, \byte_instruction, w, 0
	exclusive_store_modes \name\()_h, \half_instruction, w, 1
	exclusive_store_modes \name\()_w, \word_instruction, w, 3
	exclusive_store_modes \name\()_x, \double_instruction, x, 7
.endm

exclusive_store_sizes relaxed, stxrb, stxrh, stxr, stxr
exclusive_store_sizes release, stlxrb, stlxrh, stlxr, stlxr

.purgem exclusive_store_sizes
.purgem exclusive_store_modes
.purgem exclusive_store_gadget

/* Pair exclusive operations. Fixed host registers capture both load results;
 * translation-time variants only commit architecturally visible destinations
 * and materialize ZR store operands without runtime branches. */
.macro exclusive_pair_load_gadget name, instruction, host_register, alignment, discard_first, discard_second
gadget exclusive_pair_load_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #32, #16
	ldr x7, [_vcpu, x20]
	tbi_strip x7
	single_copy_alignment exclusive_pair_load_\name, \alignment, 0
	exclusive_translate EXCL(pair_load_miss_)\name
EXCL(pair_load_resume_)\name:
	\instruction \host_register\()15, \host_register\()16, [x7]
	exclusive_record_load_pair
	.if !\discard_first
	and x21, x19, #0xffff
	str x15, [_vcpu, x21]
	.endif
	.if !\discard_second
	ubfx x22, x19, #16, #16
	str x16, [_vcpu, x22]
	.endif
	next_gadget
EXCL(pair_load_miss_)\name:
	adr x13, EXCL(pair_load_resume_)\name
	b EXCL(load_miss)
.endm

.macro exclusive_pair_load_modes name, instruction, host_register, alignment
	exclusive_pair_load_gadget \name\()_rr, \instruction, \host_register, \alignment, 0, 0
	exclusive_pair_load_gadget \name\()_rz, \instruction, \host_register, \alignment, 0, 1
	exclusive_pair_load_gadget \name\()_zr, \instruction, \host_register, \alignment, 1, 0
	exclusive_pair_load_gadget \name\()_zz, \instruction, \host_register, \alignment, 1, 1
.endm

.macro exclusive_pair_load_sizes name, word_instruction, double_instruction
	exclusive_pair_load_modes \name\()_w, \word_instruction, w, 7
	exclusive_pair_load_modes \name\()_x, \double_instruction, x, 15
.endm

exclusive_pair_load_sizes relaxed, ldxp, ldxp
exclusive_pair_load_sizes acquire, ldaxp, ldaxp

.purgem exclusive_pair_load_sizes
.purgem exclusive_pair_load_modes
.purgem exclusive_pair_load_gadget

.macro exclusive_pair_store_gadget name, instruction, host_register, alignment, first_zero, second_zero, discard_status, complete, lost
gadget exclusive_pair_store_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #36, #12
	ldr x7, [_vcpu, x20]
	tbi_strip x7
	.if \first_zero
	mov x15, xzr
	.else
	and x21, x19, #0xfff
	ldr x15, [_vcpu, x21]
	.endif
	.if \second_zero
	mov x16, xzr
	.else
	ubfx x22, x19, #12, #12
	ldr x16, [_vcpu, x22]
	.endif
	single_copy_alignment exclusive_pair_store_\name, \alignment, 1
	exclusive_translate EXCL(pair_store_miss_)\name
	\instruction w14, \host_register\()15, \host_register\()16, [x7]
	cbnz w14, EXCL(pair_store_lost_)\name
EXCL(pair_store_done_)\name:
	exclusive_record_clear
	.if !\discard_status
	ubfx x20, x19, #24, #12
	str x14, [_vcpu, x20]
	.endif
	next_gadget
EXCL(pair_store_miss_)\name:
	adr x13, EXCL(pair_store_done_)\name
	b \complete
EXCL(pair_store_lost_)\name:
	adr x13, EXCL(pair_store_done_)\name
	b \lost
.endm

.macro exclusive_pair_store_modes name, instruction, host_register, alignment
	exclusive_pair_store_gadget \name\()_rrr, \instruction, \host_register, \alignment, 0, 0, 0, EXCL(pair_store_complete_)\name, EXCL(pair_store_lost_)\name
	exclusive_pair_store_gadget \name\()_rrz, \instruction, \host_register, \alignment, 0, 0, 1, EXCL(pair_store_complete_)\name, EXCL(pair_store_lost_)\name
	exclusive_pair_store_gadget \name\()_rzr, \instruction, \host_register, \alignment, 0, 1, 0, EXCL(pair_store_complete_)\name, EXCL(pair_store_lost_)\name
	exclusive_pair_store_gadget \name\()_rzz, \instruction, \host_register, \alignment, 0, 1, 1, EXCL(pair_store_complete_)\name, EXCL(pair_store_lost_)\name
	exclusive_pair_store_gadget \name\()_zrr, \instruction, \host_register, \alignment, 1, 0, 0, EXCL(pair_store_complete_)\name, EXCL(pair_store_lost_)\name
	exclusive_pair_store_gadget \name\()_zrz, \instruction, \host_register, \alignment, 1, 0, 1, EXCL(pair_store_complete_)\name, EXCL(pair_store_lost_)\name
	exclusive_pair_store_gadget \name\()_zzr, \instruction, \host_register, \alignment, 1, 1, 0, EXCL(pair_store_complete_)\name, EXCL(pair_store_lost_)\name
	exclusive_pair_store_gadget \name\()_zzz, \instruction, \host_register, \alignment, 1, 1, 1, EXCL(pair_store_complete_)\name, EXCL(pair_store_lost_)\name
.endm

.macro exclusive_pair_store_sizes name, word_instruction, double_instruction
	exclusive_pair_store_modes \name\()_w, \word_instruction, w, 7
	exclusive_pair_store_modes \name\()_x, \double_instruction, x, 15
.endm

exclusive_pair_store_sizes relaxed, stxp, stxp
exclusive_pair_store_sizes release, stlxp, stlxp

.purgem exclusive_pair_store_sizes
.purgem exclusive_pair_store_modes
.purgem exclusive_pair_store_gadget

/* The shared routines of the stubs above. */
	.p2align 4
	exclusive_load_miss EXCL(load_miss), uml_arm64_gadget_memory_fault_indexed

.macro exclusive_store_completes order, byte_cas, half_cas, word_cas, double_cas
	exclusive_store_complete EXCL(store_complete_)\order\()_b, EXCL(store_lost_)\order\()_b, \byte_cas, w, uml_arm64_gadget_memory_fault_indexed
	exclusive_store_complete EXCL(store_complete_)\order\()_h, EXCL(store_lost_)\order\()_h, \half_cas, w, uml_arm64_gadget_memory_fault_indexed
	exclusive_store_complete EXCL(store_complete_)\order\()_w, EXCL(store_lost_)\order\()_w, \word_cas, w, uml_arm64_gadget_memory_fault_indexed
	exclusive_store_complete EXCL(store_complete_)\order\()_x, EXCL(store_lost_)\order\()_x, \double_cas, x, uml_arm64_gadget_memory_fault_indexed
.endm

exclusive_store_completes relaxed, casb, cash, cas, cas
exclusive_store_completes release, caslb, caslh, casl, casl

.purgem exclusive_store_completes

.macro exclusive_pair_store_completes order, word_cas, double_cas
	exclusive_pair_store_complete EXCL(pair_store_complete_)\order\()_w, EXCL(pair_store_lost_)\order\()_w, \word_cas, w, uml_arm64_gadget_memory_fault_indexed
	exclusive_pair_store_complete EXCL(pair_store_complete_)\order\()_x, EXCL(pair_store_lost_)\order\()_x, \double_cas, x, uml_arm64_gadget_memory_fault_indexed
.endm

exclusive_pair_store_completes relaxed, casp, casp
exclusive_pair_store_completes release, caspl, caspl

.purgem exclusive_pair_store_completes

/* Fill the rest of the old footprint with BRK #0, which the region layout
 * guard accepts where it rejects zero words; no flow reaches it. The .org
 * after the fill fails the build if the group ever outgrows the footprint. */
.if Lsme_twin
	.fill (Lexcl_footprint_twin - (. - EXCL(group))) / 4, 4, 0xd4200000
	.org EXCL(group) + Lexcl_footprint_twin
.else
	.fill (Lexcl_footprint - (. - EXCL(group))) / 4, 4, 0xd4200000
	.org EXCL(group) + Lexcl_footprint
.endif


/* Ordered scalar loads/stores (LDAR and STLR families). */
.macro ordered_load_gadget name, instruction, host_register, alignment, discard_result
gadget ordered_load_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #16, #16
	ldr x7, [_vcpu, x20]
	tbi_strip x7
	.if \alignment
	single_copy_alignment ordered_load_\name, \alignment, 0
	.endif
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	\instruction \host_register\()15, [x7]
	.if !\discard_result
	and x21, x19, #0xffff
	str x15, [_vcpu, x21]
	.endif
	next_gadget
.endm

.macro ordered_load_modes name, instruction, host_register, alignment
	ordered_load_gadget \name\()_r, \instruction, \host_register, \alignment, 0
	ordered_load_gadget \name\()_z, \instruction, \host_register, \alignment, 1
.endm

.macro ordered_load_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	ordered_load_modes \name\()_b, \byte_instruction, w, 0
	ordered_load_modes \name\()_h, \half_instruction, w, 1
	ordered_load_modes \name\()_w, \word_instruction, w, 3
	ordered_load_modes \name\()_x, \double_instruction, x, 7
.endm

ordered_load_sizes acquire, ldarb, ldarh, ldar, ldar

.purgem ordered_load_sizes
.purgem ordered_load_modes
.purgem ordered_load_gadget

.macro ordered_store_gadget name, instruction, host_register, alignment, source_zero
gadget ordered_store_\name
	ldr x19, [_stream], #8
	ubfx x20, x19, #16, #16
	ldr x7, [_vcpu, x20]
	tbi_strip x7
	.if \alignment
	single_copy_alignment ordered_store_\name, \alignment, 1
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	.if \source_zero
	mov x15, xzr
	.else
	and x21, x19, #0xffff
	ldr x15, [_vcpu, x21]
	.endif
	\instruction \host_register\()15, [x7]
	next_gadget
.endm

.macro ordered_store_modes name, instruction, host_register, alignment
	ordered_store_gadget \name\()_r, \instruction, \host_register, \alignment, 0
	ordered_store_gadget \name\()_z, \instruction, \host_register, \alignment, 1
.endm

.macro ordered_store_sizes name, byte_instruction, half_instruction, word_instruction, double_instruction
	ordered_store_modes \name\()_b, \byte_instruction, w, 0
	ordered_store_modes \name\()_h, \half_instruction, w, 1
	ordered_store_modes \name\()_w, \word_instruction, w, 3
	ordered_store_modes \name\()_x, \double_instruction, x, 7
.endm

ordered_store_sizes release, stlrb, stlrh, stlr, stlr

.purgem ordered_store_sizes
.purgem ordered_store_modes
.purgem ordered_store_gadget

/* FEAT_LRCPC/2 LDAPR and LDAPUR plus FEAT_LRCPC2 STLUR. The builder packs
 * the signed byte offset and chooses width, extension, and ZR handling, so a
 * valid hot path only calculates the address, translates it, and executes the
 * exact host RCpc/release instruction. Stream: packed operand, which ends in
 * the instruction's position in its block. */
.macro rcpc_load_gadget name, instruction, host_register, alignment, extension, discard
gadget rcpc_load_\name
	ldr x19, [_stream], #8
	and w20, w19, #0x1f
	ubfx w21, w19, #5, #5
	add x8, _vcpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x20, lsl #3]
	sbfx x9, x19, #10, #9
	add x7, x7, x9
	tbi_strip x7
	.if \alignment
	single_copy_alignment rcpc_load_\name, \alignment, 0
	.endif
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	\instruction \host_register\()15, [x7]
	.if \extension == 1
	sxtb w15, w15
	.elseif \extension == 2
	sxtb x15, w15
	.elseif \extension == 3
	sxth w15, w15
	.elseif \extension == 4
	sxth x15, w15
	.elseif \extension == 5
	sxtw x15, w15
	.endif
	.if !\discard
	add x8, _vcpu, #UML_GADGET_CPU_X0
	str x15, [x8, x21, lsl #3]
	.endif
	next_gadget
.endm

.macro rcpc_load_modes name, instruction, host_register, alignment, extension
	rcpc_load_gadget \name\()_r, \instruction, \host_register, \alignment, \extension, 0
	rcpc_load_gadget \name\()_z, \instruction, \host_register, \alignment, \extension, 1
.endm

rcpc_load_modes zero_b, ldaprb, w, 0, 0
rcpc_load_modes zero_h, ldaprh, w, 1, 0
rcpc_load_modes zero_w, ldapr, w, 3, 0
rcpc_load_modes zero_x, ldapr, x, 7, 0
rcpc_load_modes sb_w, ldaprb, w, 0, 1
rcpc_load_modes sb_x, ldaprb, w, 0, 2
rcpc_load_modes sh_w, ldaprh, w, 1, 3
rcpc_load_modes sh_x, ldaprh, w, 1, 4
rcpc_load_modes sw_x, ldapr, w, 3, 5

.purgem rcpc_load_modes
.purgem rcpc_load_gadget

.macro rcpc_store_gadget name, instruction, host_register, alignment, source_zero
gadget rcpc_store_\name
	ldr x19, [_stream], #8
	and w20, w19, #0x1f
	ubfx w21, w19, #5, #5
	add x8, _vcpu, #UML_GADGET_CPU_X0
	ldr x7, [x8, x20, lsl #3]
	sbfx x9, x19, #10, #9
	add x7, x7, x9
	tbi_strip x7
	.if \alignment
	single_copy_alignment rcpc_store_\name, \alignment, 1
	.endif
	translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	.if \source_zero
	mov x15, xzr
	.else
	add x8, _vcpu, #UML_GADGET_CPU_X0
	ldr x15, [x8, x21, lsl #3]
	.endif
	\instruction \host_register\()15, [x7]
	next_gadget
.endm

.macro rcpc_store_modes name, instruction, host_register, alignment
	rcpc_store_gadget \name\()_r, \instruction, \host_register, \alignment, 0
	rcpc_store_gadget \name\()_z, \instruction, \host_register, \alignment, 1
.endm

rcpc_store_modes b, stlrb, w, 0
rcpc_store_modes h, stlrh, w, 1
rcpc_store_modes w, stlr, w, 3
rcpc_store_modes x, stlr, x, 7

.purgem rcpc_store_modes
.purgem rcpc_store_gadget

/* Purged only now, which lets engine/sme.inc define them again. */
.purgem single_copy_alignment
.purgem mops_epilogue
.purgem mops_prologue
