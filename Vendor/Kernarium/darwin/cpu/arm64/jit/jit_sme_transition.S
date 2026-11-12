// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/layout/assembly_offsets.h"

/*
 * Host streaming-mode transitions for SM=1 JIT translations. Guest Z, P,
 * ZA and ZT0 live in the identically numbered host registers while SM=1
 * translations run, and in the vCPU's register file
 * (CpuState::sme_registers, laid out by sme_profile.hpp) everywhere else.
 *
 * Generated code calls these routines with x15 = CpuState. They change no
 * register but x9-x13 and x30 (x16 holds the routine's address at a call
 * from generated code), never NZCV, and carry FPSR across their own
 * SMSTART or SMSTOP, which reset it to 0x0800009F. That FPSR is the host's:
 * XNU also resets FPSR on every kernel entry while a thread is in streaming
 * mode, so an SM=1 translation keeps the guest's FPSR in CpuState and never
 * takes the native FP environment. FPCR survives kernel entries: enter
 * installs the guest's (CpuState::fpcr with its trap enables clear, since
 * streaming mode never traps a floating-point exception) for the SM=1 code
 * and keeps the host's in CpuState::gadget_host_fpcr, which no one reads
 * while gadget_fp_active is clear, as it stays while SM=1 code runs; leave
 * puts the host's back. Every size and offset comes from the run-time SVL
 * (RDSVL, ADDSVL and VL-scaled addressing), never from a constant.
 *
 * The file is assembled for every Apple target; only an SME host executes
 * it, and only for a run whose profile implements SME.
 */
	.arch	armv9-a+sme2

.if UML_ARM64_SME_HOST_STREAMING != 1 || UML_ARM64_SME_HOST_ZA != 2
    .error "sme_host_mode bit layout changed"
.endif
.if UML_ARM64_SME_P_OFFSET_VL != 32 || UML_ARM64_SME_ZA_OFFSET_VL != 34
    .error "SME register file layout changed"
.endif
.if UML_ARM64_CPU_FP_ACTIVE != 904
    .error "CpuState gadget_fp_active layout changed"
.endif
.if UML_ARM64_CPU_HOST_FPCR != 888 || UML_ARM64_CPU_HOST_FPSR != 896
    .error "CpuState native FP layout changed"
.endif
.if UML_GADGET_CPU_FPCR != 784 || UML_ARM64_FPCR_TRAP_ENABLES != 0x9f00
    .error "CpuState FPCR layout or trap enables changed"
.endif

/* The JIT-private sme_host_mode bit for a resident guest ZT0
 * (jit_sme_host_zt0_resident in jit_sme_transition.hpp). */
#define JIT_SME_HOST_ZA_BIT 1
#define JIT_SME_HOST_ZT0_BIT 4

	.text

/*
 * uml_arm64_jit_sme_enter: w9 = the sme_host_mode to establish
 * (jit_sme_host_mode()). Enters streaming mode, with ZA when the mode
 * names it, records the mode, and fills Z0-Z31 and P0-P15, then ZA and,
 * for an SME2 profile, ZT0, from the register file; then installs the
 * guest's FPCR.
 */
	.p2align	2
	.private_extern _uml_arm64_jit_sme_enter
	.globl	_uml_arm64_jit_sme_enter
_uml_arm64_jit_sme_enter:
	mrs	x10, fpsr
	tbz	w9, #JIT_SME_HOST_ZA_BIT, 1f
	smstart
	b	2f
1:
	smstart	sm
