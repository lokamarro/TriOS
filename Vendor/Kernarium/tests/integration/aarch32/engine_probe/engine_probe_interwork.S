// SPDX-License-Identifier: GPL-2.0-or-later
/* Group interwork: calls and returns between A32 and T32 in every form that
 * changes the instruction set, PC reads and the Align(PC, 4) base at both
 * halfword positions, table branches, and IT blocks. */

	.syntax	unified
	.text

@ ---- callees that return the lr they were called with ----
	.arm
	.balign	4
	.globl	iw_arm_lr
	.type	iw_arm_lr, %function
iw_arm_lr:
	mov	r0, lr
	bx	lr

	.thumb
	.balign	4
	.thumb_func
	.globl	iw_thumb_lr
	.type	iw_thumb_lr, %function
iw_thumb_lr:
	mov	r0, lr
	bx	lr

@ ---- callees that return a constant naming their instruction set ----
	.arm
	.balign	4
	.globl	iw_arm_k
	.type	iw_arm_k, %function
iw_arm_k:
	mov	r0, #0x41
	bx	lr

	.thumb
	.balign	4
	.thumb_func
	.globl	iw_thumb_k
	.type	iw_thumb_k, %function
iw_thumb_k:
	movs	r0, #0x54
	bx	lr

@ ---- long iw_*_calls(long (*fn)(void)): blx fn and return what it returns ----
	.arm
	.balign	4
	.globl	iw_arm_calls
	.type	iw_arm_calls, %function
iw_arm_calls:
	push	{r4, lr}
	blx	r0
	pop	{r4, pc}

	.thumb
	.balign	4
	.thumb_func
	.globl	iw_thumb_calls
	.type	iw_thumb_calls, %function
iw_thumb_calls:
	push	{r4, lr}
	blx	r0
	pop	{r4, pc}

@ ---- A32 callees, each returning by a different instruction ----
	.arm
	.balign	4
	.globl	iw_arm_ret_pop
	.type	iw_arm_ret_pop, %function
iw_arm_ret_pop:
	push	{lr}
	mov	r0, #2
	pop	{pc}

	.globl	iw_arm_ret_ldr
	.type	iw_arm_ret_ldr, %function
