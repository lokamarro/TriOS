/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux FP plus polymorphic indirect-branch control.
 *
 * Three equally spaced targets rotate through one BLR site.  The production
 * indirect successor cache has two entries, so the branch deliberately exits
 * the assembly engine on every steady-state iteration.  Each target executes
 * native SIMD first, reproducing the frequent FP-environment leave pattern
 * observed in multi-core Geekbench HDR.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 10000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	w21, wzr
	movi	v0.16b, #0
	movi	v1.16b, #0
	adr	x23, .Ltarget0

.Lloop:
	add	x22, x23, x21, lsl #4
	blr	x22
	add	w21, w21, #1
	cmp	w21, #3
	csel	w21, wzr, w21, eq
	subs	x19, x19, #1
	b.ne	.Lloop

	umov	x20, v0.d[0]
	cbnz	x20, .Lfailure
	umov	x20, v0.d[1]
	cbnz	x20, .Lfailure

	probe_finish

	.p2align 4
.Ltarget0:
	fadd	v0.4s, v0.4s, v1.4s
	ret
	.p2align 4
.Ltarget1:
	fadd	v0.4s, v0.4s, v1.4s
	ret
	.p2align 4
.Ltarget2:
	fadd	v0.4s, v0.4s, v1.4s
	ret

	probe_end
