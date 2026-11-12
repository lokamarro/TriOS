/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Callable AArch64 workloads shared by the native and ideal gadget ceilings.
 *
 * Each labeled body is a gadget_ceiling_*.inc macro that the matching
 * guest_*_loop.S probe includes as well, so the Builder input, the native
 * CPU's instruction stream and the real-guest probe cannot drift.
 *
 * Calling convention: x0 iterations, x1 a zeroed 64 KiB page-aligned buffer,
 * x2 two path counters written back; the checksum is returned. Loop state is
 * x19 counter, x20 checksum, x21 buffer, x22 cursor, x23 hash, x24/x25 path
 * counters and v16-v18 accumulators, all zero except the counter and buffer.
 */

#include "gadget_ceiling_add_immediate.inc"
#include "gadget_ceiling_scalar_mix.inc"
#include "gadget_ceiling_simd_mix.inc"

	.text

.macro ceiling_workload name, body
	.globl uml_gadget_ceiling_native_\name
	.globl uml_gadget_ceiling_\name\()_begin
	.globl uml_gadget_ceiling_\name\()_end
	.p2align 6
uml_gadget_ceiling_native_\name:
	stp	x19, x20, [sp, #-64]!
	stp	x21, x22, [sp, #16]
	stp	x23, x24, [sp, #32]
	stp	x25, x26, [sp, #48]
	mov	x19, x0
	mov	x21, x1
	mov	x26, x2
	mov	x20, xzr
	mov	x22, xzr
	mov	x23, xzr
	mov	x24, xzr
	mov	x25, xzr
	movi	v16.16b, #0
	movi	v17.16b, #0
	movi	v18.16b, #0
	\body uml_gadget_ceiling_\name\()_begin, Lfail_\name
uml_gadget_ceiling_\name\()_end:
	stp	x24, x25, [x26]
	mov	x0, x20
	ldp	x25, x26, [sp, #48]
	ldp	x23, x24, [sp, #32]
	ldp	x21, x22, [sp, #16]
	ldp	x19, x20, [sp], #64
	ret
Lfail_\name:
	brk	#0
.endm

ceiling_workload add_immediate, uml_gadget_ceiling_add_immediate_body
ceiling_workload scalar_mix, uml_gadget_ceiling_scalar_mix_body
ceiling_workload simd_mix, uml_gadget_ceiling_simd_mix_body
