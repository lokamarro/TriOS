/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for G107's successor-fetch hoist.
 *
 * Every odd instruction is the exact same ADD-immediate encoding, so all
 * eight sites enter one host gadget and use one indirect BR instruction. Its
 * successor rotates through eight distinct gadget families: NOP, MOVZ, MOVK,
 * EOR, REV, CLZ, EXTR/ROR, and ADC. This is the polymorphic branch-target
 * shape that the monomorphic guest_add_immediate_loop cannot exercise.
 *
 * The ADD results form the only measured architectural result. The final
 * check catches a missing or repeated predecessor without putting another
 * dependency into any successor family; the focused host structure test owns
 * the complementary proof that all eight successor entries were selected.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 100000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr
	mov	x1, #1
	mov	x2, #2
	mov	x3, #3
	mov	x4, #4
	mov	x5, #5
	mov	x6, #6
	mov	x18, #1

.Lloop:
	add	x20, x20, #1
	nop
	add	x20, x20, #1
	movz	x0, #0x1234
	add	x20, x20, #1
	movk	x1, #0x5678, lsl #16
	add	x20, x20, #1
	eor	x2, x2, x3, lsl #1
	add	x20, x20, #1
	rev	x3, x3
	add	x20, x20, #1
	clz	x4, x4
	add	x20, x20, #1
	ror	x5, x5, #7
	add	x20, x20, #1
	adc	x6, x6, xzr
	/* Keep the loop counter outside the changed immediate family so the A/B
	 * measures only the eight polymorphic predecessor sites above. */
	subs	x19, x19, x18
	b.ne	.Lloop

	movz	x21, #(ITERATIONS & 0xffff)
	movk	x21, #((ITERATIONS >> 16) & 0xffff), lsl #16
	lsl	x21, x21, #3
	cmp	x20, x21
	b.ne	.Lfailure

	probe_finish

	probe_end
