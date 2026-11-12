// SPDX-License-Identifier: GPL-2.0-or-later
/* Group ptrace: routines a traced child runs. Each stops at a bkpt for the
 * tracer, which reads and changes the child's registers and steps it. */
	.syntax	unified
	.text

@ ---- long pt_regs_{arm,thumb}(void): magic registers, a stop, a changed r0 ----
@ r4-r10 hold 0x44444444 .. 0xaaaaaaaa and the flags N and C are set at the
@ stop. The tracer sets r0 = 0x42 and r5 = 0x77 and moves the pc past the
@ bkpt; the routine returns r0 + r5.
	.arm
	.balign	4
	.globl	pt_regs_arm
	.type	pt_regs_arm, %function
pt_regs_arm:
	push	{r4-r11, lr}
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
	movt	r0, #0xa000
	msr	apsr_nzcvq, r0
	mov	r0, #0x10
	.globl	pt_regs_arm_stop
pt_regs_arm_stop:
	bkpt	#1
	add	r0, r0, r5
	pop	{r4-r11, pc}

	.thumb
	.balign	4
	.thumb_func
	.globl	pt_regs_thumb
	.type	pt_regs_thumb, %function
pt_regs_thumb:
	push	{r4-r11, lr}
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
	movt	r0, #0xa000
	msr	apsr_nzcvq, r0
	movw	r0, #0x10
	.globl	pt_regs_thumb_stop
pt_regs_thumb_stop:
	bkpt	#1
	add	r0, r0, r5
	pop	{r4-r11, pc}

@ ---- long pt_break_{arm,thumb}(void): the debugger encodings, then a value ----
	.arm
	.balign	4
	.globl	pt_break_arm
	.type	pt_break_arm, %function
pt_break_arm:
	.globl	pt_break_arm_insn
pt_break_arm_insn:
	.inst	0xe7f001f0
	mov	r0, #0x21
	bx	lr

	.thumb
	.balign	4
	.thumb_func
	.globl	pt_break_thumb
	.type	pt_break_thumb, %function
pt_break_thumb:
	.globl	pt_break_thumb_insn
pt_break_thumb_insn:
	.inst.n	0xde01
	movs	r0, #0x22
	bx	lr

@ ---- long pt_step_it(void): single steps through an IT block (T32) ----
@ Stops at a bkpt, then runs: movs r1,#0; cmp r1,r1; itete eq; addeq r1,#1;
@ addne r1,#2; addeq r1,#4; addne r1,#8; and returns r1 (5).
	.balign	4
	.thumb_func
	.globl	pt_step_it
	.type	pt_step_it, %function
pt_step_it:
	push	{r4, lr}
	.globl	pt_step_it_stop
pt_step_it_stop:
	bkpt	#1
	.globl	pt_step_it_first
pt_step_it_first:
	movs	r1, #0
	cmp	r1, r1
	itete	eq
	addeq	r1, r1, #1
	addne	r1, r1, #2
	addeq	r1, r1, #4
	addne	r1, r1, #8
	mov	r0, r1
	pop	{r4, pc}

@ ---- long pt_step_arm(void): single steps through branches, an svc, bx ----
	.arm
	.balign	4
	.globl	pt_step_arm
	.type	pt_step_arm, %function
pt_step_arm:
	push	{r4, r7, lr}
	.globl	pt_step_arm_stop
pt_step_arm_stop:
	bkpt	#1
	.globl	pt_step_arm_s1
pt_step_arm_s1:
	mov	r1, #1
	add	r1, r1, #2
	b	.Lstep_s5
	mov	r1, #99
	.globl	pt_step_arm_s5
.Lstep_s5:
pt_step_arm_s5:
	add	r1, r1, #4
	mov	r7, #20
	svc	#0
	adr	r2, .Lstep_t1
	orr	r2, r2, #1
	bx	r2
	.thumb
	.balign	4
	.globl	pt_step_arm_t1
.Lstep_t1:
pt_step_arm_t1:
	movs	r0, #0x30
	adds	r0, r0, r1
	pop	{r4, r7, pc}

@ ---- long pt_step_swp(void): single steps over an emulated and a skipped SWP ----
@ A word on the stack holds 0x55 and r1 0xaa. After the bkpt the tracer steps:
@ swp r0, r1, [r2] (emulated: r0 = 0x55, the word 0xaa), cmp r1, #0 (Z clear),
@ mov r5, #0x77, swpeq r5, r1, [r2] (its condition false: r5 stays 0x77). The
@ routine returns the word, 0xaa.
	.arm
	.balign	4
	.globl	pt_step_swp
	.type	pt_step_swp, %function
pt_step_swp:
	push	{r4, r5, lr}
	sub	sp, sp, #8
	mov	r2, sp
	mov	r4, #0x55
	str	r4, [r2]
	mov	r1, #0xaa
	.globl	pt_step_swp_stop
pt_step_swp_stop:
	bkpt	#1
	.globl	pt_step_swp_s1
pt_step_swp_s1:
	swp	r0, r1, [r2]
	cmp	r1, #0
	mov	r5, #0x77
	.globl	pt_step_swp_s4
pt_step_swp_s4:
	swpeq	r5, r1, [r2]
	ldr	r0, [r2]
	add	sp, sp, #8
	pop	{r4, r5, pc}
