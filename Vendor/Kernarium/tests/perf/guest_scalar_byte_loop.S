/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux register-offset scalar byte-transfer control.
 *
 * One-byte accesses cannot cross a guest page. The loop isolates the
 * production scalar byte gadget's valid TLB-hit path.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 200000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	adrp	x0, data_page
	add	x0, x0, :lo12:data_page
	mov	x2, xzr
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	ldrb	w1, [x0, x2]
	add	w1, w1, #1
	strb	w1, [x0, x2]
	subs	x19, x19, #1
	b.ne	.Lloop

	ldrb	w20, [x0]
	mov	w21, #(ITERATIONS & 0xff)
	cmp	w20, w21
	b.ne	.Lfailure

	probe_finish

	probe_end

	.section .data
	.p2align 12
data_page:
	.byte 0
