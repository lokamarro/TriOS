/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for unsigned-offset transfers whose
 * transferred register reaches CpuState by a baked displacement.
 *
 * Three independent load/modify/store chains run interleaved through one
 * same-page buffer. The three loads are issued together, then the three adds,
 * then the three stores, so no two consecutive words name the same register:
 * adjacent forwarding is never selected and every value crosses CpuState twice
 * on its way from the load that produced it to the store that consumes it.
 * That is the pair a baked displacement lets the host forward at rename, and
 * the shape a real workload makes whenever a loaded field is updated in place.
 *
 * Three chains rather than one because a single chain measures only latency;
 * with three the host has independent work to overlap, which is the condition
 * under which the addressing form of the hop decides throughput.
 *
 * x19 counts iterations, x20 is the checksum, x22 and x23 carry the other two
 * chains and are folded into x20 after the loop, and x21 holds the buffer base.
 * Twenty instructions per iteration.
 */

#include "probe_harness.inc"

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 30000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	adrp	x21, buffer
	add	x21, x21, :lo12:buffer
	mov	x20, #1
	mov	x22, #2
	mov	x23, #3
	str	x20, [x21, #0]
	str	x22, [x21, #8]
	str	x23, [x21, #16]

.Lloop:
	ldr	x20, [x21, #0]
	ldr	x22, [x21, #8]
	ldr	x23, [x21, #16]
	add	x20, x20, #1
	add	x22, x22, #2
	add	x23, x23, #3
	str	x20, [x21, #0]
	str	x22, [x21, #8]
	str	x23, [x21, #16]
	ldr	x20, [x21, #0]
	ldr	x22, [x21, #8]
	ldr	x23, [x21, #16]
	add	x20, x20, #4
	add	x22, x22, #5
	add	x23, x23, #6
	str	x20, [x21, #0]
	str	x22, [x21, #8]
	str	x23, [x21, #16]
	subs	x19, x19, #1
	b.ne	.Lloop

	eor	x20, x20, x22
	eor	x20, x20, x23

	probe_finish	failure=0

	probe_end

	.section .bss
	.align 6
buffer:
	.space 64
