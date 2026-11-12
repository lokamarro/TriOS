/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux load-acquire through a tagged pointer and nothing
 * else on its page: the cost of the ordered form itself, which an engine that
 * strips the tag in line pays on every access and one that keys the TLB by
 * the tagged page pays only once. Run it as tbi_atomic_probe.inc says; with
 * an argument the pointer is untagged.
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
	add	x3, x3, x0
	ldar	x0, [x20]
	add	x3, x3, x0
	ldar	x0, [x20]
	add	x3, x3, x0
	ldar	x0, [x20]
	add	x3, x3, x0
	subs	x19, x19, #1
	b.ne	.Lloop

	/* Each iteration adds four loads of 1. */
	movz	x4, #((ITERATIONS * 4) & 0xffff)
	movk	x4, #(((ITERATIONS * 4) >> 16) & 0xffff), lsl #16
	cmp	x3, x4
	b.ne	.Lfailure

	probe_finish

	probe_end

	tbi_probe_data
