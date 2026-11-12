/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for the logical-register MOV alias.
 *
 * Every measured MOV encodes as ORR Xd, XZR, Xm. The dependency ring keeps
 * all results live while one linked branch is amortized over sixteen gadgets.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 100000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, #1
	mov	x21, #2
	mov	x22, #3
	mov	x23, #4

.Lloop:
	mov	x20, x21
	mov	x21, x22
	mov	x22, x23
	mov	x23, x20
	mov	x20, x22
	mov	x22, x21
	mov	x21, x23
	mov	x23, x20
	mov	x20, x21
	mov	x21, x22
	mov	x22, x23
	mov	x23, x20
	mov	x20, x22
	mov	x22, x21
	mov	x21, x23
	mov	x23, x20
	subs	x19, x19, #1
	b.ne	.Lloop

	probe_finish	failure=0

	probe_end
