// SPDX-License-Identifier: GPL-2.0-or-later
/* Group kuser: the helpers of the kernel's user-space page at 0xffff0f60 to
 * 0xffff0ffc, called from A32 and T32 with register and flag state known. */
	.syntax	unified
	.text

@ ---- u32 ku_cmpxchg_{arm,thumb}(u32 old, u32 new, u32 *ptr, u32 *carry) ----
@ Returns the helper's r0; *carry is the C flag it returned.
	.arm
	.balign	4
	.globl	ku_cmpxchg_arm
	.type	ku_cmpxchg_arm, %function
ku_cmpxchg_arm:
	push	{r4, lr}
	mov	r4, r3
	movw	r3, #0x0fc0
	movt	r3, #0xffff
	blx	r3
	mov	r1, #0
	adc	r1, r1, #0
	str	r1, [r4]
	pop	{r4, pc}

	.thumb
	.balign	4
	.thumb_func
	.globl	ku_cmpxchg_thumb
	.type	ku_cmpxchg_thumb, %function
ku_cmpxchg_thumb:
	push	{r4, lr}
	mov	r4, r3
	movw	r3, #0x0fc0
	movt	r3, #0xffff
	blx	r3
	mov	r1, #0
	adc	r1, r1, #0
	str	r1, [r4]
	pop	{r4, pc}

@ ---- u32 ku_cmpxchg64_{arm,thumb}(u64 *old, u64 *new, u64 *ptr, u32 *carry) ----
	.arm
	.balign	4
	.globl	ku_cmpxchg64_arm
	.type	ku_cmpxchg64_arm, %function
ku_cmpxchg64_arm:
	push	{r4, lr}
	mov	r4, r3
	movw	r3, #0x0f60
	movt	r3, #0xffff
	blx	r3
	mov	r1, #0
	adc	r1, r1, #0
	str	r1, [r4]
	pop	{r4, pc}

	.thumb
	.balign	4
	.thumb_func
	.globl	ku_cmpxchg64_thumb
	.type	ku_cmpxchg64_thumb, %function
ku_cmpxchg64_thumb:
	push	{r4, lr}
	mov	r4, r3
	movw	r3, #0x0f60
	movt	r3, #0xffff
	blx	r3
	mov	r1, #0
	adc	r1, r1, #0
	str	r1, [r4]
	pop	{r4, pc}

@ ---- void ku_preserves_{arm,thumb}(u32 helper, u32 out[20], u32 r0v, u32 r1v) ----
@ Calls the helper at address `helper` with r0 = r0v, r1 = r1v and
@ r2 = &out[16], r4-r11 holding magic and N, C set, then stores
@ out[0..7] = r4-r11, out[8] = r1, out[9] = r2, out[10] = r0, out[11] = the
@ APSR read right after the call.
	.macro	ku_preserves
	push	{r4-r11, lr}
	push	{r1}
	mov	r12, r0
	add	r0, r1, #64
	mov	r1, r2
	mov	r2, r3
	mov	r3, r0
	mov	r0, r1
	mov	r1, r2
	mov	r2, r3
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
	movw	r11, #0xbbbb
	movt	r11, #0xbbbb
	movw	r3, #0
	movt	r3, #0xa000
	msr	apsr_nzcvq, r3
	blx	r12
	mrs	r3, apsr
	ldr	r12, [sp]
	stm	r12, {r4-r11}
	str	r1, [r12, #32]
	str	r2, [r12, #36]
	str	r0, [r12, #40]
	str	r3, [r12, #44]
	add	sp, sp, #4
	pop	{r4-r11, pc}
	.endm

	.arm
	.balign	4
	.globl	ku_preserves_arm
	.type	ku_preserves_arm, %function
ku_preserves_arm:
	ku_preserves

	.thumb
	.balign	4
	.thumb_func
	.globl	ku_preserves_thumb
	.type	ku_preserves_thumb, %function
ku_preserves_thumb:
	ku_preserves

@ ---- void ku_store(u32 *address): a store the page must refuse ----
	.arm
	.balign	4
	.globl	ku_store
	.type	ku_store, %function
ku_store:
	.globl	ku_store_insn
ku_store_insn:
	str	r0, [r0]
	bx	lr
