/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for guest CMP of two registers, followed by
 * B.cond.
 *
 * G116's histogram over a full Geekbench run ranks this pair fourth and ninth
 * of 47,494 distinct pairs - 2.65% followed by B.eq and 0.84% by B.ne, 3.49%
 * together - while the immediate form that guest_compare_branch_pair_loop
 * measures does not reach the table at all. The two probes are deliberately
 * kept apart: they exercise different halves of the same instruction class, and
 * only this half is common in real workloads.
 *
 * Each iteration runs two CMP/B.cond pairs whose outcomes alternate with the
 * loop counter, so both directions of each branch are taken equally often and
 * the block-linking machinery sees a realistic edge rather than one perfectly
 * predicted target. The accumulator's final value depends on every branch
 * having gone the right way, so a run reporting OK has checked the lowering
 * rather than merely timed it.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 200000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x21, xzr			/* accumulator */

.Lloop:
	and	x2, x19, #1
	mov	x6, xzr
	cmp	x2, x6
	b.eq	.Leven
	add	x21, x21, #3
	b	.Lsecond
.Leven:
	add	x21, x21, #5

.Lsecond:
	and	x3, x19, #2
	mov	x7, #2
	cmp	x3, x7
	b.ne	.Lnext
	add	x21, x21, #7

.Lnext:
	subs	x19, x19, #1
	b.ne	.Lloop

	/* Every group of four iterations contributes 3+5+3+5 from the first pair
	 * and 7 twice from the second, so 16 + 14 = 30 per four iterations. The
	 * count is a multiple of four by construction. */
	movz	x4, #(ITERATIONS & 0xffff)
	movk	x4, #((ITERATIONS >> 16) & 0xffff), lsl #16
	lsr	x4, x4, #2
	mov	x5, #30
	mul	x4, x4, x5
	cmp	x21, x4
	b.ne	.Lfailure

	probe_finish

	probe_end
