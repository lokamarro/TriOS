// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Entry and state-transfer code of the AArch32 corpus oracle
 * (a32_oracle.cpp), a freestanding armhf program.
 *
 * a32_run() loads a complete AArch32 user state, jumps to the test program
 * and, when the program reaches one of the exit stubs, stores the state it
 * finds and returns. The test owns every register, so neither the launch nor
 * the epilogue may use one: the launch ends in a direct branch to
 * A32_LAUNCH, an `ldr pc, [pc, #-4]` the C++ side writes into the code
 * region, and the epilogue saves each register with PC-relative stores before
 * it clobbers r0. The exit stubs in the code region jump to a32_exit_arm and
 * a32_exit_thumb, which record the instruction set the program finished in.
 *
 * The program image is one writable and executable segment (a32_oracle.ld),
 * so the save area and the saved stack pointer live beside the code.
 * State layout (see struct State in a32_oracle.cpp): r0-r14 at 0, APSR at
 * 60, FPSCR at 64, TPIDRURO at 68, TPIDRURW at 72, D0-D31 at 80.
 */

	.syntax	unified
	.arch	armv8-a
	.fpu	crypto-neon-fp-armv8
	.arm
	.text

	.equ	OFF_SP, 52
	.equ	OFF_LR, 56
	.equ	OFF_APSR, 60
	.equ	OFF_FPSCR, 64
	.equ	OFF_TPIDRURW, 72
	.equ	OFF_D, 80

	.global	_start
	.type	_start, %function
_start:
	mov	r0, sp
	bl	oracle_main
	mov	r7, #1
	svc	#0

	/* a32_run(const State *in): never returns until the program ends. */
	.global	a32_run
	.type	a32_run, %function
a32_run:
	push	{r4-r11, lr}
	ldr	r2, =a32_saved_sp
	str	sp, [r2]
	mov	r12, r0
	ldr	r0, [r12, #OFF_APSR]
	msr	APSR_nzcvqg, r0
	ldr	r0, [r12, #OFF_FPSCR]
	vmsr	fpscr, r0
	ldr	r0, [r12, #OFF_TPIDRURW]
	mcr	p15, 0, r0, c13, c0, 2
	add	r0, r12, #OFF_D
	vldmia	r0!, {d0-d15}
	vldmia	r0, {d16-d31}
	ldmia	r12, {r0-r11}
	ldr	lr, [r12, #OFF_LR]
	ldr	sp, [r12, #OFF_SP]
	ldr	r12, [r12, #48]
	b	A32_LAUNCH
	.ltorg

	/* The exit stubs land here, in the program's final state. Each entry
	   records the instruction set the program finished in; r0 and r1 are
	   the only registers the epilogue overwrites and it saves them first. */
	.global	a32_exit_arm
	.type	a32_exit_arm, %function
a32_exit_arm:
1:	str	r1, [pc, #(a32_save_area + 4 - 1b - 8)]
	mov	r1, #0
	b	a32_exit_common
	.global	a32_exit_thumb
	.type	a32_exit_thumb, %function
a32_exit_thumb:
2:	str	r1, [pc, #(a32_save_area + 4 - 2b - 8)]
	mov	r1, #1
a32_exit_common:
3:	str	r1, [pc, #(a32_exit_kind - 3b - 8)]
4:	ldr	r1, [pc, #(a32_save_area + 4 - 4b - 8)]
5:	str	r0, [pc, #(a32_save_area - 5b - 8)]
	adr	r0, a32_save_area
	stmib	r0, {r1-r12}
	str	sp, [r0, #OFF_SP]
	str	lr, [r0, #OFF_LR]
	mrs	r1, apsr
	str	r1, [r0, #OFF_APSR]
	vmrs	r1, fpscr
	str	r1, [r0, #OFF_FPSCR]
	mrc	p15, 0, r1, c13, c0, 2
	str	r1, [r0, #OFF_TPIDRURW]
	add	r1, r0, #OFF_D
	vstmia	r1!, {d0-d15}
	vstmia	r1, {d16-d31}
	ldr	r1, =a32_saved_sp
	ldr	sp, [r1]
	pop	{r4-r11, pc}
	.ltorg

	/* Signal handlers redirect a faulting program here with the stack
	   pointer set to the one a32_run saved. */
	.global	a32_resume
	.type	a32_resume, %function
a32_resume:
	pop	{r4-r11, pc}

	/* Read the TPIDRURW register (a signal handler cannot see the
	   program's). */
	.global	a32_read_tpidrurw
	.type	a32_read_tpidrurw, %function
a32_read_tpidrurw:
	mrc	p15, 0, r0, c13, c0, 2
	bx	lr

	/* The signal return trampoline (SA_RESTORER). */
	.global	a32_sigreturn
	.type	a32_sigreturn, %function
a32_sigreturn:
	mov	r7, #173
	svc	#0

	.balign	8
	.global	a32_save_area
a32_save_area:
	.space	336
	.global	a32_exit_kind
a32_exit_kind:
	.word	0
	.global	a32_saved_sp
a32_saved_sp:
	.word	0
