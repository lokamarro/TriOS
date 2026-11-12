// SPDX-License-Identifier: GPL-2.0-or-later
/* clone(2) for a thread: the new task runs fn(arg) on its own stack and
 * exits with its result. The wrapper is shared by every group that starts
 * threads. */
	.syntax	unified
	.text
	.arm
	.balign	4
@ long a32_clone(long (*fn)(void *), void *stack_top, unsigned flags,
@                void *arg, unsigned tls, int *parent_tid, int *child_tid)
@ Returns the new thread id (or -errno) in the parent.
	.globl	a32_clone
	.type	a32_clone, %function
a32_clone:
	push	{r4-r7, lr}
	ldr	r4, [sp, #28]		@ child_tid
	mov	r5, r0			@ fn: survives in the child
	mov	r6, r3			@ arg
	mov	r0, r2			@ flags
	ldr	r2, [sp, #24]		@ parent_tid
	ldr	r3, [sp, #20]		@ tls
	mov	r7, #120		@ clone
	svc	#0
	cmp	r0, #0
	bne	1f
	mov	r0, r6
	blx	r5
	mov	r7, #1			@ exit
	svc	#0
	udf	#0
1:	pop	{r4-r7, pc}
