/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared fixed-work harness for the targeted 32-bit FP/SIMD store probes.
 *
 * Each source supplies target_store_setup and target_store_body. The timed
 * region is exactly 5,000,000 repetitions of 32 stores, SUBS, and B.NE:
 * 160,000,000 target stores and 170,000,000 guest instructions in the loop.
 * Five samples are reported from CLOCK_MONOTONIC_RAW before the payload either
 * exits normally or powers off when it is serving as PID 1 in a minimal
 * Kernarium probe root.
 */

	.equ NR_WRITE,		64
	.equ NR_SYNC,		81
	.equ NR_EXIT_GROUP,	94
	.equ NR_CLOCK_GETTIME,	113
	.equ NR_REBOOT,		142
	.equ NR_GETPID,		172
	.equ CLOCK_MONOTONIC_RAW, 4
	.equ ITERATIONS,	5000000
	.equ SAMPLE_COUNT,	5

	.section .text
	.global _start
	.type _start, %function
_start:
	sub	sp, sp, #64
	target_store_setup
	mov	w22, #1

.Lsample_loop:
	mov	x0, #CLOCK_MONOTONIC_RAW
	mov	x1, sp
	mov	x8, #NR_CLOCK_GETTIME
	svc	#0
	cbnz	x0, .Lfailure

	movz	x19, #(ITERATIONS & 0xffff)
	movk	x19, #((ITERATIONS >> 16) & 0xffff), lsl #16
.Lstore_loop:
	target_store_body
	subs	x19, x19, #1
	b.ne	.Lstore_loop

	mov	x0, #CLOCK_MONOTONIC_RAW
	add	x1, sp, #16
	mov	x8, #NR_CLOCK_GETTIME
	svc	#0
	cbnz	x0, .Lfailure

	ldp	x9, x10, [sp]
	ldp	x11, x12, [sp, #16]
	sub	x11, x11, x9
	movz	x9, #0xca00
	movk	x9, #0x3b9a, lsl #16
	mul	x11, x11, x9
	sub	x12, x12, x10
	add	x20, x11, x12

	adrp	x9, sample_digit
	add	x9, x9, :lo12:sample_digit
	add	w10, w22, #48
	strb	w10, [x9]
	adrp	x9, elapsed_hex
	add	x9, x9, :lo12:elapsed_hex
	mov	x10, x20
	mov	w11, #16
.Lformat_elapsed:
	lsr	x12, x10, #60
	add	w12, w12, #48
	cmp	w12, #58
	b.lo	.Lstore_hex_digit
	add	w12, w12, #39
.Lstore_hex_digit:
	strb	w12, [x9], #1
	lsl	x10, x10, #4
	subs	w11, w11, #1
	b.ne	.Lformat_elapsed

	adrp	x1, success_message
	add	x1, x1, :lo12:success_message
	mov	x0, #1
	mov	x2, #(success_message_end - success_message)
	mov	x8, #NR_WRITE
	svc	#0

	add	w22, w22, #1
	cmp	w22, #(SAMPLE_COUNT + 1)
	b.ne	.Lsample_loop

	ldr	w9, [x13]
	cmp	w9, w23
	b.ne	.Lfailure
	ldr	w9, [x29]
	cmp	w9, w23
	b.ne	.Lfailure
	adrp	x1, completion_message
	add	x1, x1, :lo12:completion_message
	mov	x0, #1
	mov	x2, #(completion_message_end - completion_message)
	mov	x8, #NR_WRITE
	svc	#0
	mov	w20, #0
	b	.Lfinish

.Lfailure:
	adrp	x1, failure_message
	add	x1, x1, :lo12:failure_message
	mov	x0, #2
	mov	x2, #(failure_message_end - failure_message)
	mov	x8, #NR_WRITE
	svc	#0
	mov	w20, #1

.Lfinish:
	add	sp, sp, #64
	mov	x8, #NR_GETPID
	svc	#0
	cmp	x0, #1
	b.eq	.Lpoweroff
	mov	x0, x20
	mov	x8, #NR_EXIT_GROUP
	svc	#0
	brk	#0

.Lpoweroff:
	mov	x8, #NR_SYNC
	svc	#0
	movz	x0, #0xdead
	movk	x0, #0xfee1, lsl #16
	movz	x1, #0x1969
	movk	x1, #0x2812, lsl #16
	movz	x2, #0xfedc
	movk	x2, #0x4321, lsl #16
	mov	x3, xzr
	mov	x8, #NR_REBOOT
	svc	#0
	brk	#0

	.size _start, .-_start

	.section .rodata
completion_message:
	.ascii "OK\n"
completion_message_end:
failure_message:
	.ascii "FAIL\n"
failure_message_end:

	.section .data
	.p2align 14
data_page:
	.zero 1024

	.section .note.GNU-stack, "", %progbits
