/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux FP/SIMD unsigned-offset memory-run benchmark.
 *
 * Each loop contains four consecutive 128-bit transfers through one unchanged
 * allocated base.  It isolates sharing one same-page permission translation
 * while retaining each guest load/store as one register-remapped native
 * instruction.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 20000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	sub	sp, sp, #128
	mov	x13, sp
	add	x14, sp, #64

	movi	v0.4s, #1
	movi	v1.4s, #2
	movi	v2.4s, #3
	movi	v3.4s, #4
	str	q0, [x13]
	str	q1, [x13, #16]
	str	q2, [x13, #32]
	str	q3, [x13, #48]

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lload_loop:
	ldr	q8, [x13]
	ldr	q9, [x13, #16]
	ldr	q10, [x13, #32]
	ldr	q11, [x13, #48]
	subs	x19, x19, #1
	b.ne	.Lload_loop

	umov	w0, v8.s[0]
	umov	w1, v9.s[0]
	umov	w2, v10.s[0]
	umov	w3, v11.s[0]
	cmp	w0, #1
	b.ne	.Lfailure
	cmp	w1, #2
	b.ne	.Lfailure
	cmp	w2, #3
	b.ne	.Lfailure
	cmp	w3, #4
	b.ne	.Lfailure

	movi	v12.4s, #5
	movi	v13.4s, #6
	movi	v14.4s, #7
	movi	v15.4s, #8
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lstore_loop:
	str	q12, [x14]
	str	q13, [x14, #16]
	str	q14, [x14, #32]
	str	q15, [x14, #48]
	subs	x19, x19, #1
	b.ne	.Lstore_loop

	ldr	w0, [x14]
	ldr	w1, [x14, #16]
	ldr	w2, [x14, #32]
	ldr	w3, [x14, #48]
	cmp	w0, #5
	b.ne	.Lfailure
	cmp	w1, #6
	b.ne	.Lfailure
	cmp	w2, #7
	b.ne	.Lfailure
	cmp	w3, #8
	b.ne	.Lfailure

	add	sp, sp, #128
	probe_finish	failure_stack=128

	probe_end

	.section .note.GNU-stack, "", %progbits
