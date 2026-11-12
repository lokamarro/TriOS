/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux one-register structure-memory benchmark.
 *
 * Four lane loads update one guest vector and four lane stores read one guest
 * vector on every iteration.  This isolates the cost of keeping that vector
 * in a translation-time-selected host register instead of staging it through
 * a fixed temporary for every instruction.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 20000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	sub	sp, sp, #64
	mov	x13, sp
	add	x14, sp, #4
	add	x15, sp, #8
	add	x16, sp, #12

	mov	w0, #1
	mov	w1, #2
	mov	w2, #3
	mov	w3, #4
	str	w0, [x13]
	str	w1, [x14]
	str	w2, [x15]
	str	w3, [x16]

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lload_loop:
	ld1	{v8.s}[0], [x13]
	ld1	{v8.s}[1], [x14]
	ld1	{v8.s}[2], [x15]
	ld1	{v8.s}[3], [x16]
	subs	x19, x19, #1
	b.ne	.Lload_loop

	umov	w0, v8.s[0]
	umov	w1, v8.s[1]
	umov	w2, v8.s[2]
	umov	w3, v8.s[3]
	cmp	w0, #1
	b.ne	.Lfailure
	cmp	w1, #2
	b.ne	.Lfailure
	cmp	w2, #3
	b.ne	.Lfailure
	cmp	w3, #4
	b.ne	.Lfailure

	mov	w4, #5
	mov	w5, #6
	mov	w6, #7
	mov	w7, #8
	str	w4, [sp, #16]
	str	w5, [sp, #20]
	str	w6, [sp, #24]
	str	w7, [sp, #28]
	ldr	q9, [sp, #16]

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lstore_loop:
	st1	{v9.s}[0], [x13]
	st1	{v9.s}[1], [x14]
	st1	{v9.s}[2], [x15]
	st1	{v9.s}[3], [x16]
	subs	x19, x19, #1
	b.ne	.Lstore_loop

	ldr	w0, [x13]
	ldr	w1, [x14]
	ldr	w2, [x15]
	ldr	w3, [x16]
	cmp	w0, #5
	b.ne	.Lfailure
	cmp	w1, #6
	b.ne	.Lfailure
	cmp	w2, #7
	b.ne	.Lfailure
	cmp	w3, #8
	b.ne	.Lfailure

	add	sp, sp, #64
	probe_finish	failure_stack=64

	probe_end

	.section .note.GNU-stack, "", %progbits
