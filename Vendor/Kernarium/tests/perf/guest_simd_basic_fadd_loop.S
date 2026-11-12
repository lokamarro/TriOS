/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux vector FADD throughput control.
 *
 * Fixed zero sources keep the result exact while isolating the translated
 * register-addressing overhead of the basic SIMD floating-point family.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 300000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	movi	v1.16b, #0
	movi	v2.16b, #0

.Lloop:
	fadd	v0.4s, v1.4s, v2.4s
	subs	x19, x19, #1
	b.ne	.Lloop

	umov	x20, v0.d[0]
	cbnz	x20, .Lfailure
	umov	x20, v0.d[1]
	cbnz	x20, .Lfailure

	probe_finish

	probe_end
