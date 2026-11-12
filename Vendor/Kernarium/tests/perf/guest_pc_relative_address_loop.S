/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for the ADRP + ADD address idiom.
 *
 * `adrp xN, sym` followed by `add xN, xN, #:lo12:sym` is how position-independent
 * code names anything at all, and G113's translation-time histogram measured it
 * as the third most frequent adjacent gadget pair in real translated code, at
 * 1.5% of all pairs - behind only ADRP+LDR and a load-pair repeat, and far ahead
 * of the conditional shapes.
 *
 * Nothing in tests/perf exercised it in a loop: the existing probes use the
 * idiom once each, to find their success message on the way out.
 *
 * Four independent addresses per iteration, so the measurement is not dominated
 * by a single dependency chain, and the addresses are summed rather than
 * dereferenced so the loop measures address materialisation rather than the
 * memory path. The expected total is derived by running the body once and
 * multiplying, which makes the check independent of where the linker put the
 * symbols.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 100000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	.macro body
	adrp	x0, anchor_a
	add	x0, x0, :lo12:anchor_a
	adrp	x1, anchor_b
	add	x1, x1, :lo12:anchor_b
	adrp	x2, anchor_c
	add	x2, x2, :lo12:anchor_c
	adrp	x3, anchor_d
	add	x3, x3, :lo12:anchor_d
	add	x5, x0, x1
	add	x5, x5, x2
	add	x5, x5, x3
	.endm

	probe_start
	body
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mul	x22, x19, x5			/* expected total */
	mov	x20, xzr

.Lloop:
	body
	add	x20, x20, x5
	subs	x19, x19, #1
	b.ne	.Lloop

	cmp	x20, x22
	b.ne	.Lfailure

	probe_finish

	probe_end

	/* Spread across pages so the four ADRPs do not all fold to one page. */
	.section .bss
	.p2align 14
anchor_a:
	.space 16384
anchor_b:
	.space 16384
anchor_c:
	.space 16384
anchor_d:
	.space 16
