/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for the cost of the guest entry boundary.
 *
 * Everything else in tests/perf measures guest code once the emulator is already
 * inside it. This probe measures the transition instead: a syscall leaves guest
 * execution, runs natively in the UML kernel, and re-enters, and each re-entry
 * resolves the calling thread's execution object twice - once from
 * darwin_uml_static_guest_interrupt_sequence() and once from run_common_guest().
 * A change to that resolution shows up here and nowhere else.
 *
 * getpid is deliberately the cheapest syscall the guest kernel offers. There is
 * no I/O, no allocation and no scheduling in it, so what the loop measures is
 * almost entirely the boundary rather than the work behind it - which is the
 * point, and also why the absolute number is meaningless on its own and only a
 * matched A/B of it means anything.
 *
 * Runs as parallel copies under --workers, since the resolution's cost is
 * partly contention on process-global state shared by every vCPU thread.
 *
 * The result is checked: every call must return the same positive pid, so a run
 * reporting OK has made all BENCH_ITERATIONS crossings and come back intact.
 */

#include "probe_harness.inc"

	.equ NR_GETPID,		172

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 2000000
#endif
	.equ ITERATIONS,	BENCH_ITERATIONS

	probe_start
	mov	x8, #NR_GETPID
	svc	#0
	cmp	x0, #0
	b.le	.Lfailure
	mov	x19, x0				/* expected pid */
	movz	x20, #(ITERATIONS & 0xffff)
	movk	x20, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Lloop:
	mov	x8, #NR_GETPID
	svc	#0
	cmp	x0, x19
	b.ne	.Lfailure
	subs	x20, x20, #1
	b.ne	.Lloop

	probe_finish

	probe_end
