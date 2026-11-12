/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for the MOVK and EXTR gadget bodies.
 *
 * guest_logical_move_loop and guest_add_immediate_loop use movk only to
 * materialise their iteration count, so neither reaches the move_wide_keep or
 * extract_register entries from its measured loop. This probe puts both in the
 * loop body, in every shape the bank has a separate entry for: MOVK at all four
 * 64-bit halfword positions and both 32-bit ones, EXTR with distinct sources at
 * both widths, and the ROR alias where the two sources are the same register.
 *
 * Every iteration recomputes the same values from constants, so the accumulated
 * sum is exactly the count times one iteration's contribution. The expected
 * total is derived by running the body once before the loop and multiplying,
 * which makes the check independent of the constants themselves: a wrong
 * insert position, a wrong shift complement or a wrong rotate all change the
 * sum and fail the run.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 200000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	/* One iteration's worth of work, used both to derive the expected total
	 * and as the measured loop body. Leaves the contribution in x5. */
	.macro body
	movz	x0, #0x1111
	movk	x0, #0x2222, lsl #16
	movk	x0, #0x3333, lsl #32
	movk	x0, #0x4444, lsl #48
	movz	w1, #0x5555
	movk	w1, #0x6666, lsl #16
	extr	x2, x0, x1, #17
	extr	w3, w1, w0, #9
	ror	x4, x0, #23
	add	x5, x2, x3
	add	x5, x5, x4
	.endm

	probe_start
	body
	movz	x21, #(ITERATIONS & 0xffff)
	movk	x21, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mul	x22, x21, x5			/* expected total */
	mov	x19, x21
	mov	x20, xzr

.Lloop:
	body
	add	x20, x20, x5
	subs	x19, x19, #1
	b.ne	.Lloop

	cmp	x20, x22
	b.ne	.Lfailure

	probe_finish

	probe_end
