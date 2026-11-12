/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Identical native/reference loops for translation-selected forwarding.
 * Every labeled body is exactly sixteen useful instructions followed by
 * SUBS/B.NE. All mixed bodies contain 4 ADD, 4 SUB, 4 EOR, 2 AND, and 2 ORR.
 * Symbols intentionally have no platform prefix; C++ binds with asm labels.
 */

.text

.macro begin_workload name
	.globl scalar_ceiling_native_\name
	.globl scalar_ceiling_body_\name
	.globl scalar_ceiling_end_\name
	.p2align 4
scalar_ceiling_native_\name:
	stp x19, x20, [sp, #-64]!
	stp x21, x22, [sp, #16]
	stp x23, x24, [sp, #32]
	stp x25, x26, [sp, #48]
	mov x19, x0
	ldp x20, x21, [x1]
	ldp x22, x23, [x1, #16]
	ldp x24, x25, [x1, #32]
	cbz x19, scalar_ceiling_end_\name
	.p2align 4
scalar_ceiling_body_\name:
.endm

.macro end_workload name
	subs x19, x19, #1
	b.ne scalar_ceiling_body_\name
scalar_ceiling_end_\name:
	stp x20, x21, [x1]
	stp x22, x23, [x1, #16]
	stp x24, x25, [x1, #32]
	eor x0, x20, x21
	eor x0, x0, x22
	eor x0, x0, x23
	eor x0, x0, x24
	eor x0, x0, x25
	ldp x25, x26, [sp, #48]
	ldp x23, x24, [sp, #32]
	ldp x21, x22, [sp, #16]
	ldp x19, x20, [sp], #64
	ret
.endm

begin_workload add_chain
	add x20, x20, #1
	add x20, x20, #2
	add x20, x20, #3
	add x20, x20, #4
	add x20, x20, #5
	add x20, x20, #6
	add x20, x20, #7
	add x20, x20, #8
	add x20, x20, #9
	add x20, x20, #10
	add x20, x20, #11
	add x20, x20, #12
	add x20, x20, #13
	add x20, x20, #14
	add x20, x20, #15
	add x20, x20, #16
end_workload add_chain

/* All fifteen adjacent useful-instruction edges carry a RAW dependency.
 * The first instruction also reads the preceding iteration's final result.
 */
begin_workload mixed_chain
	add x20, x23, x21
	eor x21, x20, x22
	sub x22, x21, x23
	add x23, x22, x24
	orr x24, x23, x25
	eor x25, x24, x20
	sub x20, x25, x21
	and x21, x20, x22
	add x22, x21, x23
	eor x23, x22, x24
	sub x24, x23, x25
	orr x25, x24, x20
	and x20, x25, x21
	add x21, x20, x22
	eor x22, x21, x23
	sub x23, x22, x24
end_workload mixed_chain

/* Alternate independent x20..x22 and x23..x25 chains. There are no RAW
 * dependencies between adjacent useful instructions, including loop wrap.
 */
begin_workload mixed_twochain
	add x20, x21, x22
	eor x23, x24, x25
	sub x21, x20, x22
	add x24, x23, x25
	orr x22, x21, x20
	eor x25, x24, x23
	sub x20, x22, x21
	and x23, x25, x24
	add x21, x20, x22
	eor x24, x23, x25
	sub x22, x21, x20
	orr x25, x24, x23
	and x20, x22, x21
	add x23, x25, x24
	eor x21, x20, x22
	sub x24, x23, x25
end_workload mixed_twochain

/* The same two independent chains, grouped to create four adjacent RAW
 * edges: 1->2, 3->4, 8->9, and 12->13 (4/15 within the useful body).
 */
begin_workload mixed_sparse
	add x20, x21, x22
	eor x21, x20, x22
	sub x23, x24, x25
	add x24, x23, x25
	orr x22, x21, x20
	eor x25, x24, x23
	sub x20, x22, x21
	and x23, x25, x24
	add x24, x23, x25
	eor x21, x20, x22
	sub x25, x24, x23
	orr x22, x21, x20
	and x20, x22, x21
	add x23, x25, x24
	eor x21, x20, x22
	sub x24, x23, x25
end_workload mixed_sparse

/* Every adjacent useful edge is dependent; four ADDs consume their
 * predecessor's result as both operands. Other operations keep distinct
 * inputs, avoiding identity/zero idioms in logical and SUB operations.
 */
begin_workload mixed_both
	add x20, x23, x23
	eor x21, x20, x22
	sub x22, x21, x23
	add x23, x22, x22
	orr x24, x23, x25
	eor x25, x24, x20
	sub x20, x25, x21
	and x21, x20, x22
	add x22, x21, x21
	eor x23, x22, x24
	sub x24, x23, x25
	orr x25, x24, x20
	and x20, x25, x21
	add x21, x20, x20
	eor x22, x21, x23
	sub x23, x22, x24
end_workload mixed_both

begin_workload add_independent
	add x20, x20, #1
	add x21, x21, #2
	add x22, x22, #3
	add x23, x23, #4
	add x24, x24, #5
	add x25, x25, #6
	add x20, x20, #7
	add x21, x21, #8
	add x22, x22, #9
	add x23, x23, #10
	add x24, x24, #11
	add x25, x25, #12
	add x20, x20, #13
	add x21, x21, #14
	add x22, x22, #15
	add x23, x23, #16
end_workload add_independent

/* Varied scalar transformations use the same dependency contract as plain
 * ALU operations. This is a control for mechanism generality, not a selector. */
begin_workload varied_chain
	add x20, x23, x21
	lsl x21, x20, x22
	ubfx x22, x21, #3, #17
	madd x23, x22, x24, x25
	eor x24, x23, x20, ror #7
	orr x25, x24, #0x00ff00ff00ff00ff
	rev x20, x25
	extr x21, x20, x24, #11
	subs x22, x21, x23
	csel x23, x22, x24, hi
	movk x23, #0xa55a, lsl #16
	add x24, x25, w23, sxtw #2
	lsr x25, x24, #9
	mul x20, x25, x21
	sdiv x21, x20, x22
	add x23, x21, #71
end_workload varied_chain

.purgem end_workload
.purgem begin_workload

#if !__APPLE__
.section .note.GNU-stack,"",%progbits
#endif
