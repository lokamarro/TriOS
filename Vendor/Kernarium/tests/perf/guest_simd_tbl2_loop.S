/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unaffected two-table TBL control for one-table offset specialization. */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 300000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	movi	v1.16b, #7
	movi	v2.16b, #9
	movi	v3.16b, #0

.Lloop:
	tbl	v0.16b, {v1.16b-v2.16b}, v3.16b
	subs	x19, x19, #1
	b.ne	.Lloop

	umov	w20, v0.b[0]
	cmp	w20, #7
	b.ne	.Lfailure
	probe_finish

	probe_end
