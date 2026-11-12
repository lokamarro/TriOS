// SPDX-License-Identifier: GPL-2.0-or-later
/* Group atomics: the loops the contention tests run, each in A32 and T32.
 * They use the exclusives of every width and the kernel's cmpxchg helpers, so
 * the code is shared between instruction sets through macros. */
	.syntax	unified
	.text

	.macro	func_arm name
	.arm
	.balign	4
	.globl	\name
	.type	\name, %function
\name:
	.endm

	.macro	func_thumb name
	.thumb
	.balign	4
	.thumb_func
	.globl	\name
	.type	\name, %function
\name:
	.endm

@ ---- u32 at_add_<width>(void *p, u32 n): n increments; returns STREX retries ----
	.macro	body_add ld, st
	mov	r12, #0
1:	\ld	r2, [r0]
	add	r2, r2, #1
	\st	r3, r2, [r0]
	cmp	r3, #0
	beq	2f
	add	r12, r12, #1
	b	1b
2:	subs	r1, r1, #1
	bne	1b
	mov	r0, r12
	bx	lr
	.endm

	func_arm at_add_ldrex_arm
	body_add ldrex, strex
	func_thumb at_add_ldrex_thumb
	body_add ldrex, strex
	func_arm at_add_ldrexb_arm
	body_add ldrexb, strexb
	func_thumb at_add_ldrexb_thumb
	body_add ldrexb, strexb
	func_arm at_add_ldrexh_arm
	body_add ldrexh, strexh
	func_thumb at_add_ldrexh_thumb
	body_add ldrexh, strexh

@ ---- void at_add_ldrexd(u64 *p, u32 n) ----
	.macro	body_add64
1:	ldrexd	r2, r3, [r0]
	adds	r2, r2, #1
	adc	r3, r3, #0
	strexd	r12, r2, r3, [r0]
	cmp	r12, #0
	bne	1b
	subs	r1, r1, #1
	bne	1b
	bx	lr
	.endm

	func_arm at_add_ldrexd_arm
	body_add64
	func_thumb at_add_ldrexd_thumb
	body_add64

@ ---- u32 at_add_cmpxchg(u32 *p, u32 n): the kernel helper; returns retries ----
	.macro	body_cmpxchg
	push	{r4-r6, lr}
	mov	r4, r0
	mov	r5, r1
	mov	r6, #0
1:	ldr	r0, [r4]
	add	r1, r0, #1
	mov	r2, r4
	movw	r3, #0x0fc0
	movt	r3, #0xffff
	blx	r3
	cmp	r0, #0
	beq	2f
	add	r6, r6, #1
	b	1b
2:	subs	r5, r5, #1
	bne	1b
	mov	r0, r6
	pop	{r4-r6, pc}
	.endm

	func_arm at_add_cmpxchg_arm
	body_cmpxchg
	func_thumb at_add_cmpxchg_thumb
	body_cmpxchg

@ ---- void at_add_cmpxchg64(u64 *p, u32 n): the 64-bit helper ----
	.macro	body_cmpxchg64
	push	{r4-r8, lr}
	sub	sp, sp, #16
	mov	r4, r0
	mov	r5, r1
1:	ldr	r6, [r4]
	ldr	r7, [r4, #4]
	str	r6, [sp]
	str	r7, [sp, #4]
	adds	r6, r6, #1
	adc	r7, r7, #0
	str	r6, [sp, #8]
	str	r7, [sp, #12]
	mov	r0, sp
	add	r1, sp, #8
	mov	r2, r4
	movw	r3, #0x0f60
	movt	r3, #0xffff
	blx	r3
	cmp	r0, #0
	bne	1b
	subs	r5, r5, #1
	bne	1b
	add	sp, sp, #16
	pop	{r4-r8, pc}
	.endm

	func_arm at_add_cmpxchg64_arm
	body_cmpxchg64
	func_thumb at_add_cmpxchg64_thumb
	body_cmpxchg64

@ ---- void at_store_loop(volatile u32 *p, volatile u32 *stop): plain stores ----
	func_arm at_store_loop_arm
	mov	r2, #0
1:	add	r2, r2, #1
	str	r2, [r0]
	ldr	r3, [r1]
	cmp	r3, #0
	beq	1b
	bx	lr

	func_thumb at_store_loop_thumb
	movs	r2, #0
1:	adds	r2, r2, #1
	str	r2, [r0]
	ldr	r3, [r1]
	cmp	r3, #0
	beq	1b
	bx	lr

@ ---- void at_pair_writer(u64 *p, u32 n): (i, i) for i = 1..n by STREXD ----
	.macro	body_pair_writer
	push	{r4, lr}
	mov	r12, #1
1:	ldrexd	r2, r3, [r0]
	mov	r2, r12
	mov	r3, r12
	strexd	r4, r2, r3, [r0]
	cmp	r4, #0
	bne	1b
	add	r12, r12, #1
	cmp	r12, r1
	bls	1b
	pop	{r4, pc}
	.endm

	func_arm at_pair_writer_arm
	body_pair_writer
	func_thumb at_pair_writer_thumb
	body_pair_writer

@ ---- u32 at_pair_reader_{ldrexd,ldrd}(u64 *p, volatile u32 *stop) ----
@ Counts the pairs whose halves differ until *stop is set; a reader that is
@ single-copy atomic never sees one.
	.macro	body_pair_reader ld
	mov	r12, #0
1:	\ld
	cmp	r2, r3
	beq	2f
	add	r12, r12, #1
2:	ldr	r2, [r1]
	cmp	r2, #0
	beq	1b
	mov	r0, r12
	bx	lr
	.endm

	func_arm at_pair_reader_ldrexd_arm
	body_pair_reader "ldrexd r2, r3, [r0]"
	func_thumb at_pair_reader_ldrexd_thumb
	body_pair_reader "ldrexd r2, r3, [r0]"
	func_arm at_pair_reader_ldrd_arm
	body_pair_reader "ldrd r2, r3, [r0]"
	func_thumb at_pair_reader_ldrd_thumb
	body_pair_reader "ldrd r2, r3, [r0]"

@ ---- void at_monitor(u32 *p, u32 out[3]) ----
@ The monitor's state: a STREX after LDREX stores and returns 0, a second
@ STREX without an LDREX fails, and so does one after CLREX.
	.macro	body_monitor
	push	{r4, lr}
	mov	r4, r1
	ldrex	r2, [r0]
	add	r2, r2, #1
	strex	r3, r2, [r0]
	str	r3, [r4]
	add	r2, r2, #100
	strex	r3, r2, [r0]
	str	r3, [r4, #4]
	ldrex	r2, [r0]
	add	r2, r2, #1000
	clrex
	strex	r3, r2, [r0]
	str	r3, [r4, #8]
	pop	{r4, pc}
	.endm

	func_arm at_monitor_arm
	body_monitor
	func_thumb at_monitor_thumb
	body_monitor

@ ---- u32 at_swap_<kind>(void *p, u32 token, u32 n): n exchanges of a token ----
@ Each exchange stores the token the thread holds and takes the one that was
@ there, so the tokens of all threads and the word stay a permutation of what
@ they began as unless an exchange or a store is lost. Returns the token held
@ at the end. SWP and SWPB exist in A32 only, and are emulated by the kernel
@ while abi.swp is 1.
	func_arm at_swap_swp_arm
1:	swp	r1, r1, [r0]
	subs	r2, r2, #1
	bne	1b
	mov	r0, r1
	bx	lr

	func_arm at_swap_swpb_arm
1:	swpb	r1, r1, [r0]
	subs	r2, r2, #1
	bne	1b
	and	r0, r1, #0xff
	bx	lr

	.macro	body_swap_ldrex
1:	ldrex	r3, [r0]
	strex	r12, r1, [r0]
	cmp	r12, #0
	bne	1b
	mov	r1, r3
	subs	r2, r2, #1
	bne	1b
	mov	r0, r1
	bx	lr
	.endm

	func_arm at_swap_ldrex_arm
	body_swap_ldrex
	func_thumb at_swap_ldrex_thumb
	body_swap_ldrex

@ The same through the kernel's cmpxchg helper: replace the word if it still
@ holds the value just read.
	.macro	body_swap_cmpxchg
	push	{r4-r7, lr}
	mov	r4, r0
	mov	r5, r1
	mov	r6, r2
1:	ldr	r7, [r4]
	mov	r0, r7
	mov	r1, r5
	mov	r2, r4
	movw	r3, #0x0fc0
	movt	r3, #0xffff
	blx	r3
	cmp	r0, #0
	bne	1b
	mov	r5, r7
	subs	r6, r6, #1
	bne	1b
	mov	r0, r5
	pop	{r4-r7, pc}
	.endm

	func_arm at_swap_cmpxchg_arm
	body_swap_cmpxchg
	func_thumb at_swap_cmpxchg_thumb
	body_swap_cmpxchg
