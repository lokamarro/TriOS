/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AVX-VNNI dot products: VPDPBUSD, VPDPBUSDS, VPDPWSSD and VPDPWSSDS. */

/* v`output` = each dword of v`accumulator` plus the sum of the four products
 * of the unsigned bytes of v`left` and the signed bytes of v`right` (0x50,
 * wrapping, and 0x51, saturating), or of the two products of their signed
 * words (0x52, wrapping, and 0x53, saturating). The saturation applies to the
 * exact sum. Each byte product fits 16 bits and a pair of word products
 * overflows 32 only at 0x80000000, so the byte forms sum in 32 bits and the
 * saturating word form in 64. */
.macro vector_dot_lane opcode, output, accumulator, left, right
	.if \opcode == 0x50 || \opcode == 0x51
	uxtl v16.8h, v\left\().8b
	uxtl2 v17.8h, v\left\().16b
	sxtl v18.8h, v\right\().8b
	sxtl2 v19.8h, v\right\().16b
	mul v16.8h, v16.8h, v18.8h
	mul v17.8h, v17.8h, v19.8h
	saddlp v16.4s, v16.8h
	saddlp v17.4s, v17.8h
	addp v16.4s, v16.4s, v17.4s
	.if \opcode == 0x50
	add v\output\().4s, v\accumulator\().4s, v16.4s
	.else
	sqadd v\output\().4s, v\accumulator\().4s, v16.4s
	.endif
	.else
	smull v16.4s, v\left\().4h, v\right\().4h
	smull2 v17.4s, v\left\().8h, v\right\().8h
	.if \opcode == 0x52
	addp v16.4s, v16.4s, v17.4s
	add v\output\().4s, v\accumulator\().4s, v16.4s
	.else
	saddlp v16.2d, v16.4s
	saddlp v17.2d, v17.4s
	saddw v16.2d, v16.2d, v\accumulator\().2s
	saddw2 v17.2d, v17.2d, v\accumulator\().4s
	sqxtn v\output\().2s, v16.2d
	sqxtn2 v\output\().4s, v17.2d
	.endif
	.endif
.endm

/* The destination's old value is in v0 (and v3), vvvv in v1 (v4) and r/m in
 * v2 (v5); the result goes to v6 (v7). */
.macro vector_dot_body opcode, width
	vector_dot_lane \opcode, 6, 0, 1, 2
	.if \width == 256
	vector_dot_lane \opcode, 7, 3, 4, 5
	.endif
.endm

/* A VEX write: a 128-bit result zeroes the register's upper half. */
.macro vector_dot_write width, descriptor
	and x9, x\descriptor, #0xffff
	add x9, _vcpu, x9
	str q6, [x9]
	.if \width == 256
	str q7, [x9, #16]
	.else
	movi v16.16b, #0
	str q16, [x9, #16]
	.endif
.endm

.macro vector_dot_register opcode, width
gadget vector_dot_\opcode\()_\width\()_rr
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	add x11, _vcpu, x11
	ldr q0, [x9]
	ldr q1, [x10]
	ldr q2, [x11]
	.if \width == 256
	ldr q3, [x9, #16]
	ldr q4, [x10, #16]
	ldr q5, [x11, #16]
	.endif
	vector_dot_body 0x\opcode, \width
	vector_dot_write \width, 8
	retire_and_next
.endm

.macro vector_dot_memory opcode, width
gadget vector_dot_\opcode\()_\width\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	vector_memory_source (\width / 8), Lx86_vector_dot_cross_fault\@, 2, 5
	and x9, x23, #0xffff
	and x10, x26, #0xffff
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	ldr q0, [x9]
	ldr q1, [x10]
	.if \width == 256
	ldr q3, [x9, #16]
	ldr q4, [x10, #16]
	.endif
	vector_dot_body 0x\opcode, \width
	vector_dot_write \width, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_dot_cross_fault\@
.endm

.macro vector_dot_family opcode, width
	vector_dot_register \opcode, \width
	vector_dot_memory \opcode, \width
.endm

vector_dot_family 50, 128
vector_dot_family 50, 256
vector_dot_family 51, 128
vector_dot_family 51, 256
vector_dot_family 52, 128
vector_dot_family 52, 256
vector_dot_family 53, 128
vector_dot_family 53, 256

.purgem vector_dot_family
.purgem vector_dot_memory
.purgem vector_dot_register
.purgem vector_dot_write
.purgem vector_dot_body
.purgem vector_dot_lane