iw_arm_ret_ldr:
	str	lr, [sp, #-4]!
	mov	r0, #3
	ldr	pc, [sp], #4

	.globl	iw_arm_ret_mov
	.type	iw_arm_ret_mov, %function
iw_arm_ret_mov:
	mov	r0, #4
	mov	pc, lr

	.globl	iw_arm_ret_ldm
	.type	iw_arm_ret_ldm, %function
iw_arm_ret_ldm:
	push	{r4, r5, lr}
	mov	r0, #5
	ldm	sp!, {r4, r5, pc}

	.globl	iw_arm_ret_bxcond
	.type	iw_arm_ret_bxcond, %function
iw_arm_ret_bxcond:
	mov	r0, #6
	cmp	r0, r0
	bxeq	lr
	mov	r0, #99
	bx	lr

	.globl	iw_arm_ret_movcond
	.type	iw_arm_ret_movcond, %function
iw_arm_ret_movcond:
	mov	r0, #7
	cmp	r0, r0
	moveq	pc, lr
	mov	r0, #99
	bx	lr

	.globl	iw_arm_ret_add
	.type	iw_arm_ret_add, %function
iw_arm_ret_add:
	mov	r0, #8
	add	pc, lr, #0

@ ---- T32 callees ----
	.thumb
	.balign	4
	.thumb_func
	.globl	iw_thumb_ret_pop
	.type	iw_thumb_ret_pop, %function
iw_thumb_ret_pop:
	push	{lr}
	movs	r0, #2
	pop	{pc}

	.balign	4
	.thumb_func
	.globl	iw_thumb_ret_ldr
	.type	iw_thumb_ret_ldr, %function
iw_thumb_ret_ldr:
	str	lr, [sp, #-4]!
	movs	r0, #3
	ldr	pc, [sp], #4

	.balign	4
	.thumb_func
	.globl	iw_thumb_ret_popw
	.type	iw_thumb_ret_popw, %function
iw_thumb_ret_popw:
	push.w	{r4, r5, lr}
	movs	r0, #4
	pop.w	{r4, r5, pc}

	.balign	4
	.thumb_func
	.globl	iw_thumb_ret_itbx
	.type	iw_thumb_ret_itbx, %function
iw_thumb_ret_itbx:
	movs	r0, #5
	cmp	r0, r0
	it	eq
	bxeq	lr
	movs	r0, #99
	bx	lr

@ Thumb callers only: mov pc keeps the instruction set (bit 0 is dropped).
	.balign	4
	.thumb_func
	.globl	iw_thumb_ret_mov
	.type	iw_thumb_ret_mov, %function
iw_thumb_ret_mov:
	movs	r0, #6
	mov	pc, lr

@ ---- jumps to the target in r0 (a tail call: the target returns) ----
	.arm
	.balign	4
	.globl	iw_a_bx
	.type	iw_a_bx, %function
iw_a_bx:
	bx	r0

	.globl	iw_a_mov
	.type	iw_a_mov, %function
iw_a_mov:
	mov	pc, r0

	.globl	iw_a_add
	.type	iw_a_add, %function
iw_a_add:
	add	pc, r0, #0

@ r0 = address of a word holding the target.
	.globl	iw_a_ldr
	.type	iw_a_ldr, %function
iw_a_ldr:
	ldr	pc, [r0]

	.globl	iw_a_pop
	.type	iw_a_pop, %function
iw_a_pop:
	push	{r0}
	pop	{pc}

	.thumb
	.balign	4
	.thumb_func
	.globl	iw_t_bx
	.type	iw_t_bx, %function
iw_t_bx:
	bx	r0

	.balign	4
	.thumb_func
	.globl	iw_t_mov
	.type	iw_t_mov, %function
iw_t_mov:
	mov	pc, r0

	.balign	4
	.thumb_func
	.globl	iw_t_ldr
	.type	iw_t_ldr, %function
iw_t_ldr:
	ldr.w	pc, [r0]

	.balign	4
	.thumb_func
	.globl	iw_t_pop
	.type	iw_t_pop, %function
iw_t_pop:
	push	{r0}
	pop	{pc}

@ long iw_t_tbb(long index): a byte table branch; iw_t_tbh the halfword form.
	.balign	4
	.thumb_func
	.globl	iw_t_tbb
	.type	iw_t_tbb, %function
iw_t_tbb:
	tbb	[pc, r0]
1:	.byte	(2f - 1b) / 2, (3f - 1b) / 2, (4f - 1b) / 2, (5f - 1b) / 2
	.balign	2
2:	movs	r0, #10
	bx	lr
3:	movs	r0, #11
	bx	lr
4:	movs	r0, #12
	bx	lr
5:	movs	r0, #13
	bx	lr

	.balign	4
	.thumb_func
	.globl	iw_t_tbh
	.type	iw_t_tbh, %function
iw_t_tbh:
	tbh	[pc, r0, lsl #1]
1:	.short	(2f - 1b) / 2, (3f - 1b) / 2, (4f - 1b) / 2
2:	movs	r0, #20
	bx	lr
3:	movs	r0, #21
	bx	lr
4:	movs	r0, #22
	bx	lr

@ ---- PC reads: the distance from the instruction to the value read ----
	.arm
	.balign	4
	.globl	iw_a_pc
	.type	iw_a_pc, %function
iw_a_pc:
	adr	r1, 1f
1:	mov	r0, pc
	sub	r0, r0, r1
	bx	lr

	.thumb
	.balign	4
	.thumb_func
	.globl	iw_t_pc
	.type	iw_t_pc, %function
iw_t_pc:
	adr	r1, 1f
	.balign	4
1:	mov	r0, pc
	subs	r0, r0, r1
	bx	lr

@ ---- Align(PC, 4) with the instruction at each halfword position ----
@ adr and ldr (literal) at 0 mod 4, then at 2 mod 4; blx and bl likewise.
	.balign	4
	.thumb_func
	.globl	iw_t_adr0
	.type	iw_t_adr0, %function
iw_t_adr0:
	adr	r0, .Lpool
	bx	lr
	.balign	4
	.thumb_func
	.globl	iw_t_adr2
	.type	iw_t_adr2, %function
iw_t_adr2:
	nop
	adr	r0, .Lpool
	bx	lr
	.balign	4
	.thumb_func
	.globl	iw_t_adrw0
	.type	iw_t_adrw0, %function
iw_t_adrw0:
	adr.w	r0, .Lpool
	bx	lr
	.balign	4
	.thumb_func
	.globl	iw_t_adrw2
	.type	iw_t_adrw2, %function
iw_t_adrw2:
	nop
	adr.w	r0, .Lpool
	bx	lr
	.balign	4
	.thumb_func
	.globl	iw_t_lit0
	.type	iw_t_lit0, %function
iw_t_lit0:
	ldr	r0, .Lpool
	bx	lr
	.balign	4
	.thumb_func
	.globl	iw_t_lit2
	.type	iw_t_lit2, %function
iw_t_lit2:
	nop
	ldr	r0, .Lpool
	bx	lr
	.balign	4
	.thumb_func
	.globl	iw_t_litw0
	.type	iw_t_litw0, %function
iw_t_litw0:
	ldr.w	r0, .Lpool
	bx	lr
	.balign	4
	.thumb_func
	.globl	iw_t_litw2
	.type	iw_t_litw2, %function
iw_t_litw2:
	nop
	ldr.w	r0, .Lpool
	bx	lr

@ blx and bl from 0 mod 4 and 2 mod 4: nop shifts the instruction.
	.balign	4
	.thumb_func
	.globl	iw_t_blx0
	.type	iw_t_blx0, %function
iw_t_blx0:
	nop
	push	{lr}
	blx	iw_arm_k
	pop	{pc}
	.balign	4
	.thumb_func
	.globl	iw_t_blx2
	.type	iw_t_blx2, %function
iw_t_blx2:
	push	{lr}
	blx	iw_arm_k
	pop	{pc}
	.balign	4
	.thumb_func
	.globl	iw_t_bl0
	.type	iw_t_bl0, %function
iw_t_bl0:
	nop
	push	{lr}
	bl	iw_thumb_k
	pop	{pc}
	.balign	4
	.thumb_func
	.globl	iw_t_bl2
	.type	iw_t_bl2, %function
iw_t_bl2:
	push	{lr}
	bl	iw_thumb_k
	pop	{pc}

@ A literal pool both the adr and ldr forms reach.
	.balign	4
	.globl	iw_pool
iw_pool:
.Lpool:
	.word	0x1badf00d

@ ---- Q, GE and the flags through an IT block that is skipped ----
@ void iw_it_skip(u32 out[2]): the conditional load of a skipped slot must not
@ fault, and the instruction after the block runs.
	.balign	4
	.thumb_func
	.globl	iw_it_skip
	.type	iw_it_skip, %function
iw_it_skip:
	push	{r4, lr}
	movs	r3, #0			@ a null pointer
	movs	r2, #0
	movs	r1, #0
	cmp	r1, r1			@ Z = 1
	ittt	ne
	ldrne.w	r2, [r3, #4]		@ skipped: no fault
	ldrne	r2, [r3]
	movne	r2, #0x55
	movs	r1, #7
	str	r2, [r0]
	str	r1, [r0, #4]
	pop	{r4, pc}

@ void iw_it_svc(u32 out[3]): an SVC inside a block, taken and skipped.
	.balign	4
	.thumb_func
	.globl	iw_it_svc
	.type	iw_it_svc, %function
iw_it_svc:
	push	{r4, r5, r7, lr}
	mov	r5, r0
	movs	r4, #0
	movs	r7, #20			@ getpid
	movs	r0, #0x55
	cmp	r4, r4			@ Z = 1
	itt	eq
	svceq	#0
	addeq	r4, r4, #1		@ still conditional, and runs once
	str	r0, [r5]		@ the pid
	str	r4, [r5, #4]
	movs	r0, #0x66
	cmp	r4, r4
	ite	ne
	svcne	#0			@ skipped: r0 keeps its value
	addeq	r4, r4, #10
	str	r0, [r5, #8]
	str	r4, [r5, #12]
	pop	{r4, r5, r7, pc}

	.macro	iw_case cond
	.arm
	.balign	4
	.globl	iw_ac_\cond
	.type	iw_ac_\cond, %function
iw_ac_\cond:
	mov	r1, r0, lsl #28
	msr	apsr_nzcvq, r1
	mov	r2, #0
	mov\cond r2, #1
	mov	r0, r2
	bx	lr
	.thumb
	.balign	4
	.thumb_func
	.globl	iw_tc_\cond
	.type	iw_tc_\cond, %function
iw_tc_\cond:
	lsls	r1, r0, #28
	movs	r2, #0
	msr	apsr_nzcvq, r1
	it	\cond
	mov\cond r2, #1
	mov	r0, r2
	bx	lr
	.endm
	iw_case	eq
	iw_case	ne
	iw_case	cs
	iw_case	cc
	iw_case	mi
	iw_case	pl
	iw_case	vs
	iw_case	vc
	iw_case	hi
	iw_case	ls
	iw_case	ge
	iw_case	lt
	iw_case	gt
	iw_case	le
