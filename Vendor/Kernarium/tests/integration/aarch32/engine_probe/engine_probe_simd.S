// SPDX-License-Identifier: GPL-2.0-or-later
/* Group simd: VFP and Advanced SIMD routines. Built with the Armv8 VFP and NEON
 * extensions, called as ordinary functions with the soft-float argument
 * registers; the C++ side never touches a floating-point register, so what
 * these routines leave in D0-D31 survives until the next routine. */
	.syntax	unified
	.arch	armv8-a
	.fpu	neon-fp-armv8
	.text

	.macro	func_arm name
	.arm
	.balign	4
	.globl	\name
	.type	\name, %function
\name:
	.endm

	.macro	func_thumb name
	.thumb
	.balign	4
	.thumb_func
	.globl	\name
	.type	\name, %function
\name:
	.endm

@ ---- state: simd_load(const u64 d[32]), simd_store(u64 d[32]) ----
	func_arm simd_load
	vldmia	r0!, {d0-d15}
	vldmia	r0, {d16-d31}
	bx	lr

	func_arm simd_store
	vstmia	r0!, {d0-d15}
	vstmia	r0, {d16-d31}
	bx	lr

	func_thumb simd_load_thumb
	vldmia	r0!, {d0-d15}
	vldmia	r0, {d16-d31}
	bx	lr

	func_thumb simd_store_thumb
	vstmia	r0!, {d0-d15}
	vstmia	r0, {d16-d31}
	bx	lr

@ ---- u32 simd_get_fpscr(void), void simd_set_fpscr(u32) ----
	func_arm simd_get_fpscr
	vmrs	r0, fpscr
	bx	lr

	func_arm simd_set_fpscr
	vmsr	fpscr, r0
	bx	lr

@ ---- void simd_clobber(u32 fpscr): every D register and FPSCR changed ----
	func_arm simd_clobber
	vmov.i32 q0, #0xff
	vmov.i32 q1, #0xff
	vmov.i32 q2, #0xff
	vmov.i32 q3, #0xff
	vmov.i32 q4, #0xff
	vmov.i32 q5, #0xff
	vmov.i32 q6, #0xff
	vmov.i32 q7, #0xff
	vmov.i32 q8, #0xff
	vmov.i32 q9, #0xff
	vmov.i32 q10, #0xff
	vmov.i32 q11, #0xff
	vmov.i32 q12, #0xff
	vmov.i32 q13, #0xff
	vmov.i32 q14, #0xff
	vmov.i32 q15, #0xff
	vmsr	fpscr, r0
	bx	lr

@ ---- void simd_spin(u32 n, u32 out[12]): d0 += 1.0 and q8 += 1, n times ----
@ out: d0 (2 words), q8 (4 words), q10 (4 words, never changed), d12 (2 words).
	func_arm simd_spin
	veor	d0, d0, d0
	vmov.f64 d1, #1.0
	veor	q8, q8, q8
	vmov.i32 q9, #1
	vmov.i32 q10, #0x5a
	movw	r3, #0x1234
	movt	r3, #0x5678
	vmov	d12, r3, r3
1:	vadd.f64 d0, d0, d1
	vadd.i32 q8, q8, q9
	subs	r0, r0, #1
	bne	1b
	vstr	d0, [r1]
	add	r1, r1, #8
	vst1.32	{d16, d17}, [r1]
	add	r1, r1, #16
	vst1.32	{d20, d21}, [r1]
	add	r1, r1, #16
	vstr	d12, [r1]
	bx	lr

