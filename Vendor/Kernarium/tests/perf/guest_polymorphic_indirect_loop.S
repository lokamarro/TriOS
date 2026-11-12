/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux polymorphic indirect-call control.
 *
 * Thirty-two equally spaced targets rotate through one BLR site, far more
 * than any per-site cell array holds: the shape of a virtual call or an
 * interpreter's handler dispatch, and of most of the executor round trips
 * left in a Geekbench 6 single-core run. Each target adds its own constant,
 * so a skipped or repeated target changes the checked sum.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 10000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS
	.equ TARGETS,		32
	/* Every full rotation adds 1 + 2 + ... + TARGETS. */
	.equ EXPECTED,		(ITERATIONS / TARGETS) * (TARGETS * (TARGETS + 1) / 2)

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	w21, wzr
	mov	x20, xzr
	adr	x23, .Ltarget0

.Lloop:
	add	x22, x23, x21, lsl #4
	blr	x22
	add	w21, w21, #1
	and	w21, w21, #(TARGETS - 1)
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x21, #(EXPECTED & 0xffff)
	movk	x21, #((EXPECTED >> 16) & 0xffff), lsl #16
	movk	x21, #((EXPECTED >> 32) & 0xffff), lsl #32
	cmp	x20, x21
	b.ne	.Lfailure

	probe_finish

	.p2align 4
.Ltarget0:
	.set increment, 1
	.rept TARGETS
	.p2align 4
	add	x20, x20, #increment
	ret
	.set increment, increment + 1
	.endr

	probe_end
