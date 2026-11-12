// SPDX-License-Identifier: GPL-2.0-or-later
/* Group faults: one memory access per routine at the address in r0, with the
 * access first so that a handler resumes after it by skipping its size, then
 * bx lr. The routines that load store their result words through r1. */
	.syntax	unified
	.text

	.macro	fa_arm name, insn
	.arm
	.balign	4
	.globl	\name
	.type	\name, %function
\name:
	movw	r2, #0x1111
	movt	r2, #0x1111
	movw	r3, #0x2222
	movt	r3, #0x2222
\insn
	stm	r1, {r2, r3}
	bx	lr
	.endm

	.macro	fa_thumb name, insn
	.thumb
	.balign	4
	.thumb_func
	.globl	\name
	.type	\name, %function
\name:
	movw	r2, #0x1111
	movt	r2, #0x1111
	movw	r3, #0x2222
	movt	r3, #0x2222
	.globl	\name\()_insn
\name\()_insn:
\insn
	stm	r1, {r2, r3}
	bx	lr
	.endm

@ The instruction of an A32 routine is its fifth word (four movw/movt come
@ first), at the routine plus 16; each T32 routine exports <name>_insn.
	fa_arm	fa_ldrd_arm, "ldrd r2, r3, [r0]"
	fa_arm	fa_strd_arm, "strd r2, r3, [r0]"
	fa_arm	fa_ldm_arm, "ldm r0, {r2, r3}"
	fa_arm	fa_stm_arm, "stm r0, {r2, r3}"
	fa_arm	fa_ldrex_arm, "ldrex r2, [r0]"
	fa_arm	fa_ldrexh_arm, "ldrexh r2, [r0]"
	fa_arm	fa_ldrexb_arm, "ldrexb r2, [r0]"
	fa_arm	fa_ldrexd_arm, "ldrexd r2, r3, [r0]"
	fa_arm	fa_ldr_arm, "ldr r2, [r0]"
	fa_arm	fa_ldrh_arm, "ldrh r2, [r0]"
	fa_arm	fa_ldrsh_arm, "ldrsh r2, [r0]"
	fa_arm	fa_str_arm, "str r2, [r0]"
	fa_arm	fa_strh_arm, "strh r2, [r0]"

	fa_thumb fa_ldrd_thumb, "ldrd r2, r3, [r0]"
	fa_thumb fa_strd_thumb, "strd r2, r3, [r0]"
	fa_thumb fa_ldm_thumb, "ldm.w r0, {r2, r3}"
	fa_thumb fa_stm_thumb, "stm.w r0, {r2, r3}"
	fa_thumb fa_ldrex_thumb, "ldrex r2, [r0]"
	fa_thumb fa_ldrexh_thumb, "ldrexh r2, [r0]"
	fa_thumb fa_ldrexd_thumb, "ldrexd r2, r3, [r0]"
	fa_thumb fa_ldr_thumb, "ldr.w r2, [r0]"
	fa_thumb fa_ldrh_thumb, "ldrh.w r2, [r0]"
	fa_thumb fa_str_thumb, "str.w r2, [r0]"

@ ---- long fa_call_{arm,thumb}(address): blx address and return its r0 ----
	.arm
	.balign	4
	.globl	fa_call_arm
	.type	fa_call_arm, %function
fa_call_arm:
	push	{r4, lr}
	blx	r0
	pop	{r4, pc}

	.thumb
	.balign	4
	.thumb_func
	.globl	fa_call_thumb
	.type	fa_call_thumb, %function
fa_call_thumb:
	push	{r4, lr}
	blx	r0
	pop	{r4, pc}
