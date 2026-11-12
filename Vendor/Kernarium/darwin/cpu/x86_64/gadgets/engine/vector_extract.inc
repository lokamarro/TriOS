/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SIMD sign-mask and word-extraction operations. */

.macro vector_movmsk width, element
gadget vector_movmsk_\element\()_\width
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	add x10, _vcpu, x10
	mov x11, xzr
	mov x12, xzr
Lx86_vector_movmsk_loop\@:
	.if \element == 32
	ldr w14, [x10, x12, lsl #2]
	lsr w14, w14, #31
	.else
	ldr x14, [x10, x12, lsl #3]
	lsr x14, x14, #63
	.endif
	lsl x14, x14, x12
	orr x11, x11, x14
	add x12, x12, #1
	cmp x12, #(\width / \element)
	b.lo Lx86_vector_movmsk_loop\@
	str x11, [_vcpu, x9]
	retire_and_next
.endm

vector_movmsk 128, 32
vector_movmsk 128, 64
vector_movmsk 256, 32
vector_movmsk 256, 64

.purgem vector_movmsk

.macro vector_pmovmskb width
gadget vector_pmovmskb_\width
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	add x10, _vcpu, x10
	/* Collapse one sign bit per byte into a bit mask by folding pairs of
	 * lanes at doubling widths: each shift-and-accumulate step packs the bits
	 * of two neighbouring lanes into the lower one, so after four steps byte 0
	 * holds the low eight bits and byte 8 the high eight.
	 */
	.if \width == 64
	ldr d0, [x10]
	ushr v0.8b, v0.8b, #7
	usra v0.4h, v0.4h, #7
	usra v0.2s, v0.2s, #14
	usra d0, d0, #28
	umov w11, v0.b[0]
	.else
	ldr q0, [x10]
	ushr v0.16b, v0.16b, #7
	usra v0.8h, v0.8h, #7
	usra v0.4s, v0.4s, #14
	usra v0.2d, v0.2d, #28
	umov w11, v0.b[0]
	umov w12, v0.b[8]
	orr w11, w11, w12, lsl #8
	.endif
	.if \width == 256
	ldr q1, [x10, #16]
	ushr v1.16b, v1.16b, #7
	usra v1.8h, v1.8h, #7
	usra v1.4s, v1.4s, #14
	usra v1.2d, v1.2d, #28
	umov w13, v1.b[0]
	umov w14, v1.b[8]
	orr w13, w13, w14, lsl #8
	orr w11, w11, w13, lsl #16
	.endif
	str x11, [_vcpu, x9]
	retire_and_next
.endm

vector_pmovmskb 64
vector_pmovmskb 128
vector_pmovmskb 256

.purgem vector_pmovmskb

.macro vector_pextrw width
gadget vector_pextrw_\width
	ldp x8, x11, [_stream], #16
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	and x11, x11, #(\width / 16 - 1)
	add x10, _vcpu, x10
	ldrh w12, [x10, x11, lsl #1]
	str x12, [_vcpu, x9]
	retire_and_next
.endm

vector_pextrw 64
vector_pextrw 128
vector_pextrw 256

.purgem vector_pextrw

/* PINSRW and VPINSRW: copy the source register, whose state offset x10 holds,
 * over the destination at offset x9 with the 16-bit `word` stored to element
 * x11, and synchronize an MMX destination with the x87 registers. */
.macro vector_insert_word width, vex, word
	add x9, _vcpu, x9
	add x10, _vcpu, x10
	.if \width == 64
	ldr x14, [x10]
	str x14, [x9]
	.else
	ldr q0, [x10]
	str q0, [x9]
	.endif
	strh \word, [x9, x11, lsl #1]
	.ifc \vex,_vex
	movi v0.16b, #0
	str q0, [x9, #16]
	.endif
	.if \width == 64
	ldr x14, [x9]
	sub x12, x9, _vcpu
	synchronize_mmx x12, x14, x12, x15, w13
	.endif
	retire_and_next
.endm

.macro vector_insert_register width, vex
gadget vector_insert_\width\()_rr\vex
	ldp x8, x11, [_stream], #16
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ubfx x12, x8, #32, #16
	ldrh w13, [_vcpu, x12]
	and x11, x11, #(\width / 16 - 1)
	vector_insert_word \width, \vex, w13
.endm

vector_insert_register 64,
vector_insert_register 128,
vector_insert_register 128, _vex

.purgem vector_insert_register

.macro vector_insert_memory width, vex
gadget vector_insert_\width\()_rm\vex
	effective_address
	mov x25, x7
	read_operand 2, ldrh, w, 27, w
	ldr x24, [_stream], #8
	and x9, x23, #0xffff
	and x10, x24, #0xffff
	ubfx x11, x24, #16, #8
	and x11, x11, #(\width / 16 - 1)
	vector_insert_word \width, \vex, w27
.endm

vector_insert_memory 64,
vector_insert_memory 128,
vector_insert_memory 128, _vex

.purgem vector_insert_memory
.purgem vector_insert_word
