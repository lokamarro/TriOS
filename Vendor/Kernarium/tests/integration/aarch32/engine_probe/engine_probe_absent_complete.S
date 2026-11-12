// SPDX-License-Identifier: GPL-2.0-or-later
/* Group absent: one instruction of each extension per routine, in A32 and
 * T32, to run where the extension is not advertised and has to be UNDEFINED.
 * r0 is a valid address and r1 a value. */
	.syntax	unified
	.text

	.macro	ab_arm name, insn
	.arm
	.balign	4
	.globl	\name\()_arm
	.type	\name\()_arm, %function
\name\()_arm:
\insn
	bx	lr
	.endm

	.macro	ab_thumb name, insn
	.thumb
	.balign	4
	.thumb_func
	.globl	\name\()_thumb
	.type	\name\()_thumb, %function
\name\()_thumb:
\insn
	bx	lr
	.endm

	.macro	ab name, insn
	ab_arm	\name, "\insn"
	ab_thumb \name, "\insn"
	.endm

@ VFP
	ab	ab_vadd, "vadd.f32 s0, s1, s2"
	ab	ab_vmrs, "vmrs r2, fpscr"
	ab	ab_vmsr, "vmsr fpscr, r1"
	ab	ab_vldr, "vldr d0, [r0]"
	ab	ab_vmov_core, "vmov s0, r1"
	ab	ab_vcvt, "vcvt.f64.f32 d0, s0"
@ Advanced SIMD
	ab	ab_vadd_i32, "vadd.i32 q0, q1, q2"
	ab	ab_vld1, "vld1.32 {d0}, [r0]"
	ab	ab_vmov_imm, "vmov.i32 q0, #1"
	ab	ab_vdup, "vdup.32 q0, r1"
@ Armv8 floating point additions (part of the VFP row)
	ab	ab_vrintn, "vrintn.f32 s0, s1"
	ab	ab_vmaxnm, "vmaxnm.f32 s0, s1, s2"
@ Armv8.2 and later extensions, the build enables them all for this file
	ab	ab_vsdot, "vsdot.s8 d0, d1, d2"
	ab	ab_fp16_add, "vadd.f16 s0, s1, s2"
	ab	ab_fp16_vec, "vadd.f16 d0, d1, d2"
	ab	ab_vfmal, "vfmal.f16 d0, s1, s2"
	ab	ab_bf16_dot, "vdot.bf16 d0, d1, d2"
	ab	ab_i8mm, "vusdot.s8 d0, d1, d2"
@ Crypto and CRC32
	ab	ab_aese, "aese.8 q0, q1"
	ab	ab_aesd, "aesd.8 q0, q1"
	ab	ab_aesmc, "aesmc.8 q0, q1"
	ab	ab_aesimc, "aesimc.8 q0, q1"
	ab	ab_pmull, "vmull.p64 q0, d2, d3"
	ab	ab_sha1c, "sha1c.32 q0, q1, q2"
	ab	ab_sha1h, "sha1h.32 q0, q1"
	ab	ab_sha256h, "sha256h.32 q0, q1, q2"
	ab	ab_crc32b, "crc32b r2, r1, r1"
	ab	ab_crc32cw, "crc32cw r2, r1, r1"
