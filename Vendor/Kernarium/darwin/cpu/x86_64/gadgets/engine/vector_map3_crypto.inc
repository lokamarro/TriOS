/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PCLMULQDQ, SHA1RNDS4, and AESKEYGENASSIST native arm64 gadgets. */

.arch_extension crypto

.macro vector_map3_pclmul_lane output, left, right
	tbz w13, #0, Lx86_vector_map3_pclmul_left_low\@
	dup v6.2d, v\left\().d[1]
	b Lx86_vector_map3_pclmul_left_done\@
Lx86_vector_map3_pclmul_left_low\@:
	dup v6.2d, v\left\().d[0]
Lx86_vector_map3_pclmul_left_done\@:
	tbz w13, #4, Lx86_vector_map3_pclmul_right_low\@
	dup v7.2d, v\right\().d[1]
	b Lx86_vector_map3_pclmul_right_done\@
Lx86_vector_map3_pclmul_right_low\@:
	dup v7.2d, v\right\().d[0]
Lx86_vector_map3_pclmul_right_done\@:
	pmull v\output\().1q, v6.1d, v7.1d
.endm

.macro vector_map3_pclmul_body width
	vector_map3_pclmul_lane 4, 0, 1
	.if \width == 256
	vector_map3_pclmul_lane 5, 2, 3
	.endif
.endm

.macro vector_map3_pclmul_register width
gadget vector_map3_pclmul_\width\()_rr
	ldp x8, x13, [_stream], #16
	vector_read_pair \width
	vector_map3_pclmul_body \width
	vector_write_xmm \width, 8
	retire_and_next
.endm

.macro vector_map3_pclmul_memory width
gadget vector_map3_pclmul_\width\()_rm
	effective_address
	ldp x26, x27, [_stream], #16
	mov x25, x7
	vector_memory_source (\width / 8), Lx86_vector_map3_pclmul_cross_fault\@
	mov w13, w27
	and x10, x26, #0xffff
	add x10, _vcpu, x10
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
	vector_map3_pclmul_body \width
	vector_write_xmm \width, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_map3_pclmul_cross_fault\@
.endm

vector_map3_pclmul_register 128
vector_map3_pclmul_memory 128
vector_map3_pclmul_register 256
vector_map3_pclmul_memory 256

.purgem vector_map3_pclmul_memory
.purgem vector_map3_pclmul_register
.purgem vector_map3_pclmul_body
.purgem vector_map3_pclmul_lane

.macro vector_map3_sha1_function output, b, c, d
	cmp w14, #0
	b.eq Lx86_vector_map3_sha1_choice\@
	cmp w14, #2
	b.eq Lx86_vector_map3_sha1_majority\@
	eor w\output, w\b, w\c
	eor w\output, w\output, w\d
	b Lx86_vector_map3_sha1_function_done\@
Lx86_vector_map3_sha1_choice\@:
	eor w15, w\c, w\d
	and w15, w15, w\b
	eor w\output, w15, w\d
	b Lx86_vector_map3_sha1_function_done\@
Lx86_vector_map3_sha1_majority\@:
	eor w15, w\b, w\c
	and w15, w15, w\d
	and w\output, w\b, w\c
	eor w\output, w\output, w15
Lx86_vector_map3_sha1_function_done\@:
.endm

.macro vector_map3_sha1_round source_lane, first
	vector_map3_sha1_function 9, 1, 2, 3
	ror w10, w0, #27
	add w9, w9, w10
	add w9, w9, w12
	mov w10, w4
	.if !\first
	mov w11, v1.s[\source_lane]
	add w10, w10, w11
	.endif
	add w9, w9, w10
	mov w4, w3
	mov w3, w2
	ror w2, w1, #2
	mov w1, w0
	mov w0, w9
.endm

