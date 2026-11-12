/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux FP/SIMD successor-link throughput control.
 *
 * After the first iteration, the cached loop target contains SIMD state and a
 * conditional self-edge. This isolates whether vector blocks remain inside
 * the assembly engine or return through C++ at every iteration.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	movi	v0.4s, #0
	movi	v1.4s, #1

.Lloop:
	add	v0.4s, v0.4s, v1.4s
	subs	x19, x19, #1
	b.ne	.Lloop

	umov	w23, v0.s[0]
	movz	w22, #(ITERATIONS & 0xffff)
	movk	w22, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	w23, w22
	b.ne	.Lfailure

	probe_finish

	probe_end
