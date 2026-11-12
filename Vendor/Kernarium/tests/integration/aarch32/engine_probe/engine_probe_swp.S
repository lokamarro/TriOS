// SPDX-License-Identifier: GPL-2.0-or-later
/* Group swp: one SWP or SWPB per routine, at a label the tests compare with
 * the pc of a signal. SWP does not exist in T32, so every routine is A32.
 * The tests resume a faulting routine 4 bytes on, at its bx lr. */
	.syntax	unified
	.arm
	.text

	.macro	func name
	.balign	4
	.globl	\name
	.type	\name, %function
\name:
	.endm

@ ---- u32 sw_word(u32 *p, u32 v, u32 sentinel): swp r0, r1, [r3] ----
@ Returns what was at p, or the sentinel when the instruction raised a signal.
	func	sw_word
	mov	r3, r0
	mov	r0, r2
	.globl	sw_word_insn
sw_word_insn:
	swp	r0, r1, [r3]
	bx	lr

@ ---- u32 sw_byte(u8 *p, u32 v, u32 sentinel) ----
	func	sw_byte
	mov	r3, r0
	mov	r0, r2
	.globl	sw_byte_insn
sw_byte_insn:
	swpb	r0, r1, [r3]
	bx	lr

@ ---- u32 sw_same(u32 *p, u32 v): Rt == Rt2, the form CTS runs ----
	func	sw_same
	mov	r3, r0
	mov	r0, r1
	.globl	sw_same_insn
sw_same_insn:
	swp	r0, r0, [r3]
	bx	lr

@ ---- u32 sw_rn_is_rt(u32 *p, u32 v): Rn == Rt; the address is read first ----
	func	sw_rn_is_rt
	.globl	sw_rn_is_rt_insn
sw_rn_is_rt_insn:
	swp	r0, r1, [r0]
	bx	lr

@ ---- u32 sw_rn_is_rt2(u32 *p, u32 v): Rn == Rt2; the address is the data ----
@ r2 is Rt, so p receives p and r2 the old contents.
	func	sw_rn_is_rt2
	mov	r3, r0
	.globl	sw_rn_is_rt2_insn
sw_rn_is_rt2_insn:
	swp	r2, r3, [r3]
	mov	r0, r2
	bx	lr

@ ---- u32 sw_c_<cc>(u32 flags, u32 *p, u32 v, u32 sentinel) ----
@ Sets NZCV from the top four bits of r0, then runs swp<cc> r0, r2, [r1] with
@ r0 holding the sentinel: the result is the old contents, or the sentinel
@ when the condition failed.
	.macro	cond_swp cc
	func	sw_c_\cc
	msr	apsr_nzcvq, r0
	mov	r0, r3
	.globl	sw_c_\cc\()_insn
sw_c_\cc\()_insn:
	swp\cc	r0, r2, [r1]
	bx	lr
	.endm

	cond_swp eq
	cond_swp ne
	cond_swp cs
	cond_swp cc
	cond_swp mi
	cond_swp pl
	cond_swp vs
	cond_swp vc
	cond_swp hi
	cond_swp ls
	cond_swp ge
	cond_swp lt
	cond_swp gt
	cond_swp le
	cond_swp al

@ ---- u32 sw_cb_<cc>: the same for SWPB ----
	.macro	cond_swpb cc
	func	sw_cb_\cc
	msr	apsr_nzcvq, r0
	mov	r0, r3
	.globl	sw_cb_\cc\()_insn
sw_cb_\cc\()_insn:
	swpb\cc	r0, r2, [r1]
	bx	lr
	.endm

	cond_swpb eq
	cond_swpb ne

@ ---- void sw_u_<name>(u32 flags): words the kernel does not emulate ----
@ The flags are set first, so a condition can fail; the word raises SIGILL at
@ its label whatever they are.
	.macro	word_routine name, word
	func	\name
	msr	apsr_nzcvq, r0
	.globl	\name\()_insn
\name\()_insn:
	.inst	\word
	bx	lr
	.endm

	word_routine sw_u_pc_rt, 0xe102f091	@ swp pc, r1, [r2]
	word_routine sw_u_pc_rt2, 0xe102009f	@ swp r0, pc, [r2]
	word_routine sw_u_pc_rn, 0xe10f0091	@ swp r0, r1, [pc]
	word_routine sw_u_pcb_rt, 0xe142f091	@ swpb pc, r1, [r2]
	word_routine sw_u_pc_rt_ne, 0x1102f091	@ swpne pc, r1, [r2]
	word_routine sw_u_uncond, 0xf1020091	@ condition 0xf
	word_routine sw_u_sbz, 0xe1020191	@ bit 8 of the SBZ field set
	word_routine sw_u_sbz2, 0xe1020f91	@ bits 11:8 of the SBZ field set
	word_routine sw_u_bit20, 0xe1120091	@ bit 20 set: not a swap