.macro vector_map3_sha1_body
	and w14, w13, #3
	mov w12, #0x7999
	movk w12, #0x5a82, lsl #16
	cmp w14, #1
	b.ne Lx86_vector_map3_sha1_not_parity_1\@
	mov w12, #0xeba1
	movk w12, #0x6ed9, lsl #16
	b Lx86_vector_map3_sha1_constant_ready\@
Lx86_vector_map3_sha1_not_parity_1\@:
	cmp w14, #2
	b.ne Lx86_vector_map3_sha1_not_majority\@
	mov w12, #0xbcdc
	movk w12, #0x8f1b, lsl #16
	b Lx86_vector_map3_sha1_constant_ready\@
Lx86_vector_map3_sha1_not_majority\@:
	cmp w14, #3
	b.ne Lx86_vector_map3_sha1_constant_ready\@
	mov w12, #0xc1d6
	movk w12, #0xca62, lsl #16
Lx86_vector_map3_sha1_constant_ready\@:
	mov w0, v0.s[3]
	mov w1, v0.s[2]
	mov w2, v0.s[1]
	mov w3, v0.s[0]
	mov w4, v1.s[3]
	vector_map3_sha1_round 3, 1
	vector_map3_sha1_round 2, 0
	vector_map3_sha1_round 1, 0
	vector_map3_sha1_round 0, 0
	mov v4.s[3], w0
	mov v4.s[2], w1
	mov v4.s[1], w2
	mov v4.s[0], w3
.endm

gadget vector_map3_sha1rnds4_rr
	ldp x8, x13, [_stream], #16
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	ldr q0, [_vcpu, x10]
	ldr q1, [_vcpu, x11]
	vector_map3_sha1_body
	vector_write_xmm 128, 8
	retire_and_next

gadget vector_map3_sha1rnds4_rm
	effective_address
	ldp x26, x27, [_stream], #16
	mov x25, x7
	vector_memory_source 16, Lx86_vector_map3_sha1_cross_fault, frame=16
	mov w13, w27
	and x10, x26, #0xffff
	ldr q0, [_vcpu, x10]
	vector_map3_sha1_body
	vector_write_xmm 128, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_map3_sha1_cross_fault, 16

.purgem vector_map3_sha1_body
.purgem vector_map3_sha1_round
.purgem vector_map3_sha1_function

/* Each source word is replicated across the state, so ShiftRows moves
 * nothing and AESE with a zero key is SubWord in every lane. RotWord is a
 * shift and a shift-insert per lane, the round constant is broadcast, and
 * three zips interleave the four result words; nothing crosses to the
 * general registers, whose round trips serialized the old sequence. */
.macro vector_map3_aeskeygen_body
	dup v4.4s, v1.s[1]
	dup v5.4s, v1.s[3]
	movi v6.16b, #0
	aese v4.16b, v6.16b
	aese v5.16b, v6.16b
	dup v6.4s, w13
	ushr v7.4s, v4.4s, #8
	sli v7.4s, v4.4s, #24
	eor v7.16b, v7.16b, v6.16b
	zip1 v4.4s, v4.4s, v7.4s
	ushr v7.4s, v5.4s, #8
	sli v7.4s, v5.4s, #24
	eor v7.16b, v7.16b, v6.16b
	zip1 v5.4s, v5.4s, v7.4s
	zip1 v4.2d, v4.2d, v5.2d
.endm

gadget vector_map3_aeskeygen_rr
	ldp x8, x13, [_stream], #16
	ubfx x10, x8, #16, #16
	ldr q1, [_vcpu, x10]
	vector_map3_aeskeygen_body
	vector_write_xmm 128, 8
	retire_and_next

gadget vector_map3_aeskeygen_rm
	effective_address
	ldr x27, [_stream], #8
	mov x25, x7
	vector_memory_source 16, Lx86_vector_map3_aeskeygen_cross_fault, frame=16
	mov w13, w27
	vector_map3_aeskeygen_body
	vector_write_xmm 128, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_map3_aeskeygen_cross_fault, 16

.purgem vector_map3_aeskeygen_body
