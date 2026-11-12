// SPDX-License-Identifier: GPL-2.0-or-later
/* Group complete: CRC32, the AES, PMULL, SHA-1 and SHA-256 instructions, the
 * dot product, half-precision arithmetic and FMLAL, each as a routine the C++
 * side checks against a known answer. The instruction sets are enabled for
 * the assembler by the build (Armv8.2-A with crypto, crc, dotprod, fp16 and
 * fp16fml); the group runs only where the HWCAP words advertise them. */
	.syntax	unified
	.text

	.macro	func_arm name
	.arm
	.balign	4
	.globl	\name\()_arm
	.type	\name\()_arm, %function
\name\()_arm:
	.endm

	.macro	func_thumb name
	.thumb
	.balign	4
	.thumb_func
	.globl	\name\()_thumb
	.type	\name\()_thumb, %function
\name\()_thumb:
	.endm

@ ---- u32 crc32{,c}_{b,h,w}(u32 crc, const u8 *p, u32 n) ----
@ The unit-size instruction over the whole units, the byte one over the tail.
	.macro	crc_body ld, unit, step, tail
1:	cmp	r2, #\unit
	blo	2f
	\ld	r3, [r1], #\unit
	\step	r0, r0, r3
	sub	r2, r2, #\unit
	b	1b
2:	cmp	r2, #0
	beq	4f
3:	ldrb	r3, [r1], #1
	\tail	r0, r0, r3
	subs	r2, r2, #1
	bne	3b
4:	bx	lr
	.endm

	.macro	crc_pair name, ld, unit, step, tail
	func_arm \name
	crc_body \ld, \unit, \step, \tail
	func_thumb \name
	crc_body \ld, \unit, \step, \tail
	.endm

	crc_pair crc32_b, ldrb, 1, crc32b, crc32b
	crc_pair crc32_h, ldrh, 2, crc32h, crc32b
	crc_pair crc32_w, ldr, 4, crc32w, crc32b
	crc_pair crc32c_b, ldrb, 1, crc32cb, crc32cb
	crc_pair crc32c_h, ldrh, 2, crc32ch, crc32cb
	crc_pair crc32c_w, ldr, 4, crc32cw, crc32cb

@ ---- AES: aes_encrypt(rk[11][16], in, out), aes_decrypt(dk[11][16], in, out),
@ aes_imc(in, out). dk is rk with InvMixColumns applied to rk[1..9]. ----
	.macro	aes_encrypt_body
	vld1.8	{d0, d1}, [r1]
	mov	r3, #9
1:	vld1.8	{d2, d3}, [r0]!
	aese.8	q0, q1
	aesmc.8	q0, q0
	subs	r3, r3, #1
	bne	1b
	vld1.8	{d2, d3}, [r0]!
	aese.8	q0, q1
	vld1.8	{d2, d3}, [r0]
	veor	q0, q0, q1
	vst1.8	{d0, d1}, [r2]
	bx	lr
	.endm

	.macro	aes_decrypt_body
	vld1.8	{d0, d1}, [r1]
	add	r0, r0, #160
	vld1.8	{d2, d3}, [r0]
	sub	r0, r0, #16
	aesd.8	q0, q1
	aesimc.8 q0, q0
	mov	r3, #8
1:	vld1.8	{d2, d3}, [r0]
	sub	r0, r0, #16
	aesd.8	q0, q1
	aesimc.8 q0, q0
	subs	r3, r3, #1
	bne	1b
	vld1.8	{d2, d3}, [r0]
	sub	r0, r0, #16
	aesd.8	q0, q1
	vld1.8	{d2, d3}, [r0]
	veor	q0, q0, q1
	vst1.8	{d0, d1}, [r2]
	bx	lr
	.endm

	.macro	aes_imc_body
	vld1.8	{d0, d1}, [r0]
	aesimc.8 q0, q0
	vst1.8	{d0, d1}, [r1]
	bx	lr
	.endm

	func_arm aes_encrypt
	aes_encrypt_body
	func_thumb aes_encrypt
	aes_encrypt_body
	func_arm aes_decrypt
	aes_decrypt_body
	func_thumb aes_decrypt
	aes_decrypt_body
	func_arm aes_imc
	aes_imc_body
	func_thumb aes_imc
	aes_imc_body