@ ---- arithmetic: void fp_<op>(FpCall *call) ----
@ FpCall: a at 0, b at 8, c at 16, result at 24, fpscr_in at 32, fpscr_out at 36.
	.macro	fp_binary name, single, double, ins
	func_arm \name
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	.if \single
	vldr	s0, [r0]
	vldr	s1, [r0, #8]
	vldr	s2, [r0, #16]
	\ins\().f32 s2, s0, s1
	vmrs	r2, fpscr
	vstr	s2, [r0, #24]
	.else
	vldr	d0, [r0]
	vldr	d1, [r0, #8]
	vldr	d2, [r0, #16]
	\ins\().f64 d2, d0, d1
	vmrs	r2, fpscr
	vstr	d2, [r0, #24]
	.endif
	str	r2, [r0, #36]
	bx	lr
	.endm

	fp_binary fp_add_f32, 1, 0, vadd
	fp_binary fp_sub_f32, 1, 0, vsub
	fp_binary fp_mul_f32, 1, 0, vmul
	fp_binary fp_div_f32, 1, 0, vdiv
	fp_binary fp_fma_f32, 1, 0, vfma
	fp_binary fp_add_f64, 0, 1, vadd
	fp_binary fp_mul_f64, 0, 1, vmul
	fp_binary fp_div_f64, 0, 1, vdiv
	fp_binary fp_fma_f64, 0, 1, vfma

	func_arm fp_sqrt_f32
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	s0, [r0]
	vsqrt.f32 s2, s0
	vmrs	r2, fpscr
	vstr	s2, [r0, #24]
	str	r2, [r0, #36]
	bx	lr

@ vcvtr and vcvt to s32 (the first rounds as FPSCR says, the second toward zero).
	func_arm fp_cvtr_s32_f32
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	s0, [r0]
	vcvtr.s32.f32 s2, s0
	vmrs	r2, fpscr
	vstr	s2, [r0, #24]
	str	r2, [r0, #36]
	bx	lr

	func_arm fp_cvt_s32_f32
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	s0, [r0]
	vcvt.s32.f32 s2, s0
	vmrs	r2, fpscr
	vstr	s2, [r0, #24]
	str	r2, [r0, #36]
	bx	lr

	func_arm fp_cvt_u32_f32
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	s0, [r0]
	vcvt.u32.f32 s2, s0
	vmrs	r2, fpscr
	vstr	s2, [r0, #24]
	str	r2, [r0, #36]
	bx	lr

@ f16 <-> f32 (the low half of the single register), with FPSCR.AHP.
	func_arm fp_cvtb_f16_f32
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	s0, [r0]
	vcvtb.f16.f32 s2, s0
	vmrs	r2, fpscr
	vstr	s2, [r0, #24]
	str	r2, [r0, #36]
	bx	lr

	func_arm fp_cvtb_f32_f16
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	s0, [r0]
	vcvtb.f32.f16 s2, s0
	vmrs	r2, fpscr
	vstr	s2, [r0, #24]
	str	r2, [r0, #36]
	bx	lr

@ compares: the flags are returned in fpscr_out
	func_arm fp_cmp_f32
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	s0, [r0]
	vldr	s1, [r0, #8]
	vcmp.f32 s0, s1
	vmrs	r2, fpscr
	str	r2, [r0, #36]
	bx	lr

	func_arm fp_cmpe_f32
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	s0, [r0]
	vldr	s1, [r0, #8]
	vcmpe.f32 s0, s1
	vmrs	r2, fpscr
	str	r2, [r0, #36]
	bx	lr

	func_arm fp_cmp_f64
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	d0, [r0]
	vldr	d1, [r0, #8]
	vcmp.f64 d0, d1
	vmrs	r2, fpscr
	str	r2, [r0, #36]
	bx	lr

@ The compare result copied into APSR and read back through MRS.
	func_arm fp_cmp_apsr_f32
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	s0, [r0]
	vldr	s1, [r0, #8]
	vcmp.f32 s0, s1
	vmrs	APSR_nzcv, fpscr
	mrs	r2, apsr
	str	r2, [r0, #24]
	vmrs	r2, fpscr
	str	r2, [r0, #36]
	bx	lr

@ NEON, 64-bit forms: result = d(a) op d(b), under the Standard FPSCR value.
	.macro	neon_binary name, ins
	func_arm \name
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	d0, [r0]
	vldr	d1, [r0, #8]
	\ins	d2, d0, d1
	vmrs	r2, fpscr
	vstr	d2, [r0, #24]
	str	r2, [r0, #36]
	bx	lr
	.endm

	neon_binary neon_add_f32, "vadd.f32"
	neon_binary neon_mul_f32, "vmul.f32"
	neon_binary neon_qadd_s8, "vqadd.s8"
	neon_binary neon_add_i32, "vadd.i32"
	neon_binary neon_max_f32, "vmax.f32"
	neon_binary neon_recps_f32, "vrecps.f32"
	neon_binary neon_rsqrts_f32, "vrsqrts.f32"

@ ---- data movement: void simd_alias(u32 out[8]) ----
@ d0 = 0x1122334455667788; the single, double and quad views of it.
	func_arm simd_alias
	movw	r2, #0x7788
	movt	r2, #0x5566
	movw	r3, #0x3344
	movt	r3, #0x1122
	vmov	d0, r2, r3
	vmov	r12, s0
	str	r12, [r0]
	vmov	r12, s1
	str	r12, [r0, #4]
	vmov	s4, r3
	vmov	s5, r2
	vmov	r12, r1, d2
	str	r12, [r0, #8]
	str	r1, [r0, #12]
	vmov.f64 d4, d0
	vmov	r12, r1, d4
	str	r12, [r0, #16]
	str	r1, [r0, #20]
	vmov	d6, r2, r3
	vmov	d7, r3, r2
	vmov	q4, q3
	vmov	r12, r1, d9
	str	r12, [r0, #24]
	str	r1, [r0, #28]
	bx	lr

@ ---- void simd_pushpop(u32 out[8]): vpush and vpop ----
@ out: d8, d9, d10 (6 words), the bytes vpush moved sp, and sp after vpop - sp before.
	func_arm simd_pushpop
	push	{r4, lr}
	movw	r2, #0x0001
	movt	r2, #0x0002
	movw	r3, #0x0003
	movt	r3, #0x0004
	vmov	d8, r2, r3
	vmov	d9, r3, r2
	vmov	d10, r2, r2
	mov	r1, sp
	vpush	{d8-d10}
	mov	r12, sp
	vmov.i32 d8, #0
	vmov.i32 d9, #0
	vmov.i32 d10, #0
	vpop	{d8-d10}
	vstr	d8, [r0]
	vstr	d9, [r0, #8]
	vstr	d10, [r0, #16]
	sub	r2, r1, r12
	str	r2, [r0, #24]
	sub	r2, sp, r1
	str	r2, [r0, #28]
	pop	{r4, pc}

@ ---- alignment: one access per routine at the address in r0 ----
	.macro	sa_arm name, insn
	func_arm \name
\insn
	bx	lr
	.endm

	.macro	sa_thumb name, insn
	func_thumb \name
\insn
	bx	lr
	.endm

	.macro	sa_pair name, insn
	sa_arm	\name\()_arm, "\insn"
	sa_thumb \name\()_thumb, "\insn"
	.endm

	sa_pair	sa_vldr_s, "vldr s0, [r0]"
	sa_pair	sa_vldr_d, "vldr d0, [r0]"
	sa_pair	sa_vstr_s, "vstr s0, [r0]"
	sa_pair	sa_vstr_d, "vstr d0, [r0]"
	sa_pair	sa_vldm_s, "vldmia r0, {s0-s3}"
	sa_pair	sa_vldm_d, "vldmia r0, {d0-d3}"
	sa_pair	sa_vstm_d, "vstmia r0, {d0-d3}"
	sa_pair	sa_vld1_a64, "vld1.32 {d0}, [r0:64]"
	sa_pair	sa_vld1_a128, "vld1.32 {d0, d1}, [r0:128]"
	sa_pair	sa_vld1_a256, "vld1.32 {d0, d1, d2, d3}, [r0:256]"
	sa_pair	sa_vst1_a64, "vst1.32 {d0}, [r0:64]"
	sa_pair	sa_vld1_free, "vld1.32 {d0, d1}, [r0]"
	sa_pair	sa_vld1_byte, "vld1.8 {d0}, [r0]"
	sa_pair	sa_vst1_free, "vst1.64 {d0}, [r0]"
	sa_pair	sa_vld3_free, "vld3.8 {d0, d1, d2}, [r0]"
	sa_pair	sa_vld1_lane, "vld1.32 {d0[1]}, [r0]"

@ ---- void simd_bkpt(void): a stop for the tracer ----
	func_arm simd_bkpt
	bkpt	#1
	bx	lr

@ ---- the Armv8 floating-point additions: one instruction per routine ----
@ s0 and s1 are a and b, s2 is c, and the result is s2.
	.macro	fp_any name, insn
	func_arm \name
	ldr	r1, [r0, #32]
	vmsr	fpscr, r1
	vldr	s0, [r0]
	vldr	s1, [r0, #8]
	vldr	s2, [r0, #16]
	\insn
	vmrs	r2, fpscr
	vstr	s2, [r0, #24]
	str	r2, [r0, #36]
	bx	lr
	.endm

	fp_any	fp_vrintn_f32, "vrintn.f32 s2, s0"
	fp_any	fp_vrinta_f32, "vrinta.f32 s2, s0"
	fp_any	fp_vrintp_f32, "vrintp.f32 s2, s0"
	fp_any	fp_vrintm_f32, "vrintm.f32 s2, s0"
	fp_any	fp_vrintz_f32, "vrintz.f32 s2, s0"
	fp_any	fp_vrintx_f32, "vrintx.f32 s2, s0"
	fp_any	fp_vrintr_f32, "vrintr.f32 s2, s0"
	fp_any	fp_vcvta_s32_f32, "vcvta.s32.f32 s2, s0"
	fp_any	fp_vcvtn_s32_f32, "vcvtn.s32.f32 s2, s0"
	fp_any	fp_vcvtp_s32_f32, "vcvtp.s32.f32 s2, s0"
	fp_any	fp_vcvtm_s32_f32, "vcvtm.s32.f32 s2, s0"
	fp_any	fp_vmaxnm_f32, "vmaxnm.f32 s2, s0, s1"
	fp_any	fp_vminnm_f32, "vminnm.f32 s2, s0, s1"
	fp_any	fp_vmla_f32, "vmla.f32 s2, s0, s1"
	fp_any	fp_vcvt_f64_f32, "vcvt.f64.f32 d1, s0"
	fp_any	fp_vcvt_f32_s32, "vcvt.f32.s32 s2, s0"

@ ---- structure forms in blocks of their own ----
@ split_<form>_{arm,thumb}(void *p): one Advanced SIMD structure or permute
@ form on [r0] and a return, so the block that holds it holds nothing else.
@ fused_<form>_{arm,thumb}(void *p, const u64 d[32], u64 out[32]): the same
@ form between a full load and a full store of D0-D31 in one straight line.
	.macro	split_form name, insn:vararg
	func_arm split_\name\()_arm
	\insn
	bx	lr
	func_thumb split_\name\()_thumb
	\insn
	bx	lr
	func_arm fused_\name\()_arm
	vldmia	r1!, {d0-d15}
	vldmia	r1, {d16-d31}
	\insn
	vstmia	r2!, {d0-d15}
	vstmia	r2, {d16-d31}
	bx	lr
	func_thumb fused_\name\()_thumb
	vldmia	r1!, {d0-d15}
	vldmia	r1, {d16-d31}
	\insn
	vstmia	r2!, {d0-d15}
	vstmia	r2, {d16-d31}
	bx	lr
	.endm

	split_form vld1_8, vld1.8 {d0}, [r0]
	split_form vld2_16, vld2.16 {d2,d4}, [r0]
	split_form vld3_16, vld3.16 {d0,d2,d4}, [r0]
	split_form vld4_32, vld4.32 {d16-d19}, [r0]
	split_form vld1_lane, vld1.32 {d7[1]}, [r0]
	split_form vld2_lane, vld2.16 {d0[1],d1[1]}, [r0]
	split_form vld4_dup, vld4.16 {d0[],d1[],d2[],d3[]}, [r0]
	split_form vst1_64, vst1.64 {d8-d11}, [r0]
	split_form vst2_16, vst2.16 {d2,d4}, [r0]
	split_form vst3_8, vst3.8 {d0,d1,d2}, [r0]
	split_form vst4_32, vst4.32 {d16-d19}, [r0]
	split_form vst1_lane, vst1.8 {d3[5]}, [r0]
	split_form vzip_32, vzip.32 q0, q1
	split_form vtbl_8, vtbl.8 d0, {d1,d2}, d3
