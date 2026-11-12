// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/layout/assembly_offsets.h"

/* Keep the rare bridge coupled to the verified neutral ABI, not to gadget
 * implementation headers.  C++ static assertions live in assembly_layout.hpp. */
.if UML_ARM64_CPU_FP_ACTIVE != 904
    .error "CpuState gadget_fp_active layout changed"
.endif
.if UML_ARM64_CPU_HOST_FPCR != 888 || UML_ARM64_CPU_HOST_FPSR != 896
    .error "CpuState native FP layout changed"
.endif

/*
 * Rare JIT TLB misses enter a normal C++ helper.  Generated blocks keep guest
 * values in caller-saved host registers, so use this deliberately narrow
 * preserving bridge rather than spilling every live binding into CpuState at
 * each call site.  x0 is the helper's pointer result; x1/x2 and the ordinary
 * scratch registers may be clobbered.  The caller-saved resident registers,
 * the CpuState/TLB context registers, and LR are preserved explicitly, and
 * so are V0-V7, where an AArch32 translation keeps copies of S registers
 * across its accesses.
 */

	.text
	.p2align	2
	.private_extern _uml_arm64_jit_tlb_translate_preserving
	.globl	_uml_arm64_jit_tlb_translate_preserving
_uml_arm64_jit_tlb_translate_preserving:
	sub	sp, sp, #496
	mrs	x9, nzcv
	mrs	x10, fpcr
	mrs	x11, fpsr
	stp	x3, x4, [sp, #0]
	stp	x5, x6, [sp, #16]
	stp	x7, x12, [sp, #32]
	stp	x13, x14, [sp, #48]
	stp	x15, x17, [sp, #64]
	stp	x30, x9, [sp, #80]
	stp	x10, x11, [sp, #96]
	stp	q16, q17, [sp, #112]
	stp	q18, q19, [sp, #144]
	stp	q20, q21, [sp, #176]
	stp	q22, q23, [sp, #208]
	stp	q24, q25, [sp, #240]
	stp	q26, q27, [sp, #272]
	stp	q28, q29, [sp, #304]
	stp	q30, q31, [sp, #336]
	stp	q6, q7, [sp, #368]
	stp	q0, q1, [sp, #400]
	stp	q2, q3, [sp, #432]
	stp	q4, q5, [sp, #464]

	/*
	 * A generated block may own the guest floating-point environment. A
	 * normal C++ helper must run with the saved host environment, while an
	 * inactive block simply preserves the host state already installed.
	 */
	ldrb	w9, [x15, #UML_ARM64_CPU_FP_ACTIVE]
	cbz	w9, 1f
	ldr	x10, [x15, #UML_ARM64_CPU_HOST_FPCR]
	ldr	x11, [x15, #UML_ARM64_CPU_HOST_FPSR]
	msr	fpcr, x10
	msr	fpsr, x11
1:
	bl	_uml_arm64_jit_tlb_translate

	ldp	q4, q5, [sp, #464]
	ldp	q2, q3, [sp, #432]
	ldp	q0, q1, [sp, #400]
	ldp	q6, q7, [sp, #368]
	ldp	q30, q31, [sp, #336]
	ldp	q28, q29, [sp, #304]
	ldp	q26, q27, [sp, #272]
	ldp	q24, q25, [sp, #240]
	ldp	q22, q23, [sp, #208]
	ldp	q20, q21, [sp, #176]
	ldp	q18, q19, [sp, #144]
	ldp	q16, q17, [sp, #112]
	ldp	x10, x11, [sp, #96]
	ldp	x30, x9, [sp, #80]
	ldp	x15, x17, [sp, #64]
	ldp	x13, x14, [sp, #48]
	ldp	x7, x12, [sp, #32]
	ldp	x5, x6, [sp, #16]
	ldp	x3, x4, [sp, #0]
	msr	fpcr, x10
	msr	fpsr, x11
	add	sp, sp, #496
	msr	nzcv, x9
	ret