@ ---- void pmull64(const u64 ab[2], u64 out[2]) ----
	.macro	pmull_body
	vld1.64	{d2, d3}, [r0]
	vmull.p64 q0, d2, d3
	vst1.64	{d0, d1}, [r1]
	bx	lr
	.endm

	func_arm pmull64
	pmull_body
	func_thumb pmull64
	pmull_body

@ ---- SHA-1 ----
@ void sha1_schedule(const u32 w[16], u32 out[80]): W16..W79 by SHA1SU0/SU1.
	.macro	sha1_schedule_body
	vld1.32	{d8, d9}, [r0]!
	vld1.32	{d10, d11}, [r0]!
	vld1.32	{d12, d13}, [r0]!
	vld1.32	{d14, d15}, [r0]
	vst1.32	{d8, d9}, [r1]!
	vst1.32	{d10, d11}, [r1]!
	vst1.32	{d12, d13}, [r1]!
	vst1.32	{d14, d15}, [r1]!
	mov	r3, #4
1:	sha1su0.32 q4, q5, q6
	sha1su1.32 q4, q7
	vst1.32	{d8, d9}, [r1]!
	sha1su0.32 q5, q6, q7
	sha1su1.32 q5, q4
	vst1.32	{d10, d11}, [r1]!
	sha1su0.32 q6, q7, q4
	sha1su1.32 q6, q5
	vst1.32	{d12, d13}, [r1]!
	sha1su0.32 q7, q4, q5
	sha1su1.32 q7, q6
	vst1.32	{d14, d15}, [r1]!
	subs	r3, r3, #1
	bne	1b
	bx	lr
	.endm

