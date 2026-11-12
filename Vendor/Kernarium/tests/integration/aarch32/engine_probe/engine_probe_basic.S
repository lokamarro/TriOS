// SPDX-License-Identifier: GPL-2.0-or-later
/* Group basic: system calls with known register and flag state. */
	.syntax	unified
	.arm
	.text

@ u32 syscall_state_arm(u32 out[9])
@ Sets Q, GE and the flags, calls getpid through svc with a nonzero
@ immediate, and stores the APSR and r5-r11 as the call returned them.
	.globl	syscall_state_arm
	.type	syscall_state_arm, %function
syscall_state_arm:
	push	{r4-r11, lr}
	mov	r4, r0
	mvn	r8, #0
	movw	r9, #0x0101
	movt	r9, #0x0101
	uadd8	r10, r8, r9		@ every lane carries: GE = 0b1111
	movw	r5, #0xffff
	movt	r5, #0x7fff
	mov	r6, #1
	qadd	r6, r5, r6		@ saturates: Q = 1
	movw	r5, #0x1111
	movt	r5, #0x1111
	movw	r6, #0x2222
	movt	r6, #0x2222
	movw	r8, #0x4444
	movt	r8, #0x4444
	movw	r9, #0x5555
	movt	r9, #0x5555
	movw	r10, #0x6666
	movt	r10, #0x6666
	movw	r11, #0x7777
	movt	r11, #0x7777
	cmp	r4, r4			@ Z = 1, C = 1, N = 0, V = 0
	mov	r7, #20
	svc	#0
	mrs	r1, apsr
	str	r1, [r4]
	str	r5, [r4, #4]
	str	r6, [r4, #8]
	str	r8, [r4, #12]
	str	r9, [r4, #16]
	str	r10, [r4, #20]
	str	r11, [r4, #24]
	str	r0, [r4, #28]
	pop	{r4-r11, pc}

@ long syscall_imm_arm(void): getpid through svc with a nonzero immediate.
	.globl	syscall_imm_arm
	.type	syscall_imm_arm, %function
syscall_imm_arm:
	mov	r7, #20
	svc	#0xa5a5a5		@ EABI: the immediate means nothing
	bx	lr

	.thumb
	.thumb_func
	.globl	syscall_state_thumb
	.type	syscall_state_thumb, %function
syscall_state_thumb:
	push.w	{r4-r11, lr}
	mov	r4, r0
	mvn	r8, #0
	mov.w	r9, #0x01010101
	uadd8	r10, r8, r9
	movw	r5, #0xffff
	movt	r5, #0x7fff
	mov	r6, #1
	qadd	r6, r5, r6
	movw	r5, #0x1111
	movt	r5, #0x1111
	movw	r6, #0x2222
	movt	r6, #0x2222
	movw	r8, #0x4444
	movt	r8, #0x4444
	movw	r9, #0x5555
	movt	r9, #0x5555
	movw	r10, #0x6666
	movt	r10, #0x6666
	movw	r11, #0x7777
	movt	r11, #0x7777
	cmp	r4, r4
	mov	r7, #20
	svc	#0xff
	mrs	r1, apsr
	str	r1, [r4]
	str	r5, [r4, #4]
	str	r6, [r4, #8]
	str	r8, [r4, #12]
	str	r9, [r4, #16]
	str	r10, [r4, #20]
	str	r11, [r4, #24]
	str	r0, [r4, #28]
	pop.w	{r4-r11, pc}
	.arm
