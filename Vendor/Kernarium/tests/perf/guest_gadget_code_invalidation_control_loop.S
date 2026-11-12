/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cache-maintenance execution-boundary control for exact gadget validation.
 *
 * DC CVAU retains the sensitive probe's dispatch and ordering boundary but
 * does not advance the guest code-change sequence.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 5000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, xzr
	adrp	x2, .Lunrelated
	add	x2, x2, :lo12:.Lunrelated

.Lloop:
	add	x20, x20, #1
	dc	cvau, x2
	subs	x19, x19, #1
	b.ne	.Lloop

	movz	x21, #(ITERATIONS & 0xffff)
	movk	x21, #((ITERATIONS >> 16) & 0xffff), lsl #16
	cmp	x20, x21
	b.ne	.Lfailure

	probe_finish	nop_label=.Lunrelated

	probe_end
