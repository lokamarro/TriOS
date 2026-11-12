// SPDX-License-Identifier: GPL-2.0-or-later
/* Group signals: restorers, routines that fault with a known register and
 * flag state in A32 and T32, an IT block that faults in its first slot, jumps
 * that a handler redirects across instruction sets, blocking reads, and a
 * T32 signal handler. */
	.syntax	unified
	.text

@ ---- restorers: rt_sigreturn and sigreturn ----
	.arm
	.balign	4
	.globl	sig_restorer_rt
	.type	sig_restorer_rt, %function
sig_restorer_rt:
	mov	r7, #173
	svc	#0
	udf	#0

	.globl	sig_restorer_old
	.type	sig_restorer_old, %function
sig_restorer_old:
	mov	r7, #119
	svc	#0
	udf	#0

@ ---- void sig_fault_{arm,thumb}(u32 out[12], void *bad) ----
@ Sets r2-r10, the flags, Q and GE, loads from `bad`, and after the handler
@ returns stores the APSR and r0-r10 into out. The handler skips the load,
@ puts 0x1234 in r0, and changes r5 and the flags in the frame.
	.macro	sig_fault name
	push	{r4-r11, lr}
	mov	r11, r0
	movw	r2, #0x2222
	movt	r2, #0x2222
	movw	r3, #0x3333
	movt	r3, #0x3333
	movw	r4, #0x4444
	movt	r4, #0x4444
	movw	r5, #0x5555
	movt	r5, #0x5555
	movw	r6, #0x6666
	movt	r6, #0x6666
	movw	r7, #0x7777
	movt	r7, #0x7777
	movw	r8, #0x8888
	movt	r8, #0x8888
	movw	r9, #0x9999
	movt	r9, #0x9999
	movw	r10, #0xaaaa
	movt	r10, #0xaaaa
	movw	r0, #0
	movt	r0, #0xa809
	msr	apsr_nzcvqg, r0
	.globl	\name\()_insn
\name\()_insn:
	ldr	r0, [r1]
	mrs	r12, apsr
	str	r12, [r11]
	add	r11, r11, #4
	stm	r11, {r0-r10}
	pop	{r4-r11, pc}
	.endm

	.arm
	.balign	4
	.globl	sig_fault_arm
	.type	sig_fault_arm, %function
sig_fault_arm:
	sig_fault sig_fault_arm

	.thumb
	.balign	4
	.thumb_func
	.globl	sig_fault_thumb
	.type	sig_fault_thumb, %function
sig_fault_thumb:
	sig_fault sig_fault_thumb

@ ---- u32 sig_it_fault(void *bad): a fault in the first slot of an IT block ----
@ ITTEE EQ with Z set: slots 1 and 2 run, slots 3 and 4 are skipped. The
@ handler points r5 at a valid word and resumes the same load: the rest of the
@ block then has to run with the block state the frame carried. Returns
@ (loaded value << 8) | r7, where r7 is 1 when the block state was kept.
	.balign	4
	.thumb_func
	.globl	sig_it_fault
	.type	sig_it_fault, %function
sig_it_fault:
	push	{r4-r7, lr}
	mov	r5, r0
	movs	r6, #0
	movs	r7, #0
	cmp	r5, r5
	ittee	eq
	.globl	sig_it_fault_insn
sig_it_fault_insn:
	ldreq	r6, [r5]
	addeq	r7, r7, #1
	addne	r7, r7, #16
	addne	r7, r7, #32
	lsls	r0, r6, #8
	orrs	r0, r7
	pop	{r4-r7, pc}

@ ---- long sig_jump_{arm,thumb}(void *bad): the handler redirects the pc ----
@ into the other instruction set; the target returns to the caller.
	.arm
	.balign	4
	.globl	sig_jump_arm
	.type	sig_jump_arm, %function
sig_jump_arm:
	ldr	r0, [r0]
	mov	r0, #0x99
	bx	lr

	.globl	sig_jump_target_arm
	.type	sig_jump_target_arm, %function
sig_jump_target_arm:
	mov	r0, #0x41
	bx	lr

	.thumb
	.balign	4
	.thumb_func
	.globl	sig_jump_thumb
	.type	sig_jump_thumb, %function
sig_jump_thumb:
	ldr	r0, [r0]
	movs	r0, #0x99
	bx	lr

	.balign	4
	.thumb_func
	.globl	sig_jump_target_thumb
	.type	sig_jump_target_thumb, %function
sig_jump_target_thumb:
	movs	r0, #0x54
	bx	lr

@ ---- long sig_read_{arm,thumb}(int fd, void *buffer, u32 length) ----
@ read(2) as one svc at a known address, for a signal that interrupts it.
	.arm
	.balign	4
	.globl	sig_read_arm
	.type	sig_read_arm, %function
sig_read_arm:
	push	{r7, lr}
	mov	r7, #3
	.globl	sig_read_arm_svc
sig_read_arm_svc:
	svc	#0
	pop	{r7, pc}

	.thumb
	.balign	4
	.thumb_func
	.globl	sig_read_thumb
	.type	sig_read_thumb, %function
sig_read_thumb:
	push	{r7, lr}
	movs	r7, #3
	.globl	sig_read_thumb_svc
sig_read_thumb_svc:
	svc	#0
	pop	{r7, pc}

@ ---- long sig_tgkill_{arm,thumb}(pid, tid, signal) ----
	.arm
	.balign	4
	.globl	sig_tgkill_arm
	.type	sig_tgkill_arm, %function
sig_tgkill_arm:
	push	{r7, lr}
	movw	r7, #268
	svc	#0
	pop	{r7, pc}

	.thumb
	.balign	4
	.thumb_func
	.globl	sig_tgkill_thumb
	.type	sig_tgkill_thumb, %function
sig_tgkill_thumb:
	push	{r7, lr}
	movw	r7, #268
	svc	#0
	pop	{r7, pc}

@ ---- a T32 signal handler: records r0-r2, lr and a count in sig_thumb_seen ----
	.balign	4
	.thumb_func
	.globl	sig_thumb_handler
	.type	sig_thumb_handler, %function
sig_thumb_handler:
	movw	r3, :lower16:sig_thumb_seen
	movt	r3, :upper16:sig_thumb_seen
	str	r0, [r3]
	str	r1, [r3, #4]
	str	r2, [r3, #8]
	str	lr, [r3, #12]
	ldr	r0, [r3, #16]
	adds	r0, r0, #1
	str	r0, [r3, #16]
	ldr	r0, [r2, #96]
	str	r0, [r3, #20]
	bx	lr

	.data
	.balign	4
	.globl	sig_thumb_seen
sig_thumb_seen:
	.space	24
