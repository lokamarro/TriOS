/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux same-page integer pair-transfer control.
 *
 * The adjacent 64-bit transfers isolate the production integer-pair
 * gadget's TLB-hit path.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 200000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	adrp	x0, data_page
	add	x0, x0, :lo12:data_page
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	ldp	x1, x2, [x0]
	add	x1, x1, #1
	stp	x1, x2, [x0]
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
	.quad 0, 0
