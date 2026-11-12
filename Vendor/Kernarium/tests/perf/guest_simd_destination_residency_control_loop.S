/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for destination residency. The timed topology is identical
 * to guest_simd_destination_residency_loop.S, but destination v8 is outside
 * the experimental v0-v7 bank and sources v16/v17 remain read-only.
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
	movi	v8.4s, #0
	movi	v16.4s, #1
	movi	v17.4s, #1

.Lloop:
	.rept 32
	mla	v8.4s, v16.4s, v17.4s
	.endr
	subs	x19, x19, #1
	b.ne	.Lloop

	lsl	w20, w22, #5
	umov	w21, v8.s[0]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v8.s[1]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v8.s[2]
	cmp	w21, w20
	b.ne	.Lfailure
	umov	w21, v8.s[3]
	cmp	w21, w20
	b.ne	.Lfailure

	probe_finish

	probe_end
