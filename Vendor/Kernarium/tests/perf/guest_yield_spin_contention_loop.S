/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux contended spin-lock control for guest YIELD
 * lowering.
 *
 * Guest YIELD is the hint glibc's aarch64 atomic_spin_nop() and the kernel's
 * cpu_relax() emit while waiting on a lock. Arm defines its only architectural
 * effect on symmetric multi-threading hardware, which Apple Silicon does not
 * have, so lowering it to a host YIELD is a complete no-op and a waiting guest
 * thread keeps emulating its spin loop at full speed for its whole host
 * quantum.
 *
 * The existing probes cannot see that. Every one of them either owns its data
 * (guest_atomic_add_loop) or runs as independent parallel copies, so no thread
 * ever waits on another and the hint is never reached under contention. This
 * probe creates the missing condition directly: BENCH_THREADS threads share one
 * address space and contend for a single test-and-set lock, so at any moment
 * one thread is inside the critical section and every other is in the YIELD
 * backoff. Wall time is then governed by how much host throughput the waiters
 * consume while the holder works, which is exactly what the lowering changes.
 *
 * Run with --workers 1. The probe generates its own contention; parallel copies
 * would measure host oversubscription instead of the hint.
 *
 * The unlocked read-modify-write in the critical section is deliberate. It is
 * safe only if the lock is correct, so the final count doubles as a proof that
 * mutual exclusion actually held for every one of the measured acquisitions.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT,		93
	.equ NR_EXIT_GROUP,	94
	.equ NR_CLONE,		220
	.equ NR_WAIT4,		260

	/* Share the address space but stay an ordinary child so wait4 reaps it. */
	.equ CLONE_VM,		0x00000100
	.equ SIGCHLD,		17
	.equ CLONE_FLAGS,	(CLONE_VM | SIGCHLD)

#ifndef BENCH_THREADS
#define BENCH_THREADS 10
#endif
/* Sized so a default run lasts several seconds. At 50,000 the whole run took
 * 0.21 s, which is far too short to separate a hint lowering from start-up
 * jitter. */
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 1000000
#endif
	.equ THREADS,		BENCH_THREADS
	.equ ITERATIONS,	BENCH_ITERATIONS
	.equ STACK_SIZE,	16384

#include "probe_harness.inc"

	probe_start
	mov	x21, #0				/* children already started */

.Lspawn:
	cmp	x21, #(THREADS - 1)
	b.ge	.Lrun_self
	/* Child stack top = stacks + (index + 1) * STACK_SIZE. Index zero stays
	 * reserved for the parent, which keeps the stack the kernel gave it. */
	adrp	x1, stacks
	add	x1, x1, :lo12:stacks
	add	x22, x21, #1
	mov	x2, #STACK_SIZE
	madd	x1, x22, x2, x1
	mov	x0, #CLONE_FLAGS
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x8, #NR_CLONE
	svc	#0
	cmp	x0, #0
	b.lt	.Lfailure
	b.eq	.Lchild
	add	x21, x21, #1
	b	.Lspawn

.Lchild:
	bl	.Lwork
	mov	x0, #0
	mov	x8, #NR_EXIT
	svc	#0
	brk	#0

.Lrun_self:
	bl	.Lwork
	mov	x20, #0
.Lwait_loop:
	cmp	x20, #(THREADS - 1)
	b.ge	.Lverify
	mov	x0, #-1				/* any child */
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	mov	x8, #NR_WAIT4
	svc	#0
	cmp	x0, #0
	b.lt	.Lfailure
	add	x20, x20, #1
	b	.Lwait_loop

.Lverify:
	adrp	x24, counter
	add	x24, x24, :lo12:counter
	ldr	x4, [x24]
	mov	x5, #THREADS
	movz	x6, #(ITERATIONS & 0xffff)
	movk	x6, #((ITERATIONS >> 16) & 0xffff), lsl #16
	mul	x5, x5, x6
	cmp	x4, x5
	b.ne	.Lfailure

	probe_finish

	/* ITERATIONS acquire/release cycles against the shared lock. */
	.type .Lwork, %function
.Lwork:
	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
	adrp	x23, lock
	add	x23, x23, :lo12:lock
	adrp	x24, counter
	add	x24, x24, :lo12:counter

.Lwork_loop:
.Lacquire:
	ldaxr	w0, [x23]
	cbnz	w0, .Lcontended
	mov	w1, #1
	stxr	w2, w1, [x23]
	cbnz	w2, .Lacquire
	b	.Lheld

	/* Drop the reservation before waiting, then poll with the hint under
	 * test until the lock looks free and the acquire is worth retrying. */
.Lcontended:
	clrex
.Lspin:
	yield
	ldr	w0, [x23]
	cbnz	w0, .Lspin
	b	.Lacquire

.Lheld:
	ldr	x0, [x24]
	add	x0, x0, #1
	str	x0, [x24]
	stlr	wzr, [x23]

	subs	x19, x19, #1
	b.ne	.Lwork_loop
	ret
	.size .Lwork, .-.Lwork

	probe_end

	/* Keep the lock and the counter on separate 64-byte lines so the
	 * measurement reflects contention for the lock itself rather than false
	 * sharing between the lock and the value it protects. */
	.section .data
	.balign 64
lock:
	.word 0
	.balign 64
counter:
	.quad 0

	.section .bss
	.balign 16
stacks:
	.space THREADS * STACK_SIZE