@ void sha1_rounds(u32 state[5], const u32 *wk, u32 groups, u32 type):
@ `groups` groups of four rounds with SHA1C (type 0), SHA1P (1) or SHA1M (2);
@ wk already has the round constant added.
	.macro	sha1_rounds_body
	vld1.32	{d0, d1}, [r0]
	ldr	r12, [r0, #16]
	vmov.i32 q2, #0
	vmov.32	d4[0], r12
	cmp	r3, #1
	beq	20f
	bhi	30f
10:	vld1.32	{d6, d7}, [r1]!
	sha1h.32 q1, q0
	sha1c.32 q0, q2, q3
	vmov	q2, q1
	subs	r2, r2, #1
	bne	10b
	b	40f
20:	vld1.32	{d6, d7}, [r1]!
	sha1h.32 q1, q0
	sha1p.32 q0, q2, q3
	vmov	q2, q1
	subs	r2, r2, #1
	bne	20b
	b	40f
30:	vld1.32	{d6, d7}, [r1]!
	sha1h.32 q1, q0
	sha1m.32 q0, q2, q3
	vmov	q2, q1
	subs	r2, r2, #1
	bne	30b
40:	vst1.32	{d0, d1}, [r0]
	vmov.32	r12, d4[0]
	str	r12, [r0, #16]
	bx	lr
	.endm

	func_arm sha1_schedule
	sha1_schedule_body
	func_thumb sha1_schedule
	sha1_schedule_body
	func_arm sha1_rounds
	sha1_rounds_body
	func_thumb sha1_rounds
	sha1_rounds_body

@ ---- SHA-256 ----
@ void sha256_schedule(const u32 w[16], u32 out[64]): W16..W63.
	.macro	sha256_schedule_body
	vld1.32	{d16, d17}, [r0]!
	vld1.32	{d18, d19}, [r0]!
	vld1.32	{d20, d21}, [r0]!
	vld1.32	{d22, d23}, [r0]
	vst1.32	{d16, d17}, [r1]!
	vst1.32	{d18, d19}, [r1]!
	vst1.32	{d20, d21}, [r1]!
	vst1.32	{d22, d23}, [r1]!
	mov	r3, #3
1:	sha256su0.32 q8, q9
	sha256su1.32 q8, q10, q11
	vst1.32	{d16, d17}, [r1]!
	sha256su0.32 q9, q10
	sha256su1.32 q9, q11, q8
	vst1.32	{d18, d19}, [r1]!
	sha256su0.32 q10, q11
	sha256su1.32 q10, q8, q9
	vst1.32	{d20, d21}, [r1]!
	sha256su0.32 q11, q8
	sha256su1.32 q11, q9, q10
	vst1.32	{d22, d23}, [r1]!
	subs	r3, r3, #1
	bne	1b
	bx	lr
	.endm

@ void sha256_rounds(u32 state[8], const u32 *wk, u32 groups): wk has the
@ round constants added.
	.macro	sha256_rounds_body
	vld1.32	{d0, d1}, [r0]!
	vld1.32	{d2, d3}, [r0]
	sub	r0, r0, #16
1:	vld1.32	{d6, d7}, [r1]!
	vmov	q2, q0
	sha256h.32 q0, q1, q3
	sha256h2.32 q1, q2, q3
	subs	r2, r2, #1
	bne	1b
	vst1.32	{d0, d1}, [r0]!
	vst1.32	{d2, d3}, [r0]
	bx	lr
	.endm

	func_arm sha256_schedule
	sha256_schedule_body
	func_thumb sha256_schedule
	sha256_schedule_body
	func_arm sha256_rounds
	sha256_rounds_body
	func_thumb sha256_rounds
	sha256_rounds_body

@ ---- dot product: void dot_{s8,u8,s8_lane,u8_lane}(u32 acc[4], const u8 a[16], const u8 b[16]) ----
	.macro	dot_body insn
	vld1.32	{d0, d1}, [r0]
	vld1.8	{d2, d3}, [r1]
	vld1.8	{d4, d5}, [r2]
	\insn
	vst1.32	{d0, d1}, [r0]
	bx	lr
	.endm

	func_arm dot_s8
	dot_body "vsdot.s8 q0, q1, q2"
	func_thumb dot_s8
	dot_body "vsdot.s8 q0, q1, q2"
	func_arm dot_u8
	dot_body "vudot.u8 q0, q1, q2"
	func_thumb dot_u8
	dot_body "vudot.u8 q0, q1, q2"
	func_arm dot_s8_lane
	dot_body "vsdot.s8 q0, q1, d4[1]"
	func_thumb dot_s8_lane
	dot_body "vsdot.s8 q0, q1, d4[1]"

@ ---- half precision: u32 fp16_{add,mul}(u32 a, u32 b) scalar, fp16_add_vec(u16 out[4], a[4], b[4]) ----
	.macro	fp16_scalar_body insn
	vmov	s0, r0
	vmov	s1, r1
	\insn
	vmov	r0, s2
	uxth	r0, r0
	bx	lr
	.endm

	func_arm fp16_add
	fp16_scalar_body "vadd.f16 s2, s0, s1"
	func_thumb fp16_add
	fp16_scalar_body "vadd.f16 s2, s0, s1"
	func_arm fp16_mul
	fp16_scalar_body "vmul.f16 s2, s0, s1"
	func_thumb fp16_mul
	fp16_scalar_body "vmul.f16 s2, s0, s1"

	.macro	fp16_vec_body
	vld1.16	{d0}, [r1]
	vld1.16	{d1}, [r2]
	vadd.f16 d2, d0, d1
	vst1.16	{d2}, [r0]
	bx	lr
	.endm

	func_arm fp16_add_vec
	fp16_vec_body
	func_thumb fp16_add_vec
	fp16_vec_body

@ void fhm_fmlal(float acc[2], u32 a, u32 b): acc += halves(a) * halves(b)
	.macro	fhm_body
	vld1.32	{d2}, [r0]
	vmov	s0, r1
	vmov	s1, r2
	vfmal.f16 d2, s0, s1
	vst1.32	{d2}, [r0]
	bx	lr
	.endm

	func_arm fhm_fmlal
	fhm_body
	func_thumb fhm_fmlal
	fhm_body
