/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Gadget stream-index reactivation probe.
 *
 * Reactivating a cached program re-derives its stream index entries, and that
 * walk is proportional to the size of the program rather than to the work the
 * guest asked for. The existing invalidation probes cannot show it: their timed
 * block is four instructions, so it spans one or two index granules and the
 * walk rounds away.
 *
 * This body is a full translation block of ordinary integer work, so a
 * reactivation walks many granules for the same single IC IVAU that triggered
 * it. guest_gadget_reindex_control_loop.S is the same block with DC CVAU, which
 * keeps the identical instruction mix and execution boundary while leaving the
 * guest code-change sequence alone, so the block stays linked and is never
 * reactivated.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 2000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr
	adrp	x2, .Lunrelated
	add	x2, x2, :lo12:.Lunrelated
	mov	x3, #1
	mov	x4, #2
	mov	x5, #3
	mov	x6, #4

.Lloop:
	add	x20, x20, #1
	ic	ivau, x2
	/* Fifty-six instructions of ordinary integer work, enough that the
	 * translated block spans many stream index granules. They are pure
	 * register arithmetic so the timed loop measures the reactivation walk
	 * rather than memory behaviour.
	 */
	.rept 14
	add	x3, x3, x4
	eor	x5, x5, x6
	sub	x4, x4, x3
	orr	x6, x6, x5
	.endr
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x21, #(ITERATIONS & 0xffff)
	movk	x21, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x21
	b.ne	.Lfailure

	probe_finish	nop_label=.Lunrelated

	probe_end
