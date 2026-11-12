// SPDX-License-Identifier: GPL-2.0-or-later
/* Group cacheflush: position-independent routines the group copies into a
 * writable and executable mapping. Each rewrites an instruction a few words
 * after itself, flushes it with the cacheflush system call, and runs it. */
	.syntax	unified
	.text

@ long cf_self_arm(u32 instruction): patch `mov r0, #1` with the instruction.
	.arm
	.balign	4
	.globl	cf_self_arm_start
cf_self_arm_start:
	push	{r4, r7, lr}
	adr	r1, 1f
	str	r0, [r1]
	mov	r0, r1
	add	r1, r1, #4
	mov	r2, #0
	movw	r7, #2
	movt	r7, #0xf
	svc	#0
1:	mov	r0, #1
	pop	{r4, r7, pc}
	.globl	cf_self_arm_end
cf_self_arm_end:

@ long cf_self_thumb(u16 instruction): patch `movs r0, #1` likewise.
	.thumb
	.balign	4
	.globl	cf_self_thumb_start
cf_self_thumb_start:
	push	{r4, r7, lr}
	adr	r1, 1f
	strh	r0, [r1]
	mov	r0, r1
	adds	r1, r1, #2
	movs	r2, #0
	movw	r7, #2
	movt	r7, #0xf
	svc	#0
	.balign	4
1:	movs	r0, #1
	nop
	pop	{r4, r7, pc}
	.globl	cf_self_thumb_end
cf_self_thumb_end:
