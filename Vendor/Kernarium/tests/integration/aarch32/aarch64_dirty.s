// SPDX-License-Identifier: GPL-2.0-or-later
//
// A static aarch64 program that leaves known values in x15-x30 on its
// virtual CPU, for aarch32_guest_probe.py, which assembles and links it in
// the guest with `as` and `ld -static`. The runtime enters an AArch32 task
// with R0-R14 only, so these registers are what a 64-bit task leaves behind
// for the 32-bit task that runs next on the same virtual CPU.
//
//   spin                  sets them before every sched_yield, forever
//   exec PROGRAM ARGS...  sets them and executes PROGRAM; exits 65 if
//                         execve fails
//   tokens                swaps a word and a byte of the page that
//                         descriptor 9 maps shared with LDAXR/STLXR pairs,
//                         against the program's SWP and SWPB ("swp-tokens"
//                         in aarch32_probe.S, which documents the page)
//                         until that program is done; exits 0, 3 if the
//                         program never came, 4 if the page did not map
//
// Any other use exits 64. Every value has both halves set.

	.text
	.globl	_start

.macro dirty
	ldr	x15, =0xa64d000f000f0bad
	ldr	x16, =0xa64d0010001020ad
	ldr	x17, =0xa64d0011001130ad
	ldr	x18, =0xa64d0012001240ad
	ldr	x19, =0xa64d0013001350ad
	ldr	x20, =0xa64d0014001460ad
	ldr	x21, =0xa64d0015001570ad
	ldr	x22, =0xa64d0016001680ad
	ldr	x23, =0xa64d0017001790ad
	ldr	x24, =0xa64d00180018a0ad
	ldr	x25, =0xa64d00190019b0ad
	ldr	x26, =0xa64d001a001ac0ad
	ldr	x27, =0xa64d001b001bd0ad
	ldr	x28, =0xa64d001c001ce0ad
	ldr	x29, =0xa64d001d001df0ad
	ldr	x30, =0xa64d001e001e00ad
.endm

_start:
	ldr	x9, [sp]		// argc
	add	x10, sp, #8		// argv
	cmp	x9, #2
	b.lt	usage
	ldr	x11, [x10, #8]		// argv[1]
	ldrb	w12, [x11]
	cmp	w12, #'s'
	b.eq	spin
	cmp	w12, #'e'
	b.eq	run
	cmp	w12, #'t'
	b.eq	tokens
usage:
	mov	x0, #64
	mov	x8, #94			// exit_group
	svc	#0

spin:
	dirty
	mov	x8, #124		// sched_yield
	svc	#0
	b	spin

run:
	cmp	x9, #3
	b.lt	usage
	add	x1, x10, #16		// &argv[2]
	ldr	x0, [x1]		// the program
	add	x2, x10, x9, lsl #3
	add	x2, x2, #8		// envp, after argv's NULL
	dirty
	mov	x8, #221		// execve
	svc	#0
	mov	x0, #65
	mov	x8, #94
	svc	#0

// The tokens: this task holds 2 and the other 1; the slots start at 4.
tokens:
	mov	x0, #0
	mov	x1, #4096
	mov	x2, #3			// PROT_READ | PROT_WRITE
	mov	x3, #1			// MAP_SHARED
	mov	x4, #9
	mov	x5, #0
	mov	x8, #222		// mmap
	svc	#0
	cmn	x0, #4096
	b.hi	tokens_nomap
	mov	x19, x0
	add	x1, x19, #4		// ready: wait for both tasks
1:	ldaxr	w2, [x1]
	add	w2, w2, #1
	stlxr	w3, w2, [x1]
	cbnz	w3, 1b
	ldr	x5, =200000
2:	ldar	w2, [x1]
	cmp	w2, #2
	b.hs	3f
	mov	x8, #124		// sched_yield
	svc	#0
	subs	x5, x5, #1
	b.ne	2b
	mov	x0, #3
	b	tokens_exit
3:	mov	w20, #2			// the word token
	mov	w21, #2			// the byte token
	mov	x22, #0			// the swaps
	add	x23, x19, #8		// the byte slot
	add	x24, x19, #40		// the done flag
4:	ldaxr	w1, [x19]
	stlxr	w2, w20, [x19]
	cbnz	w2, 4b
	mov	w20, w1
5:	ldaxrb	w1, [x23]
	stlxrb	w2, w21, [x23]
	cbnz	w2, 5b
	mov	w21, w1
	add	x22, x22, #1
	ldar	w1, [x24]
	cbz	w1, 4b
	str	w20, [x19, #24]
	str	w21, [x19, #44]
	str	x22, [x19, #48]
	mov	x0, #0
tokens_exit:
	mov	x8, #94
	svc	#0
tokens_nomap:
	mov	x0, #4
	b	tokens_exit

	.ltorg

	.section .note.GNU-stack,"",%progbits
