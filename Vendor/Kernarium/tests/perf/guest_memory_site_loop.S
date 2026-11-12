/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux same-site memory throughput control.
 *
 * Each iteration revisits the same scalar and SIMD memory-instruction sites
 * and pages. This is deliberately focused on the production gadget TLB hot
 * path; the final scalar value prevents the loop from being dead work.
 */

#include "probe_harness.inc"

	.equ ITERATIONS,	500000000

	probe_start
	adrp	x0, data_page
	add	x0, x0, :lo12:data_page
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x22, #1
	dup	v1.2d, x22

.Lloop:
	ldr	x1, [x0]
	add	x1, x1, #1
	str	x1, [x0]
	ldrb	w2, [x0, #8]
	add	w2, w2, #1
	strb	w2, [x0, #8]
	ldr	q0, [x0, #16]
	add	v0.2d, v0.2d, v1.2d
	str	q0, [x0, #16]
	subs	x19, x19, #1
	b.ne	.Lloop

	ldr	x20, [x0]
	movz	x21, #(ITERATIONS & 0xffff)
	movk	x21, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x21
	b.ne	.Lfailure

	probe_finish

	probe_end

	.section .data
	.p2align 12
data_page:
	.quad 0
	.byte 0
	.zero 7
	.quad 0, 0
