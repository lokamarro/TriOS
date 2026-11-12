/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux native FP-environment run-throughput control.
 *
 * Sixteen dependent FMULs share one translated block.  This distinguishes a
 * program-entry environment check from an instruction-local guard while the
 * multiply-by-one result remains exactly self-checking.
 */

#include "probe_harness.inc"

	.equ ITERATIONS,	400000000

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	movz	x20, #0x3ff0, lsl #48
	fmov	d0, x20
	fmov	d1, x20

.Lloop:
	.rept 16
	fmul	d0, d0, d1
	.endr
	subs	x19, x19, #1
	b.ne	.Lloop

	fmov	x21, d0
	cmp	x21, x20
	b.ne	.Lfailure

	probe_finish

	probe_end
