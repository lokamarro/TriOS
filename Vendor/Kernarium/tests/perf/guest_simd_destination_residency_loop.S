/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Repeated integer vector MLA run for destination-residency experiments.
 * Thirty-two consecutive operations retain guest v0 as the destination;
 * v8/v9 are read-only sources and cannot alias that dirty value.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 20000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x22, x19
	movi	v0.4s, #0
	movi	v8.4s, #1
	movi	v9.4s, #1

.Lloop:
	.rept 32
	mla	v0.4s, v8.4s, v9.4s
	.endr
	subs	x19, x19, #1
	b.ne	.Lloop

	lsl	w20, w22, #5
	umov	w21, v0.s[0]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v0.s[1]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v0.s[2]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v0.s[3]
	cmp	w21, w20
	b.ne	.Lfailure

	probe_finish

	probe_end
