/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux native FP-environment throughput control.
 *
 * FMUL by 1.0 keeps the expected result stable while exercising one native
 * floating-point gadget and its environment guard per iteration.
 */

#include "probe_harness.inc"

	.equ ITERATIONS,	500000000

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	movz	x20, #0x3ff0, lsl #48
	fmov	d0, x20
	fmov	d1, x20

.Lloop:
	fmul	d0, d0, d1
	subs	x19, x19, #1
	b.ne	.Lloop

	fmov	x21, d0
	cmp	x21, x20
	b.ne	.Lfailure

	probe_finish

	probe_end
