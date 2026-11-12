/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Entry and exit harness shared by the freestanding guest_*.S probes. It
 * defines the two system calls the harness makes and three macros, and emits
 * no code until a probe expands them.
 *
 * probe_start opens the text section at the global _start entry point.
 *
 * probe_finish writes success_message to standard output and exits 0, with a
 * trap behind the exit. Its arguments:
 *   length         the bytes of success_message to write, three for "OK\n";
 *   failure        0 for a probe that cannot fail; otherwise it defines the
 *                  .Lfailure label, which exits 1, for the probe's checks;
 *   failure_stack  the stack bytes the failure path releases before exiting;
 *   nop_label      a label on a NOP between the exit and the trap, which the
 *                  probe can address and never executes.
 *
 * probe_end closes _start and emits the "OK\n" success_message.
 */

/* A probe with a longer system call table defines these two in it first. */
	.ifndef NR_WRITE
	.equ NR_WRITE,		64
	.endif
	.ifndef NR_EXIT_GROUP
	.equ NR_EXIT_GROUP,	94
	.endif

.macro probe_start
	.section .text
	.global _start
	.type _start, %function
_start:
.endm

.macro probe_finish length=3, failure=1, failure_stack=0, nop_label
	adrp	x1, success_message
	add	x1, x1, :lo12:success_message
	mov	x0, #1
	mov	x2, #\length
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #0
.if \failure
	b	.Lexit

.Lfailure:
.if \failure_stack
	add	sp, sp, #\failure_stack
.endif
	mov	x0, #1
.Lexit:
.endif
	mov	x8, #NR_EXIT_GROUP
	svc	#0
.ifnb \nop_label
\nop_label:
	nop
.endif
	brk	#0
.endm

.macro probe_end
	.size _start, .-_start

	.section .rodata
success_message:
	.ascii "OK\n"
.endm
