/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Matched control for the gadget link-table reset scope probe.
 *
 * Identical to guest_gadget_link_population_loop, including the chain walk
 * that leaves BENCH_LINK_TARGETS named branch targets behind, except that the
 * timed loop executes DC CVAU. That keeps the same cache-maintenance execution
 * boundary while leaving the guest code-change sequence alone, so no link reset
 * ever runs and the standing population is never walked. A change to how the
 * reset is scoped must move the sensitive probe and leave this one still.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 500000
#endif
#ifndef BENCH_LINK_TARGETS
#define BENCH_LINK_TARGETS 4096
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	b	.Lchain_last

/* Each block branches to the instruction before it, so the target always lies
 * behind the translator's forward fetch window and has to become a link rather
 * than being folded into the same block. Entering at the end therefore walks
 * the chain backwards, translating BENCH_LINK_TARGETS distinct blocks and
 * naming that many distinct targets.
 */
.Lchain_exit:
	b	.Lhot_entry
	.rept (BENCH_LINK_TARGETS - 1)
	b	. - 4
	.endr
.Lchain_last:
	b	. - 4

.Lhot_entry:
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
