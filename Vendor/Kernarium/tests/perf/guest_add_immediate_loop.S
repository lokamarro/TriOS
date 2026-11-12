/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for ordinary ADD-immediate gadgets.
 *
 * The loop body is the shared gadget_ceiling_add_immediate.inc, so this
 * guest run and the ideal gadget ceiling bench execute the same words.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 100000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

#include "gadget_ceiling_add_immediate.inc"

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, #1

	uml_gadget_ceiling_add_immediate_body .Lloop

	probe_finish	failure=0

	probe_end
