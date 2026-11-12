/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux LSE atomic through a tagged pointer next to a plain
 * store to the same page. Both use the write half of the page's TLB entry, so
 * an engine that keys the entry by the address as presented makes every
 * access evict the other's key. Run it as tbi_atomic_probe.inc says; with an
 * argument the pointer is untagged and the two never alias.
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
	ldadd	x4, x0, [x20]
	str	x4, [x21, #16]
	ldadd	x4, x0, [x20]
	str	x4, [x21, #16]
	subs	x19, x19, #1
	b.ne	.Lloop

	/* The word the atomic updates started at 1 and gained 1 twice per pass. */
	ldr	x3, [x22]
	movz	x5, #(((ITERATIONS * 2) + 1) & 0xffff)
	movk	x5, #((((ITERATIONS * 2) + 1) >> 16) & 0xffff), lsl #16
	cmp	x3, x5
	b.ne	.Lfailure

	probe_finish

	probe_end

	tbi_probe_data
