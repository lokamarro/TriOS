/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for guest CMP-immediate followed by B.cond.
 *
 * That pair is the ordinary shape of a conditional in compiled code, and it is
 * the one the gadget engine pays most for: B.cond ends a translated block, so
 * unfused the pair costs a full threaded dispatch between the compare and the
 * branch plus a round trip of the guest flags through a system register in each
 * direction.
 *
 * No existing probe measures it. The loop counters in guest_add_immediate_loop
 * and its siblings are `subs xN, xN, #1` with a real destination register, which
 * is deliberately *not* the fused shape - fusion requires the XZR destination
 * that makes the instruction a CMP - so those probes exercise the unfused path
 * no matter what the translator does with CMP.
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
	cmp	x2, #0
	b.eq	.Leven
	add	x21, x21, #3
	b	.Lsecond
.Leven:
	add	x21, x21, #5

.Lsecond:
	and	x3, x19, #2
	cmp	x3, #2
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