2:
	strb	w9, [x15, #UML_ARM64_CPU_SME_HOST_MODE]
	ldr	x11, [x15, #UML_ARM64_CPU_SME_REGISTERS]
	ldr	z0, [x11, #0, mul vl]
	ldr	z1, [x11, #1, mul vl]
	ldr	z2, [x11, #2, mul vl]
	ldr	z3, [x11, #3, mul vl]
	ldr	z4, [x11, #4, mul vl]
	ldr	z5, [x11, #5, mul vl]
	ldr	z6, [x11, #6, mul vl]
	ldr	z7, [x11, #7, mul vl]
	ldr	z8, [x11, #8, mul vl]
	ldr	z9, [x11, #9, mul vl]
	ldr	z10, [x11, #10, mul vl]
	ldr	z11, [x11, #11, mul vl]
	ldr	z12, [x11, #12, mul vl]
	ldr	z13, [x11, #13, mul vl]
	ldr	z14, [x11, #14, mul vl]
	ldr	z15, [x11, #15, mul vl]
	ldr	z16, [x11, #16, mul vl]
	ldr	z17, [x11, #17, mul vl]
	ldr	z18, [x11, #18, mul vl]
	ldr	z19, [x11, #19, mul vl]
	ldr	z20, [x11, #20, mul vl]
	ldr	z21, [x11, #21, mul vl]
	ldr	z22, [x11, #22, mul vl]
	ldr	z23, [x11, #23, mul vl]
	ldr	z24, [x11, #24, mul vl]
	ldr	z25, [x11, #25, mul vl]
	ldr	z26, [x11, #26, mul vl]
	ldr	z27, [x11, #27, mul vl]
	ldr	z28, [x11, #28, mul vl]
	ldr	z29, [x11, #29, mul vl]
	ldr	z30, [x11, #30, mul vl]
	ldr	z31, [x11, #31, mul vl]
	rdsvl	x12, #16
	add	x11, x11, x12, lsl #1
	ldr	p0, [x11, #0, mul vl]
	ldr	p1, [x11, #1, mul vl]
	ldr	p2, [x11, #2, mul vl]
	ldr	p3, [x11, #3, mul vl]
	ldr	p4, [x11, #4, mul vl]
	ldr	p5, [x11, #5, mul vl]
	ldr	p6, [x11, #6, mul vl]
	ldr	p7, [x11, #7, mul vl]
	ldr	p8, [x11, #8, mul vl]
	ldr	p9, [x11, #9, mul vl]
	ldr	p10, [x11, #10, mul vl]
	ldr	p11, [x11, #11, mul vl]
	ldr	p12, [x11, #12, mul vl]
	ldr	p13, [x11, #13, mul vl]
	ldr	p14, [x11, #14, mul vl]
	ldr	p15, [x11, #15, mul vl]
	tbz	w9, #JIT_SME_HOST_ZA_BIT, 4f
	/* ZA row r at (34 + r) * SVL: SVL rows, 16 per iteration. */
	addsvl	x11, x11, #2
	mov	w12, #0
	rdsvl	x13, #1
3:
	ldr	za[w12, 0], [x11, #0, mul vl]
	ldr	za[w12, 1], [x11, #1, mul vl]
	ldr	za[w12, 2], [x11, #2, mul vl]
	ldr	za[w12, 3], [x11, #3, mul vl]
	ldr	za[w12, 4], [x11, #4, mul vl]
	ldr	za[w12, 5], [x11, #5, mul vl]
	ldr	za[w12, 6], [x11, #6, mul vl]
	ldr	za[w12, 7], [x11, #7, mul vl]
	ldr	za[w12, 8], [x11, #8, mul vl]
	ldr	za[w12, 9], [x11, #9, mul vl]
	ldr	za[w12, 10], [x11, #10, mul vl]
	ldr	za[w12, 11], [x11, #11, mul vl]
	ldr	za[w12, 12], [x11, #12, mul vl]
	ldr	za[w12, 13], [x11, #13, mul vl]
	ldr	za[w12, 14], [x11, #14, mul vl]
	ldr	za[w12, 15], [x11, #15, mul vl]
	addsvl	x11, x11, #16
	add	w12, w12, #16
	sub	x13, x13, #16
	cbnz	x13, 3b
	/* x11 now points past the last row, at ZT0. */
	tbz	w9, #JIT_SME_HOST_ZT0_BIT, 4f
	ldr	zt0, [x11]
4:
	mrs	x11, fpcr
	str	x11, [x15, #UML_ARM64_CPU_HOST_FPCR]
	ldr	w11, [x15, #UML_GADGET_CPU_FPCR]
	mov	w12, #UML_ARM64_FPCR_TRAP_ENABLES
	bic	w11, w11, w12
	msr	fpcr, x11
	msr	fpsr, x10
	ret

/*
 * uml_arm64_jit_sme_leave: the mirror image of enter for the mode
 * CpuState::sme_host_mode records, the host's FPCR included; nothing when
 * it records none, so a second leave on one exit path costs two
 * instructions.
 */
	.p2align	2
	.private_extern _uml_arm64_jit_sme_leave
	.globl	_uml_arm64_jit_sme_leave
_uml_arm64_jit_sme_leave:
	ldrb	w9, [x15, #UML_ARM64_CPU_SME_HOST_MODE]
	cbz	w9, 4f
	mrs	x10, fpsr
	ldr	x11, [x15, #UML_ARM64_CPU_SME_REGISTERS]
	str	z0, [x11, #0, mul vl]
	str	z1, [x11, #1, mul vl]
	str	z2, [x11, #2, mul vl]
	str	z3, [x11, #3, mul vl]
	str	z4, [x11, #4, mul vl]
	str	z5, [x11, #5, mul vl]
	str	z6, [x11, #6, mul vl]
	str	z7, [x11, #7, mul vl]
	str	z8, [x11, #8, mul vl]
	str	z9, [x11, #9, mul vl]
	str	z10, [x11, #10, mul vl]
	str	z11, [x11, #11, mul vl]
	str	z12, [x11, #12, mul vl]
	str	z13, [x11, #13, mul vl]
	str	z14, [x11, #14, mul vl]
	str	z15, [x11, #15, mul vl]
	str	z16, [x11, #16, mul vl]
	str	z17, [x11, #17, mul vl]
	str	z18, [x11, #18, mul vl]
	str	z19, [x11, #19, mul vl]
	str	z20, [x11, #20, mul vl]
	str	z21, [x11, #21, mul vl]
	str	z22, [x11, #22, mul vl]
	str	z23, [x11, #23, mul vl]
	str	z24, [x11, #24, mul vl]
	str	z25, [x11, #25, mul vl]
	str	z26, [x11, #26, mul vl]
	str	z27, [x11, #27, mul vl]
	str	z28, [x11, #28, mul vl]
	str	z29, [x11, #29, mul vl]
	str	z30, [x11, #30, mul vl]
	str	z31, [x11, #31, mul vl]
	rdsvl	x12, #16
	add	x11, x11, x12, lsl #1
	str	p0, [x11, #0, mul vl]
	str	p1, [x11, #1, mul vl]
	str	p2, [x11, #2, mul vl]
	str	p3, [x11, #3, mul vl]
	str	p4, [x11, #4, mul vl]
	str	p5, [x11, #5, mul vl]
	str	p6, [x11, #6, mul vl]
	str	p7, [x11, #7, mul vl]
	str	p8, [x11, #8, mul vl]
	str	p9, [x11, #9, mul vl]
	str	p10, [x11, #10, mul vl]
	str	p11, [x11, #11, mul vl]
	str	p12, [x11, #12, mul vl]
	str	p13, [x11, #13, mul vl]
	str	p14, [x11, #14, mul vl]
	str	p15, [x11, #15, mul vl]
	tbz	w9, #JIT_SME_HOST_ZA_BIT, 3f
	addsvl	x11, x11, #2
	mov	w12, #0
	rdsvl	x13, #1
1:
	str	za[w12, 0], [x11, #0, mul vl]
	str	za[w12, 1], [x11, #1, mul vl]
	str	za[w12, 2], [x11, #2, mul vl]
	str	za[w12, 3], [x11, #3, mul vl]
	str	za[w12, 4], [x11, #4, mul vl]
	str	za[w12, 5], [x11, #5, mul vl]
	str	za[w12, 6], [x11, #6, mul vl]
	str	za[w12, 7], [x11, #7, mul vl]
	str	za[w12, 8], [x11, #8, mul vl]
	str	za[w12, 9], [x11, #9, mul vl]
	str	za[w12, 10], [x11, #10, mul vl]
	str	za[w12, 11], [x11, #11, mul vl]
	str	za[w12, 12], [x11, #12, mul vl]
	str	za[w12, 13], [x11, #13, mul vl]
	str	za[w12, 14], [x11, #14, mul vl]
	str	za[w12, 15], [x11, #15, mul vl]
	addsvl	x11, x11, #16
	add	w12, w12, #16
	sub	x13, x13, #16
	cbnz	x13, 1b
	tbz	w9, #JIT_SME_HOST_ZT0_BIT, 3f
	str	zt0, [x11]
3:
	smstop
	strb	wzr, [x15, #UML_ARM64_CPU_SME_HOST_MODE]
	ldr	x11, [x15, #UML_ARM64_CPU_HOST_FPCR]
	msr	fpcr, x11
	msr	fpsr, x10
4:
	ret

/*
 * uml_arm64_jit_tlb_translate_streaming: the TLB miss bridge of SM=1
 * translations, with the contract of uml_arm64_jit_tlb_translate_preserving
 * (x0 = Tlb, x1 = address, x2 = access in; x0 = host pointer or null out;
 * x1, x2 and the scratch registers clobbered; the resident GPRs, x14, x15,
 * x30, NZCV, FPCR and FPSR preserved). It leaves streaming mode around the
 * C++ walker and enters it again in the same mode, so the guest's vector
 * state survives in the register file rather than in a 128-bit save.
 */
	.p2align	2
	.private_extern _uml_arm64_jit_tlb_translate_streaming
	.globl	_uml_arm64_jit_tlb_translate_streaming
_uml_arm64_jit_tlb_translate_streaming:
	sub	sp, sp, #128
	stp	x3, x4, [sp, #0]
	stp	x5, x6, [sp, #16]
	stp	x7, x12, [sp, #32]
	stp	x13, x14, [sp, #48]
	stp	x15, x17, [sp, #64]
	mrs	x9, nzcv
	stp	x30, x9, [sp, #80]
	mrs	x10, fpcr
	mrs	x11, fpsr
	stp	x10, x11, [sp, #96]
	ldrb	w9, [x15, #UML_ARM64_CPU_SME_HOST_MODE]
	str	x9, [sp, #112]
	bl	_uml_arm64_jit_sme_leave

	/* As the preserving bridge: the walker runs with the host's own
	 * floating-point environment when a block owns the guest's. */
	ldrb	w9, [x15, #UML_ARM64_CPU_FP_ACTIVE]
	cbz	w9, 1f
	ldr	x10, [x15, #UML_ARM64_CPU_HOST_FPCR]
	ldr	x11, [x15, #UML_ARM64_CPU_HOST_FPSR]
	msr	fpcr, x10
	msr	fpsr, x11
1:
	bl	_uml_arm64_jit_tlb_translate

	ldr	x15, [sp, #64]
	ldr	x9, [sp, #112]
	bl	_uml_arm64_jit_sme_enter

	ldp	x10, x11, [sp, #96]
	msr	fpcr, x10
	msr	fpsr, x11
	ldp	x30, x9, [sp, #80]
	msr	nzcv, x9
	ldp	x15, x17, [sp, #64]
	ldp	x13, x14, [sp, #48]
	ldp	x7, x12, [sp, #32]
	ldp	x5, x6, [sp, #16]
	ldp	x3, x4, [sp, #0]
	add	sp, sp, #128
	ret
