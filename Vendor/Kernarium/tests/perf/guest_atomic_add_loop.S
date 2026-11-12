/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux scalar atomic translation control.
 *
 * Each process owns its counter. Parallel copies therefore measure direct
 * lowering and vCPU scaling without introducing guest-visible contention.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 500000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	adrp	x20, counter
	add	x20, x20, :lo12:counter
	mov	x1, #1
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	ldaddal	x1, x0, [x20]
	subs	x19, x19, #1
	b.ne	.Lloop

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
