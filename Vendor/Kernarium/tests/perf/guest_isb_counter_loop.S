/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux probe for the ISB, a context synchronization
 * point both engines check against the address space's code-change
 * sequence. Each iteration is ISB then MRS CNTVCT_EL0, the counter read
 * arm64 Linux's vDSO makes without FEAT_ECV and that spin-wait hints and
 * userspace timers make directly.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 20000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mov	x20, #0

.Lloop:
	isb
	mrs	x9, cntvct_el0
	add	x20, x20, x9
	subs	x19, x19, #1
	b.ne	.Lloop

	probe_finish	failure=0

	probe_end
