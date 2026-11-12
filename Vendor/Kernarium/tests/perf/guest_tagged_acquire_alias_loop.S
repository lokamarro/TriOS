/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux load-acquire through a tagged pointer next to a
 * plain load of the same page. Both use the read half of the page's TLB entry,
 * so an engine that keys the entry by the address as presented makes every
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
	mov	x3, xzr
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	ldar	x0, [x20]
	ldr	x1, [x21, #8]
	add	x3, x3, x0
	add	x3, x3, x1
	ldar	x0, [x20]
	ldr	x1, [x21, #8]
	add	x3, x3, x0
	add	x3, x3, x1
	subs	x19, x19, #1
	b.ne	.Lloop

	/* Each iteration adds two loads of 1 and two of 2. */
	movz	x4, #((ITERATIONS * 6) & 0xffff)
	movk	x4, #(((ITERATIONS * 6) >> 16) & 0xffff), lsl #16
	cmp	x3, x4
	b.ne	.Lfailure

	probe_finish

	probe_end

	tbi_probe_data
