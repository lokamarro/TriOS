/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux workload for the host-side guest code sampler.
 * Two leaf functions run the same number of instructions per iteration with
 * different instruction words, and hot_loop runs nine times as many
 * iterations as cold_loop, so a correct profile gives it about nine tenths of
 * the guest samples. The thread pointer is set to THREAD_POINTER so the
 * profile can be checked for per-thread attribution.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT,		93
	.equ ROUNDS,		32
	.equ COLD_ITERATIONS,	1000000
	.equ HOT_ITERATIONS,	9000000
	.equ THREAD_POINTER,	0x5a5a0000

	.section .text
	.global _start
	.type _start, %function
_start:
	movz	x0, #(THREAD_POINTER >> 16), lsl #16
	msr	tpidr_el0, x0
	mov	x19, #ROUNDS
1:	bl	hot_loop
	bl	cold_loop
	subs	x19, x19, #1
	b.ne	1b
	mov	x0, #1
	adr	x1, message
	mov	x2, #3
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #0
	mov	x8, #NR_EXIT
	svc	#0
	.size _start, . - _start

	.global hot_loop
	.type hot_loop, %function
hot_loop:
	movz	x0, #(HOT_ITERATIONS & 0xffff)
	movk	x0, #(HOT_ITERATIONS >> 16), lsl #16
2:	add	x1, x1, #1
	add	x2, x2, #3
	eor	x3, x3, x1
	subs	x0, x0, #1
	b.ne	2b
	ret
	.size hot_loop, . - hot_loop

	.global cold_loop
	.type cold_loop, %function
cold_loop:
	movz	x0, #(COLD_ITERATIONS & 0xffff)
	movk	x0, #(COLD_ITERATIONS >> 16), lsl #16
3:	add	x4, x4, #5
	add	x5, x5, #7
	orr	x6, x6, x4
	subs	x0, x0, #1
	b.ne	3b
	ret
	.size cold_loop, . - cold_loop

	.section .rodata
message:
	.ascii	"OK\n"
