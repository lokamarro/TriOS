/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux branch-link throughput control.
 *
 * The conditional backward edge remains entirely in translated guest code,
 * making this a focused measurement of cached gadget-block successor links.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 300000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr
	mov	x21, xzr

.Lloop:
	add	x20, x20, #1
	eor	x21, x21, x20
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x22, #(ITERATIONS & 0xffff)
	movk	x22, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x22
	b.ne	.Lfailure

	probe_finish

	probe_end
