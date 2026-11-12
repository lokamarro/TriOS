/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux compare-and-retry control whose load result and
 * store status intentionally share one guest register. This is the common
 * library shape which must stay on the coordinated semantic path.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 50000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	adrp	x20, counter
	add	x20, x20, :lo12:counter
	mov	x2, #0
	mov	x3, #1
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lretry:
	ldxr	x0, [x20]
	cmp	x0, x2
	b.ne	.Lcompared
	stxr	w0, x3, [x20]
	cbnz	w0, .Lretry
.Lcompared:
	b.ne	.Lfailure
	add	x2, x2, #1
	add	x3, x3, #1
	subs	x19, x19, #1
	b.ne	.Lretry

	ldr	x4, [x20]
	movz	x5, #(ITERATIONS & 0xffff)
	movk	x5, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x4, x5
	b.ne	.Lfailure

	probe_finish

	probe_end

	.section .data
	.balign 16
counter:
	.xword 0
