/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux LSE atomic through a tagged pointer and nothing
 * else on its page: the cost of the atomic form itself, as
 * guest_atomic_add_loop.S measures it through an untagged one. Run it as
 * tbi_atomic_probe.inc says; with an argument the pointer is untagged.
 */

#include "probe_harness.inc"
#include "tbi_atomic_probe.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 10000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	tbi_probe_pointers
	mov	x4, #1
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	ldaddal	x4, x0, [x20]
	ldaddal	x4, x0, [x20]
	ldaddal	x4, x0, [x20]
	ldaddal	x4, x0, [x20]
	subs	x19, x19, #1
	b.ne	.Lloop

	/* The word started at 1 and gained 1 four times per pass. */
	ldr	x3, [x22]
	movz	x5, #(((ITERATIONS * 4) + 1) & 0xffff)
	movk	x5, #((((ITERATIONS * 4) + 1) >> 16) & 0xffff), lsl #16
	cmp	x3, x5
	b.ne	.Lfailure

	probe_finish

	probe_end

	tbi_probe_data
